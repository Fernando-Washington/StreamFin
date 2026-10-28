#include "view/curl_stream.hpp"

#include <mpv/client.h>
#include <mpv/stream_cb.h>
#include <curl/curl.h>

#include <borealis/core/logger.hpp>
#include <fmt/format.h>

#include <algorithm>
#include <atomic>
#include <deque>
#include <string>

namespace {

// One instance per stream mpv opens (the main video, plus any http subtitle
// track). All callbacks below run on mpv's demuxer thread and are expected
// to block -- there's no async handoff needed, curl is just pumped in place.
struct CurlStream {
    CURLM* multi = nullptr;
    CURL* easy = nullptr;
    std::string url;
    std::deque<char> buffer;
    int64_t pos = 0;         // absolute stream offset of the next unread byte
    int64_t totalSize = -1;  // captured once, from the very first (offset-0) request
    bool sizeKnown = false;
    bool transferDone = false;
    bool transferError = false;
    int retriesLeft = 2;  // network hiccups get a couple of silent reconnects
    // Diagnostic snapshot of the last failed attempt -- filled right before
    // giving up (transferError = true) so the final log line can say WHY,
    // instead of the bare "request failed" that gave no way to tell a real
    // TLS handshake failure apart from an expired/rejected link.
    char errorBuffer[256] = {0};
    CURLcode lastResult = CURLE_OK;
    long lastHttpCode = 0;
    double lastConnectTime = 0;
    double lastAppConnectTime = 0;

    void recordFailure(CURLcode result, long httpCode) {
        lastResult = result;
        lastHttpCode = httpCode;
        lastConnectTime = 0;
        lastAppConnectTime = 0;
        if (easy) {
            curl_easy_getinfo(easy, CURLINFO_CONNECT_TIME, &lastConnectTime);
            curl_easy_getinfo(easy, CURLINFO_APPCONNECT_TIME, &lastAppConnectTime);
        }
    }

    std::string describeFailure() const {
        std::string s;
        if (lastResult != CURLE_OK) {
            s = curl_easy_strerror(lastResult);
            if (errorBuffer[0] != '\0') s += fmt::format(" [{}]", errorBuffer);
        } else if (lastHttpCode >= 400) {
            s = fmt::format("http status {}", lastHttpCode);
        } else {
            s = "unknown";
        }
        s += fmt::format(" (tcp_connect={:.2f}s tls_handshake={:.2f}s, retries_used={}/2)", lastConnectTime,
            lastAppConnectTime, 2 - retriesLeft);
        return s;
    }
    // Set from mpv's cancel_fn, which runs on a DIFFERENT thread than the one
    // blocked in pumpUntil below. Without this, mpv has no way to interrupt a
    // stuck reconnect loop (e.g. a slow/stalled TLS handshake) -- the demuxer
    // thread just keeps retrying until it gives up on its own, and anything
    // that needs that thread to be free (backing out of playback, loading a
    // different file next) hangs until it does. That's what an "infinite
    // spinner" until a full app restart looks like from the outside.
    std::atomic<bool> cancelled { false };

    ~CurlStream() { closeEasy(); }

    static size_t writeCb(char* ptr, size_t size, size_t nmemb, void* userdata) {
        auto* self = static_cast<CurlStream*>(userdata);
        size_t n = size * nmemb;
        self->buffer.insert(self->buffer.end(), ptr, ptr + n);
        self->retriesLeft = 2;  // any real progress resets the reconnect budget
        return n;
    }

    void closeEasy() {
        if (multi && easy) curl_multi_remove_handle(multi, easy);
        if (easy) curl_easy_cleanup(easy);
        if (multi) curl_multi_cleanup(multi);
        easy = nullptr;
        multi = nullptr;
    }

