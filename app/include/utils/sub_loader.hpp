#pragma once

#include "api/stremio.hpp"
#include <vector>
#include <string>

// Fetches subtitles from the configured subtitles addon (SubSource etc.) for
// the title currently playing and attaches them to MPV once the file is
// loaded. They then appear in the player's + menu Subtitle picker alongside
// any embedded tracks. Shared by the stream picker and the autoplay-next-
// episode flow (both start playback of a Stremio title).
class SubLoader {
public:
    static SubLoader& instance();

    // Call right before starting playback of {type, id}.
    void prepare(const std::string& type, const std::string& id);

    // Call before starting playback that does NOT call prepare() (downloads,
    // webdav, ...), so a still-in-flight request from a previous Stremio
    // title can't attach its subtitles to this different piece of media once
    // it resolves.
    void cancelPending();

private:
    void init();
    void attach();

    bool inited = false;
    bool fileLoaded = false;
    int requestTicket = 0;
    std::vector<stremio::Subtitle> pending;
};
