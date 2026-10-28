#pragma once

#include <borealis/core/singleton.hpp>
#include <borealis/core/event.hpp>
#include <nlohmann/json.hpp>
#include <atomic>
#include <mutex>
#include <string>
#include <vector>

enum class DownloadStatus { Queued, Downloading, Completed, Failed, NotFound };

NLOHMANN_JSON_SERIALIZE_ENUM(DownloadStatus, {
    {DownloadStatus::Queued, "Queued"},
    {DownloadStatus::Downloading, "Downloading"},
    {DownloadStatus::Completed, "Completed"},
    {DownloadStatus::Failed, "Failed"},
})

// One saved-for-offline movie or episode. `id` is the same streamId shape
// ResumeEntry already uses ("tt1234567" for a movie, "tt1234567:1:2" for
// S1E2), so a download and its Continue Watching entry line up naturally.
struct DownloadItem {
    std::string id;
    std::string name;          // display title, e.g. "Series · S1E5 · Episode"
    std::string streamType;    // "movie" | "series"
    std::string seriesId;      // parent series id; empty for movies -- used to group episodes
    std::string seriesName;
    int seasonIndex = 0;
    int episodeIndex = 0;
    std::string poster;        // poster URL, fetched once and cached alongside the file
    std::string qualityLabel;  // display-only, e.g. "Knaben · 1080p" (from stremio::parseStream)
    std::string sourceUrl;     // the stream URL to fetch -- caller already picked/resolved this
    DownloadStatus status = DownloadStatus::Queued;
    std::string filePath;      // filename on disk inside its own item directory, e.g. "video.mp4"
    int64_t totalBytes = 0;      // filled in by the caller from a HEAD request before queuing
    int64_t downloadedBytes = 0;
    std::string errorMessage;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(DownloadItem, id, name, streamType, seriesId, seriesName,
    seasonIndex, episodeIndex, poster, qualityLabel, sourceUrl, status, filePath, totalBytes, downloadedBytes,
    errorMessage);

// Blocking HEAD-style request for a URL's Content-Length. Returns -1 if the
// server doesn't report a size or the request fails -- callers should treat
// that as "size unknown", not zero.
int64_t remoteContentLength(const std::string& url);

// Bytes free on the SD card, where downloads live (same volume as
// AppConfig::configDir()). Returns -1 on error.
int64_t downloadDiskFreeBytes();

// Total capacity of that same SD card. Returns -1 on error.
int64_t downloadDiskTotalBytes();

class DownloadManager : public brls::Singleton<DownloadManager> {
public:
    using ProgressEvent = brls::Event<std::string, int64_t, int64_t>;  // id, downloadedBytes, totalBytes
    using StatusEvent = brls::Event<std::string, DownloadStatus>;      // id, new status

    void init();

    // `item` must already have id/name/sourceUrl/totalBytes populated -- the
    // caller does the size check (remoteContentLength) and any user
    // confirmation before calling this. A no-op if `item.id` is already queued
    // or downloaded.
    void addDownload(const DownloadItem& item);
    void cancelDownload(const std::string& id);
    void removeDownload(const std::string& id);
    void resumeQueue();

    DownloadStatus findItem(const std::string& id) const;
    // {total episodes downloaded-or-queued, completed} for a series, used for
    // a season's download-progress badge.
    std::pair<size_t, size_t> findSeries(const std::string& seriesId) const;
    std::string getLocalPath(const std::string& id) const;  // "" unless Completed

    std::vector<DownloadItem> getItems() const;
    std::string downloadDir() const;
    // Sum of on-disk bytes for completed downloads -- what the app itself is
    // using on the SD card, as opposed to everything else on it.
    int64_t totalOwnBytes() const;

    ProgressEvent* getProgressEvent() { return &progressEvent; }
    StatusEvent* getStatusEvent() { return &statusEvent; }

private:
    void saveIndex();
    void loadIndex();
    void processQueue();
    void doDownload(DownloadItem& item);

    mutable std::mutex mutex;
    std::vector<DownloadItem> items;
    std::shared_ptr<std::atomic_bool> currentCancel;
    bool downloading = false;

    ProgressEvent progressEvent;
    StatusEvent statusEvent;
};
