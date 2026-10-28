#pragma once

#include <borealis/core/singleton.hpp>
#include <set>
#include <string>

// Persistent "already watched" history, separate from ResumeTracker's
// Continue Watching list. A ResumeEntry is *removed* once a title crosses
// the "finished" threshold (see ResumeTracker::onProgress) -- that's correct
// for Continue Watching (nothing left to resume), but it means there was
// never a durable record of "this episode was watched" to show a checkmark
// on the episode list, or to drive Smart Downloads (utils/smart_downloads.hpp).
class WatchedTracker : public brls::Singleton<WatchedTracker> {
public:
    void init();  // load once; safe to call repeatedly

    bool isWatched(const std::string& streamId);
    void markWatched(const std::string& streamId);
    void markUnwatched(const std::string& streamId);  // for a future "mark as unwatched" action

private:
    void load();
    void save();

    bool inited = false;
    std::set<std::string> watched;
};
