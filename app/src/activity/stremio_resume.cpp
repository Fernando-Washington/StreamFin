/*
    Stremio resume / continue-watching -- implementation.
*/
#include "activity/stremio_resume.hpp"
#include "view/mpv_core.hpp"
#include "utils/config.hpp"
#include "utils/watched.hpp"
#include "utils/smart_downloads.hpp"

#include <fstream>
#include <sys/stat.h>
#include <algorithm>
#include <ctime>

ResumeTracker& ResumeTracker::instance() {
    static ResumeTracker t;
    return t;
}

void ResumeTracker::init() {
    if (this->inited) return;
    this->inited = true;
    this->load();

    MPVCore::instance().getEvent()->subscribe([this](MpvEventEnum e) {
        auto& mpv = MPVCore::instance();
        switch (e) {
        case MpvEventEnum::MPV_LOADED:
            // Resume: jump to the saved spot once the file is loaded.
            if (this->hasCurrent && this->pendingSeek > 10) mpv.seek(this->pendingSeek, "absolute");
            this->pendingSeek = 0;
            break;
        case MpvEventEnum::UPDATE_PROGRESS: {
            if (!this->hasCurrent) break;
            long now = (long)time(nullptr);
            if (now - this->lastTick >= 20) {  // save at most every 20s
                this->lastTick = now;
                this->onProgress(mpv.playback_time, mpv.duration, false);
            }
            break;
        }
        case MpvEventEnum::MPV_STOP:
        case MpvEventEnum::END_OF_FILE:
        case MpvEventEnum::MPV_FILE_ERROR:
            // MPV_FILE_ERROR isn't just "never started" -- a dropped
            // connection well into playback ends the same way (mpv_core.cpp's
            // MPV_END_FILE_REASON_ERROR branch), and that position is worth
            // keeping. onProgress()'s own `pos < 60` guard already covers the
            // "never really played" case (mpv.playback_time is reset to 0 for
            // every new title by MPVCore::reset(), so a source that fails
            // before ever producing a real position can't wrongly inherit a
            // previous title's leftover time here).
            if (this->hasCurrent) this->onProgress(mpv.playback_time, mpv.duration, true);
            this->hasCurrent = false;
            break;
        default:
            break;
        }
    });
}

void ResumeTracker::setCurrent(const ResumeEntry& e) {
    this->current = e;
    this->hasCurrent = true;
    this->lastTick = 0;
    this->pendingSeek = 0;
    for (auto& it : this->items)
        if (it.streamId == e.streamId) {
            this->pendingSeek = it.position;
            break;
        }
}

namespace {
// Series episodes routinely have a minute or more of credits (sometimes with
// a post-credits scene) after the part someone actually came to watch --
// counting that against "watched" would mean the checkmark/Smart Downloads
// never fire until the credits finish rolling. Movies don't get the same
// early cutoff: they're not part of Smart Downloads' delete-and-queue-next
// flow, and a stray "watched" checkmark has no UI to show up in anyway, so
// there's no upside to the risk of calling a movie done too early.
constexpr double SERIES_WATCHED_THRESHOLD = 0.80;
// Regardless of type, this is when an entry stops needing to be resumable at
// all and drops out of Continue Watching.
constexpr double FINISHED_THRESHOLD = 0.92;
}  // namespace

void ResumeTracker::onProgress(double pos, double dur, bool fire) {
    if (pos < 60) return;  // ignore < 1 minute watched
    bool hasDuration = dur > 0;
    double ratio = hasDuration ? pos / dur : 0;
    bool finished = hasDuration && ratio >= FINISHED_THRESHOLD;
    bool watched = finished ||
        (this->current.streamType == "series" && hasDuration && ratio >= SERIES_WATCHED_THRESHOLD);

    // "Watched" (checkmark, Smart Downloads) and "still resumable" (Continue
    // Watching) are independent on purpose: crossing 80% of an episode marks
    // it watched and can trigger Smart Downloads right away, but if the
    // person reopens it before FINISHED_THRESHOLD -- to catch a post-credits
    // scene, say -- it still resumes from exactly where they left off
    // instead of restarting or having nothing to resume from.
    if (watched && !WatchedTracker::instance().isWatched(this->current.streamId)) {
        WatchedTracker::instance().markWatched(this->current.streamId);
        SmartDownloads::onEpisodeWatched(this->current);
    }

    if (finished) {
        this->removeId(this->current.streamId);
    } else {
        ResumeEntry e = this->current;
        e.position = pos;
        e.duration = dur;
        e.updated = (long)time(nullptr);
        this->upsert(e);
    }
    this->save();
    if (fire) this->changedEvent.fire();
}

void ResumeTracker::upsert(const ResumeEntry& e) {
    for (auto& it : this->items)
        if (it.streamId == e.streamId) {
            it = e;
            return;
        }
    this->items.push_back(e);
}

void ResumeTracker::removeId(const std::string& id) {
    this->items.erase(std::remove_if(this->items.begin(), this->items.end(),
                          [&](const ResumeEntry& x) { return x.streamId == id; }),
        this->items.end());
}

void ResumeTracker::remove(const std::string& streamId) {
    this->removeId(streamId);
    if (this->current.streamId == streamId) this->hasCurrent = false;
    this->save();
    this->changedEvent.fire();
}

void ResumeTracker::updateMeta(
    const std::string& streamId, const std::string& name, const std::string& poster) {
    bool dirty = false;
    for (auto& it : this->items) {
        if (it.streamId == streamId) {
            if (!name.empty() && it.name != name) {
                it.name = name;
                dirty = true;
            }
            if (!poster.empty() && it.poster != poster) {
                it.poster = poster;
                dirty = true;
            }
            if (!it.enriched) {
                it.enriched = true;  // mark resolved so we don't refetch every load
                dirty = true;
            }
            break;
        }
    }
    // Persist so the fix sticks across restarts. No changedEvent here: the
    // caller decides whether/when to rebuild the row.
    if (dirty) this->save();
}

std::vector<ResumeEntry> ResumeTracker::continueWatching() const {
    std::vector<ResumeEntry> r = this->items;
    std::sort(r.begin(), r.end(), [](const ResumeEntry& a, const ResumeEntry& b) { return a.updated > b.updated; });
    return r;
}

void ResumeTracker::load() {
    try {
        std::ifstream in(AppConfig::instance().configDir() + "/resume.json");
        if (in.is_open()) {
            nlohmann::json j;
            in >> j;
            this->items = j.get<std::vector<ResumeEntry>>();
        }
    } catch (const std::exception& e) {
        brls::Logger::warning("Resume load: {}", e.what());
        this->items.clear();
    }
}

void ResumeTracker::save() {
    std::string dir = AppConfig::instance().configDir();
#ifdef _WIN32
    ::mkdir(dir.c_str());
#else
    ::mkdir(dir.c_str(), 0777);
#endif
    try {
        std::ofstream out(dir + "/resume.json");
        if (out.is_open()) {
            nlohmann::json j = this->items;
            out << j.dump(2);
        }
    } catch (const std::exception& e) {
        brls::Logger::warning("Resume save: {}", e.what());
    }
}
