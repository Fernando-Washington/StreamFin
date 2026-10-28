#include "utils/sub_loader.hpp"
#include "view/mpv_core.hpp"

#include <algorithm>

SubLoader& SubLoader::instance() {
    static SubLoader inst;
    return inst;
}

void SubLoader::prepare(const std::string& type, const std::string& id) {
    this->init();
    this->pending.clear();
    this->fileLoaded = false;
    if (stremio::SUBTITLES_ADDON.empty()) return;

    int ticket = ++this->requestTicket;
    std::string url = stremio::SUBTITLES_ADDON + "/subtitles/" + type + "/" + id + ".json";
    stremio::getJSON<stremio::SubtitleList>(
        [this, ticket](stremio::SubtitleList r) {
            if (ticket != this->requestTicket) return;  // a newer title started
            // Keep the first subtitle per language, capped, so the picker
            // stays usable (addons can return dozens per release).
            std::vector<std::string> seen;
            for (auto& s : r.subtitles) {
                if (this->pending.size() >= 12) break;
                if (std::find(seen.begin(), seen.end(), s.lang) != seen.end()) continue;
                seen.push_back(s.lang);
                this->pending.push_back(s);
            }
            if (this->fileLoaded) this->attach();
        },
        [](const std::string& e) { brls::Logger::warning("subtitles addon: {}", e); }, url);
}

void SubLoader::cancelPending() {
    ++this->requestTicket;
    this->pending.clear();
    this->fileLoaded = false;
}

void SubLoader::init() {
    if (this->inited) return;
    this->inited = true;
    MPVCore::instance().getEvent()->subscribe([this](MpvEventEnum e) {
        switch (e) {
        case MpvEventEnum::MPV_LOADED:
            this->fileLoaded = true;
            this->attach();
            break;
        case MpvEventEnum::MPV_STOP:
        case MpvEventEnum::END_OF_FILE:
            this->fileLoaded = false;
            this->pending.clear();
            break;
        default:
            break;
        }
    });
}

void SubLoader::attach() {
    auto& mpv = MPVCore::instance();
    for (auto& s : this->pending) {
        std::string title = s.lang.empty() ? "External" : s.lang;
        // "auto" = add without selecting; the user picks via the + menu.
        mpv.command("sub-add", s.url.c_str(), "auto", title.c_str(), s.lang.c_str());
    }
    this->pending.clear();
}
