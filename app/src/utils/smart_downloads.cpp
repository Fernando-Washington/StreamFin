#include "utils/smart_downloads.hpp"
#include "utils/config.hpp"
#include "utils/download.hpp"
#include "api/stremio.hpp"

#include <algorithm>
#include <borealis/core/thread.hpp>

namespace SmartDownloads {

namespace {

// Second half of the flow: the next episode's streams have resolved -- fetch
// its size and queue it, same shape as the manual download flow in
// stremio_streams.cpp, minus the confirmation dialog (this is the
// "smart"/automatic path). `watchedId`'s download is only removed once this
// succeeds -- see the design note in onEpisodeWatched() below for why.
void queueNext(const std::string& nextId, int season, int episode, const std::string& seriesId,
    const std::string& seriesName, const std::string& watchedId, int64_t watchedBytes,
    std::vector<stremio::Stream> streams) {
    const stremio::Stream* best = stremio::pickBestStream(streams);
    if (!best) return;  // nothing playable for the next episode -- keep what's already downloaded
    std::string url = best->url;
    std::string qualityLabel = stremio::parseStream(*best).title;

    brls::async([nextId, season, episode, seriesId, seriesName, watchedId, watchedBytes, url, qualityLabel]() {
        int64_t size = remoteContentLength(url);
        int64_t free = downloadDiskFreeBytes();
        brls::sync([nextId, season, episode, seriesId, seriesName, watchedId, watchedBytes, url, qualityLabel, size,
                       free]() {
            if (DownloadManager::instance().findItem(nextId) != DownloadStatus::NotFound) return;
            // The watched episode's own bytes count as "about to be free" --
            // the swap (remove old, add new) shouldn't be blocked by space
            // that's already spoken for by the file this is about to replace.
            int64_t effectiveFree = free >= 0 ? free + std::max<int64_t>(0, watchedBytes) : free;
            if (size > 0 && effectiveFree >= 0 && size > effectiveFree) {
                brls::Application::notify(
                    "Smart Downloads: not enough space for the next episode -- kept the one you just watched");
                return;
            }
            DownloadItem item;
            item.id = nextId;
            item.name = fmt::format("{} · S{}E{}", seriesName, season, episode);
            item.streamType = "series";
            item.seriesId = seriesId;
            item.seriesName = seriesName;
            item.seasonIndex = season;
            item.episodeIndex = episode;
            item.qualityLabel = qualityLabel;
            item.sourceUrl = url;
            item.totalBytes = size > 0 ? size : 0;
            DownloadManager::instance().addDownload(item);
            // Only remove the watched episode once its replacement is
            // actually queued -- see onEpisodeWatched() for why this order
            // matters. A failure anywhere before this point (no addon
            // configured, lookup error, nothing playable, not enough space)
            // leaves the watched episode's download untouched.
            DownloadManager::instance().removeDownload(watchedId);
            brls::Application::notify(
                fmt::format("Smart Downloads: queued {} S{}E{}, removed the one you just watched", seriesName,
                    season, episode));
        });
    });
}

}  // namespace

void onEpisodeWatched(const ResumeEntry& entry) {
    if (!AppConfig::instance().getItem<bool>(AppConfig::SMART_DOWNLOADS, false)) return;
    if (entry.streamType != "series") return;

    auto& manager = DownloadManager::instance();
    std::string watchedId = entry.streamId;
    int64_t watchedBytes = 0;
    bool hasDownload = false;
    for (auto& it : manager.getItems()) {
        if (it.id == watchedId && it.status == DownloadStatus::Completed) {
            hasDownload = true;
            watchedBytes = it.downloadedBytes > 0 ? it.downloadedBytes : it.totalBytes;
            break;
        }
    }
    if (!hasDownload) return;  // nothing downloaded to clean up

    size_t p1 = watchedId.find(':');
    size_t p2 = (p1 == std::string::npos) ? std::string::npos : watchedId.find(':', p1 + 1);
    if (p1 == std::string::npos || p2 == std::string::npos) return;  // not "ttID:season:episode"

    int season = 0, episode = 0;
    try {
        season = std::stoi(watchedId.substr(p1 + 1, p2 - p1 - 1));
        episode = std::stoi(watchedId.substr(p2 + 1));
    } catch (...) {
        return;
    }
    std::string seriesId = watchedId.substr(0, p1);
    std::string nextId = fmt::format("{}:{}:{}", seriesId, season, episode + 1);

    // entry.name is "Series · S1E5 · Episode name" (see stremio_streams.cpp /
    // autoplay.cpp) -- take just the series part for the next item's label.
    std::string seriesName = entry.name;
    size_t sep = entry.name.find(" · ");
    if (sep != std::string::npos) seriesName = entry.name.substr(0, sep);

    // IMPORTANT: the watched episode's download is deliberately NOT removed
    // here. It used to be removed unconditionally before even checking
    // whether a next episode exists -- if the addon had nothing for
    // episode+1 (end of season, or just a temporary gap), the file vanished
    // with no replacement, offline or on. Now it's only removed inside
    // queueNext(), and only once the replacement has actually been queued.
    if (stremio::STREAM_ADDONS.empty()) return;
    stremio::getStreams("series", nextId,
        [nextId, season, episode, seriesId, seriesName, watchedId, watchedBytes](std::vector<stremio::Stream> streams) {
            queueNext(nextId, season, episode + 1, seriesId, seriesName, watchedId, watchedBytes, std::move(streams));
        },
        [](const std::string&) {});
}

}  // namespace SmartDownloads
