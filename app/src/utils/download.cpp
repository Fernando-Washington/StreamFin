#include "utils/download.hpp"
#include "utils/config.hpp"
#include "utils/thread.hpp"
#include "utils/misc.hpp"
#include "api/http.hpp"

#include <borealis/core/thread.hpp>
#include <curl/curl.h>

int64_t remoteContentLength(const std::string& url) {
    CURL* easy = curl_easy_init();
    if (!easy) return -1;

    curl_easy_setopt(easy, CURLOPT_URL, url.c_str());
    curl_easy_setopt(easy, CURLOPT_NOBODY, 1L);
    curl_easy_setopt(easy, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(easy, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(easy, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(easy, CURLOPT_NOSIGNAL, 1L);
    // Some debrid/CDN endpoints take a while just to complete the TLS
    // handshake on the Switch -- 15s was cutting that off mid-handshake
    // (surfaces as "SSL connect error", not a clean timeout) and blocking
    // downloads from ever starting, since this size check runs before the
    // user even sees the confirmation dialog.
    curl_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT, 45L);
    curl_easy_setopt(easy, CURLOPT_TIMEOUT, 45L);

    curl_off_t len = -1;
    int64_t result = -1;
    if (curl_easy_perform(easy) == CURLE_OK &&
        curl_easy_getinfo(easy, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &len) == CURLE_OK && len > 0) {
        result = (int64_t)len;
    }
    curl_easy_cleanup(easy);
    return result;
}

int64_t downloadDiskFreeBytes() {
    try {
        return (int64_t)fs::space(AppConfig::instance().configDir()).available;
    } catch (const std::exception& e) {
        brls::Logger::error("downloadDiskFreeBytes: {}", e.what());
        return -1;
    }
}

int64_t downloadDiskTotalBytes() {
    try {
        return (int64_t)fs::space(AppConfig::instance().configDir()).capacity;
    } catch (const std::exception& e) {
        brls::Logger::error("downloadDiskTotalBytes: {}", e.what());
        return -1;
    }
}

// Stream ids look like "tt1234567:1:2" (season:episode) -- ':' (and a few
// other characters some filesystems reject) can't be a path component on the
// SD card's FAT32/exFAT, which surfaced as a hard filesystem error creating
// the episode's download folder. `id` itself stays untouched (still used for
// equality checks against ResumeEntry etc.); only this on-disk folder name
// needs to be filesystem-safe.
static std::string sanitizeForPath(const std::string& id) {
    std::string out = id;
    for (char& c : out) {
        if (c == ':' || c == '/' || c == '\\' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|')
            c = '_';
    }
    return out;
}

std::string DownloadManager::downloadDir() const { return AppConfig::instance().configDir() + "/downloads"; }

void DownloadManager::init() {
    auto dir = this->downloadDir();
    if (!fs::exists(dir)) {
        fs::create_directories(dir);
    }
    this->loadIndex();

    std::lock_guard<std::mutex> lock(this->mutex);
    for (auto& item : this->items) {
        if (item.status == DownloadStatus::Downloading) {
            item.status = DownloadStatus::Queued;
        }
    }
    this->saveIndex();
    this->processQueue();
}

void DownloadManager::loadIndex() {
    std::string path = this->downloadDir() + "/index.json";
    if (!fs::exists(path)) return;

    try {
        std::ifstream f(path);
        nlohmann::json j = nlohmann::json::parse(f);
        this->items = j.get<std::vector<DownloadItem>>();
    } catch (const std::exception& e) {
        brls::Logger::error("Failed to load download index: {}", e.what());
    }
}

void DownloadManager::saveIndex() {
    std::string path = this->downloadDir() + "/index.json";
    try {
        nlohmann::json j = this->items;
        std::ofstream f(path);
        f << j.dump(2);
    } catch (const std::exception& e) {
        brls::Logger::error("Failed to save download index: {}", e.what());
    }
}

void DownloadManager::addDownload(const DownloadItem& item) {
    std::lock_guard<std::mutex> lock(this->mutex);

    for (auto& existing : this->items) {
        if (existing.id == item.id) {
            brls::Logger::info("Already exists: {}", item.id);
            return;
        }
    }

    this->items.push_back(item);
    this->items.back().status = DownloadStatus::Queued;
    this->items.back().downloadedBytes = 0;
    this->saveIndex();
    brls::Logger::info("Download queued: {}", item.name);
    this->processQueue();
}

void DownloadManager::resumeQueue() {
    std::lock_guard<std::mutex> lock(this->mutex);
    this->processQueue();
}

void DownloadManager::cancelDownload(const std::string& id) {
    bool erased = false;
    {
        std::lock_guard<std::mutex> lock(this->mutex);

        for (auto& item : this->items) {
            if (item.id == id && item.status == DownloadStatus::Downloading && this->currentCancel) {
                this->currentCancel->store(true);
                return;
            }
        }

        for (auto it = this->items.begin(); it != this->items.end(); ++it) {
            if (it->id == id && it->status == DownloadStatus::Queued) {
                this->items.erase(it);
                this->saveIndex();
                erased = true;
                break;
            }
        }
    }

    if (erased) {
        brls::sync([this, id]() { this->statusEvent.fire(id, DownloadStatus::Failed); });
    }
}

void DownloadManager::removeDownload(const std::string& id) {
    bool wasActive = false;
    bool erased = false;
    {
        std::lock_guard<std::mutex> lock(this->mutex);

        for (auto& item : this->items) {
            if (item.id == id && item.status == DownloadStatus::Downloading && this->currentCancel) {
                this->currentCancel->store(true);
                item.errorMessage = "removed";
                wasActive = true;
                break;
            }
        }

        if (!wasActive) {
            for (auto it = this->items.begin(); it != this->items.end(); ++it) {
                if (it->id == id) {
                    this->items.erase(it);
                    erased = true;
                    break;
                }
            }
            this->saveIndex();
        }
    }

    if (!wasActive) {
        std::string dir = this->downloadDir() + "/" + sanitizeForPath(id);
        brls::async([dir]() {
            try {
                if (fs::exists(dir)) fs::remove_all(dir);
            } catch (const std::exception& e) {
                brls::Logger::error("Failed to remove download dir: {}", e.what());
            }
        });
    }

    if (erased) {
        brls::sync([this, id]() { this->statusEvent.fire(id, DownloadStatus::Failed); });
    }
}

DownloadStatus DownloadManager::findItem(const std::string& id) const {
    std::lock_guard<std::mutex> lock(this->mutex);
    for (auto& item : this->items) {
        if (item.id == id) return item.status;
    }
    return DownloadStatus::NotFound;
}

std::pair<size_t, size_t> DownloadManager::findSeries(const std::string& seriesId) const {
    std::lock_guard<std::mutex> lock(this->mutex);
    size_t count = 0, done = 0;
    for (auto& item : this->items) {
        if (item.seriesId == seriesId) {
            if (item.status == DownloadStatus::Completed) done++;
            ++count;
        }
    }
    return std::make_pair(count, done);
}

std::string DownloadManager::getLocalPath(const std::string& id) const {
    std::lock_guard<std::mutex> lock(this->mutex);
    for (auto& item : this->items) {
        if (item.id == id && item.status == DownloadStatus::Completed) {
            return this->downloadDir() + "/" + sanitizeForPath(id) + "/" + item.filePath;
        }
    }
    return "";
}

std::vector<DownloadItem> DownloadManager::getItems() const {
    std::lock_guard<std::mutex> lock(this->mutex);
    return this->items;
}

int64_t DownloadManager::totalOwnBytes() const {
    std::lock_guard<std::mutex> lock(this->mutex);
    int64_t sum = 0;
    for (auto& item : this->items)
        if (item.status == DownloadStatus::Completed)
            sum += item.downloadedBytes > 0 ? item.downloadedBytes : item.totalBytes;
    return sum;
}

// Must be called with mutex held
void DownloadManager::processQueue() {
    if (this->downloading) return;

    for (auto& item : this->items) {
        if (item.status == DownloadStatus::Queued) {
            this->downloading = true;
            this->doDownload(item);
            return;
        }
    }
}

// Must be called with mutex held. Copies what it needs, then releases via async.
void DownloadManager::doDownload(DownloadItem& item) {
    item.status = DownloadStatus::Downloading;

    std::string id = item.id;
    std::string poster = item.poster;
    std::string url = item.sourceUrl;
    int64_t knownTotal = item.totalBytes;
    std::string itemDir = this->downloadDir() + "/" + sanitizeForPath(id);

    this->saveIndex();

    auto cancel = std::make_shared<std::atomic_bool>(false);
    this->currentCancel = cancel;

    brls::sync([this, id]() { this->statusEvent.fire(id, DownloadStatus::Downloading); });

    ThreadPool::instance().submit([this, id, poster, url, itemDir, knownTotal, cancel](HTTP&) {
        auto resetQueue = [this, id](const std::string& error) {
            brls::sync([this, id, error]() {
                {
                    std::lock_guard<std::mutex> lock(this->mutex);
                    for (auto& item : this->items) {
                        if (item.id == id) {
                            item.status = DownloadStatus::Failed;
                            item.errorMessage = error;
                            break;
                        }
                    }
                    this->downloading = false;
                    this->currentCancel.reset();
                    this->saveIndex();
                }
                this->statusEvent.fire(id, DownloadStatus::Failed);
                {
                    std::lock_guard<std::mutex> lock(this->mutex);
                    this->processQueue();
                }
            });
        };

        try {
            if (!fs::exists(itemDir)) fs::create_directories(itemDir);
        } catch (const std::exception& e) {
            brls::Logger::error("Failed to create download dir: {}", e.what());
            resetQueue(e.what());
            return;
        }

        if (cancel->load()) {
            resetQueue("Cancelled");
            return;
        }

        // Debrid/CDN links carry no meaningful extension in the path; mp4 is
        // what practically everything Stremio serves ends up being.
        std::string fileName = "video.mp4";
        std::string filePath = itemDir + "/" + fileName;

        {
            std::lock_guard<std::mutex> lock(this->mutex);
            for (auto& it : this->items) {
                if (it.id == id) {
                    it.filePath = fileName;
                    break;
                }
            }
            this->saveIndex();
        }

        if (!poster.empty() && !cancel->load()) {
            try {
                HTTP::download(poster, itemDir + "/thumb.jpg", HTTP::Timeout{});
            } catch (const std::exception& e) {
                fs::remove(itemDir + "/thumb.jpg");
                brls::Logger::warning("Failed to download poster: {}", e.what());
            }
        }

        auto lastProgress = std::make_shared<std::chrono::steady_clock::time_point>();
        HTTP::Progress::Callback progressCb = [this, id, knownTotal, lastProgress](curl_off_t total, curl_off_t now) {
            auto tp = std::chrono::steady_clock::now();
            if (tp - *lastProgress < std::chrono::seconds(1)) return;
            *lastProgress = tp;

            // Some debrid links don't report Content-Length on the streaming
            // GET (only on the HEAD we did up front) -- fall back to that.
            int64_t reportedTotal = total > 0 ? (int64_t)total : knownTotal;

            brls::sync([this, id, reportedTotal, now]() {
                {
                    std::lock_guard<std::mutex> lock(this->mutex);
                    for (auto& item : this->items) {
                        if (item.id == id) {
                            item.totalBytes = reportedTotal;
                            item.downloadedBytes = now;
                            break;
                        }
                    }
                }
                this->progressEvent.fire(id, now, reportedTotal);
            });
        };

        bool cancelled = false;
        bool success = false;
        std::string error;

        try {
            std::ofstream of(filePath, std::ios::binary);
            if (!of) throw std::runtime_error("Failed to open file for writing");

            HTTP s;
            HTTP::set_option(s, cancel, progressCb);
            s._get(url, &of);
            of.close();

            cancelled = cancel->load();
            if (!cancelled) {
                // A debrid/CDN link that's expired or rejected the request
                // can still answer with 200 and a tiny error/redirect body --
                // no curl error, no HTTP error status, just a few hundred
                // bytes written where a movie should be. Catch that instead
                // of quietly marking a corrupt file "Completed".
                int64_t actualSize = 0;
                try {
                    actualSize = (int64_t)fs::file_size(filePath);
                } catch (const std::exception&) {
                }
                int64_t expectedMin = knownTotal > 0 ? (int64_t)(knownTotal * 0.9) : (1 * 1024 * 1024);
                if (actualSize < expectedMin) {
                    throw std::runtime_error(
                        fmt::format("Incomplete download ({} bytes) -- the link may have expired", actualSize));
                }
                success = true;
            }
        } catch (const std::exception& ex) {
            error = ex.what();
            brls::Logger::error("Download failed: {} - {}", id, error);
        }

        brls::sync([this, id, fileName, cancelled, success, error]() {
            DownloadStatus finalStatus = DownloadStatus::Failed;

            {
                std::lock_guard<std::mutex> lock(this->mutex);

                if (cancelled) {
                    bool removed = false;
                    for (auto it = this->items.begin(); it != this->items.end(); ++it) {
                        if (it->id == id) {
                            if (it->errorMessage == "removed") {
                                this->items.erase(it);
                                removed = true;
                            } else {
                                it->status = DownloadStatus::Failed;
                                it->errorMessage = "Cancelled";
                            }
                            break;
                        }
                    }
                    this->saveIndex();
                    if (removed) {
                        std::string dir = this->downloadDir() + "/" + sanitizeForPath(id);
                        brls::async([dir]() {
                            try {
                                if (fs::exists(dir)) fs::remove_all(dir);
                            } catch (const std::exception& e) {
                                brls::Logger::error("Failed to remove download dir: {}", e.what());
                            }
                        });
                    }
                } else if (success) {
                    finalStatus = DownloadStatus::Completed;
                    for (auto& item : this->items) {
                        if (item.id == id) {
                            item.status = DownloadStatus::Completed;
                            item.filePath = fileName;
                            break;
                        }
                    }
                    this->saveIndex();
                    brls::Logger::info("Download completed: {}", id);
                } else {
                    for (auto& item : this->items) {
                        if (item.id == id) {
                            item.status = DownloadStatus::Failed;
                            item.errorMessage = error;
                            break;
                        }
                    }
                    this->saveIndex();
                }

                this->downloading = false;
                this->currentCancel.reset();
            }

            this->statusEvent.fire(id, finalStatus);
            {
                std::lock_guard<std::mutex> lock(this->mutex);
                this->processQueue();
            }
        });
    });
}
