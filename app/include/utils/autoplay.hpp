#pragma once

#include "activity/stremio_resume.hpp"
#include "utils/config.hpp"

// Plays the next episode automatically (with a cancelable on-screen countdown)
// when a Stremio series episode reaches a natural end of file. Movies and
// anything played outside the Stremio stream picker (downloads, webdav, ...)
// are never armed, so they just end normally.
class AutoplayManager {
public:
    static AutoplayManager& instance();

    // Call right after starting playback of a series episode (key.streamId
    // must be "ttID:season:episode", as produced by the Stremio stream picker
    // and the Home continue-watching row).
    void arm(const ResumeEntry& key);

    // Call before/at the start of any playback that should NOT autoplay a
    // next episode (movies, downloads, webdav, ...), and whenever the current
    // armed context becomes stale (e.g. the player is torn down while a
    // next-episode lookup is still in flight).
    void disarm();

    // True if this natural end-of-file is (still) ours to handle -- the
    // player's own "close on end of file" subscriber checks this and skips
    // closing when it's true, since AutoplayManager takes over the activity
    // (hold the frozen frame, show "up next", and either chain into the next
    // episode or close the activity itself once it knows there's nothing to
    // offer). Mirrors the exact condition onEndOfFile() itself checks before
    // consuming the arm, so the two never disagree about who's closing the
    // activity: if this returned true while onEndOfFile() went on to decide
    // there was nothing to offer, the player would suppress its own close
    // and then nobody would close it.
    bool willHandle() const {
        return armed && AppConfig::instance().getItem<bool>(AppConfig::AUTOPLAY_NEXT_EPISODE, true);
    }

    // Lets a RemotePlayer that suppressed its own close (because willHandle()
    // was true) tell, from its destructor, whether anything else has since
    // taken over (a new arm()/disarm()) -- see remote_view.cpp for why this
    // is needed instead of an unconditional disarm() there.
    int currentGeneration() const { return generation; }

private:
    AutoplayManager() = default;
    void init();
    void onEndOfFile();
    void offerNext(const std::string& nextId, int season, int episode);
    void giveUp();
    void showCountdown(const std::string& nextId, const std::string& label, const std::string& url);
    void startPlayback(const std::string& nextId, const std::string& label, const std::string& url);

    bool inited = false;
    bool armed = false;
    int generation = 0;  // bumped on arm()/disarm(); invalidates in-flight lookups
    ResumeEntry currentKey;
};