    // (Re)starts the transfer at absolute offset `from`, discarding whatever
    // was buffered for the previous request. Used both for the initial open
    // and for every seek (there's no other way to jump around an HTTP
    // stream: a seek is just a fresh GET with a Range header).
    bool startFrom(int64_t from) {
        closeEasy();
        buffer.clear();
        transferDone = false;
        transferError = false;
        pos = from;

        easy = curl_easy_init();
        multi = curl_multi_init();
        if (!easy || !multi) {
            closeEasy();
            return false;
        }

        curl_easy_setopt(easy, CURLOPT_URL, url.c_str());
        curl_easy_setopt(easy, CURLOPT_FOLLOWLOCATION, 1L);
        // Same trust posture as the app's own HTTP client (api/http.cpp) --
        // debrid/CDN endpoints, not something we pin certs for.
        curl_easy_setopt(easy, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(easy, CURLOPT_SSL_VERIFYHOST, 0L);
        errorBuffer[0] = '\0';
        curl_easy_setopt(easy, CURLOPT_ERRORBUFFER, errorBuffer);
        curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, &CurlStream::writeCb);
        curl_easy_setopt(easy, CURLOPT_WRITEDATA, this);
        curl_easy_setopt(easy, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(easy, CURLOPT_ACCEPT_ENCODING, "");
        // A dead/stalled connection (Wi-Fi drop, CDN hiccup) must fail
        // instead of blocking read_fn forever -- that's what was forcing a
        // full app restart before. But 15s turned out to be too tight for
        // the connect phase itself: some debrid/CDN endpoints genuinely take
        // a while to complete the TLS handshake on the Switch, and cutting
        // that off mid-handshake surfaces as "SSL connect error" instead of
        // a clean timeout -- it was blocking playback from starting at all,
        // not just recovering from real stalls. Give the handshake up to 45s;
        // once actually connected and transferring, 30s with near-zero
        // throughput still means something is genuinely stuck.
        curl_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT, 45L);
        curl_easy_setopt(easy, CURLOPT_LOW_SPEED_LIMIT, 1L);
        curl_easy_setopt(easy, CURLOPT_LOW_SPEED_TIME, 30L);
        if (from > 0) curl_easy_setopt(easy, CURLOPT_RESUME_FROM_LARGE, (curl_off_t)from);

        curl_multi_add_handle(multi, easy);
        return true;
    }

    // Pumps the transfer until at least `want` bytes are buffered or the
    // transfer ends (success or failure). Returns false only on a real,
    // unrecoverable error; running out of data at EOF is not an error.
    //
    // A Wi-Fi drop or CDN hiccup (the low-speed/connect timeouts above
    // firing, or curl reporting a transport error) reconnects from the
    // current position instead of failing outright -- this is what used to
    // force a full app restart. The retry budget resets on any real
    // progress, so it's "a couple of consecutive failures", not a hard cap
    // for the whole playback.
    bool pumpUntil(size_t want) {
        while (buffer.size() < want && !transferDone) {
            if (cancelled.load()) {
                transferError = true;
                break;
            }
            int stillRunning = 0;
            if (curl_multi_perform(multi, &stillRunning) != CURLM_OK) {
                recordFailure(CURLE_OK, 0);
                if (retriesLeft > 0) {
                    retriesLeft--;
                    if (startFrom(pos)) continue;
                }
                transferError = true;
                break;
            }

            if (stillRunning == 0) {
                bool failed = false;
                int msgsLeft = 0;
                CURLMsg* msg = curl_multi_info_read(multi, &msgsLeft);
                if (msg && msg->msg == CURLMSG_DONE) {
                    long code = 0;
                    curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &code);
                    failed = msg->data.result != CURLE_OK || code >= 400;
                    if (failed) recordFailure(msg->data.result, code);
                }
                if (failed) {
                    if (retriesLeft > 0) {
                        retriesLeft--;
                        brls::Logger::warning("curl_stream: connection dropped ({}), reconnecting from offset {}",
                            describeFailure(), pos);
                        if (startFrom(pos)) continue;
                    }
                    transferError = true;
                }
                transferDone = true;
                break;
            }

            // Block briefly on socket activity instead of busy-looping the
            // demuxer thread while waiting for more bytes.
            int numfds = 0;
            curl_multi_wait(multi, nullptr, 0, 200, &numfds);
        }
        return !transferError;
    }
};

