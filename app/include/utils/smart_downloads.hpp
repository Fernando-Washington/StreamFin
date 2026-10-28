#pragma once

#include "activity/stremio_resume.hpp"

// "Smart Downloads" (opt-in, Settings screen): once an episode is watched
// (see ResumeTracker::onProgress -> WatchedTracker::markWatched), if that
// episode also has a completed local download, delete it and automatically
// queue the next episode in the same season -- freeing SD card space for
// what's actually still ahead instead of what's already been seen.
namespace SmartDownloads {

// Call right after marking `entry` watched. No-op unless the setting is on,
// `entry` is a series episode, and it actually has a completed download to
// clean up. Never asks for confirmation -- that's the point of "smart".
void onEpisodeWatched(const ResumeEntry& entry);

}  // namespace SmartDownloads