int64_t readCb(void* cookie, char* buf, uint64_t nbytes) {
    auto* self = static_cast<CurlStream*>(cookie);
    self->pumpUntil((size_t)nbytes);
    if (self->buffer.empty()) {
        // A mid-playback failure (after the stream had already opened fine)
        // used to fail completely silently here -- no log line at all,
        // just mpv reporting a generic decode/EOF stall to the user.
        if (self->transferError)
            brls::Logger::error("curl_stream: mid-playback read failed for {} -- {}", self->url, self->describeFailure());
        return (self->transferDone && !self->transferError) ? 0 : -1;
    }

    size_t n = std::min<size_t>((size_t)nbytes, self->buffer.size());
    std::copy(self->buffer.begin(), self->buffer.begin() + n, buf);
    self->buffer.erase(self->buffer.begin(), self->buffer.begin() + n);
    self->pos += (int64_t)n;
    return (int64_t)n;
}

int64_t seekCb(void* cookie, int64_t offset) {
    auto* self = static_cast<CurlStream*>(cookie);
    if (offset == self->pos) return offset;
    if (!self->startFrom(offset)) return MPV_ERROR_GENERIC;
    return offset;
}

int64_t sizeCb(void* cookie) {
    auto* self = static_cast<CurlStream*>(cookie);
    return self->sizeKnown ? self->totalSize : MPV_ERROR_UNSUPPORTED;
}

void closeCb(void* cookie) { delete static_cast<CurlStream*>(cookie); }

// Called by mpv from a thread OTHER than the demuxer thread that's blocked in
// readCb/pumpUntil -- must not block itself. Just flips the flag; pumpUntil's
// loop picks it up within one 200ms curl_multi_wait tick at the latest.
void cancelCb(void* cookie) {
    static_cast<CurlStream*>(cookie)->cancelled.store(true);
}

int openCb(void* user_data, char* uri, mpv_stream_cb_info* info) {
    auto* self = new CurlStream();
    self->url = uri;
    if (!self->startFrom(0)) {
        brls::Logger::error("curl_stream: failed to init curl for {}", uri);
        delete self;
        return MPV_ERROR_LOADING_FAILED;
    }

    // Force the response headers to arrive now so the total size (if the
    // server sends one) is known up front -- this is the ONLY point we read
    // it from, since a later Range request's Content-Length reflects the
    // remaining bytes, not the full file.
    //
    // KNOWN LIMITATION: this first pumpUntil() runs before info->cancel_fn is
    // assigned below -- mpv can't hand out a cancel_fn for a stream that
    // hasn't finished opening yet, since open_fn hasn't returned a cookie
    // yet. So backing out of playback during the very first connect/TLS
    // handshake still has to wait out the connect timeout; cancel_fn only
    // helps once the stream is already open (reconnects, seeks, later
    // reads). Fixing this would mean making the open itself non-blocking
    // (e.g. run it on its own thread), which is a bigger change than
    // tonight's scope.
    self->pumpUntil(1);
    if (self->transferError) {
        brls::Logger::error("curl_stream: request failed for {} -- {}", uri, self->describeFailure());
        delete self;
        return MPV_ERROR_LOADING_FAILED;
    }
    curl_off_t len = -1;
    if (self->easy && curl_easy_getinfo(self->easy, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &len) == CURLE_OK && len > 0) {
        self->totalSize = (int64_t)len;
        self->sizeKnown = true;
    }

    info->cookie = self;
    info->read_fn = &readCb;
    info->seek_fn = &seekCb;
    info->size_fn = &sizeCb;
    info->close_fn = &closeCb;
    info->cancel_fn = &cancelCb;
    return 0;
}

}  // namespace

void registerCurlHttpsStream(mpv_handle* mpv) {
    mpv_stream_cb_add_ro(mpv, "https", nullptr, &openCb);
    mpv_stream_cb_add_ro(mpv, "http", nullptr, &openCb);
}
