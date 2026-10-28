#include "activity/stremio_downloads.hpp"
#include "activity/stremio_resume.hpp"
#include "view/recycling_grid.hpp"
#include "view/mpv_core.hpp"
#include "tab/remote_view.hpp"
#include "utils/autoplay.hpp"
#include "utils/config.hpp"
#include "utils/dialog.hpp"
#include "utils/image.hpp"
#include "utils/sub_loader.hpp"
#include "utils/theme.hpp"

#include <algorithm>
#include <fstream>
#include <unordered_map>

using namespace brls::literals;

namespace {

std::string formatBytes(int64_t bytes) {
    double value = static_cast<double>(std::max<int64_t>(0, bytes));
    const char* units[] = {"B", "KB", "MB", "GB", "TB"};
    size_t unit = 0;
    while (value >= 1024.0 && unit < 4) {
        value /= 1024.0;
        ++unit;
    }
    return fmt::format("{:.1f} {}", value, units[unit]);
}

// One episode or standalone movie -- used both on the flat "all downloads"
// list (for movies, and as the row inside a series' episode screen) and
// nowhere else. Visually a small card: poster, title, quality, status/size,
// with real spacing between each line instead of everything crammed together.
class DownloadCell : public RecyclingGridItem {
public:
    DownloadCell() {
        this->setFocusable(true);
        this->setAxis(brls::Axis::COLUMN);
        this->setPadding(14, 20, 14, 20);
        this->setCornerRadius(8);
        this->setBackgroundColor(theme::BG_CARD);
        this->setMarginBottom(2);

        auto* row = new brls::Box();
        row->setAxis(brls::Axis::ROW);
        this->poster = new brls::Image();
        this->poster->setDimensions(88, 132);
        this->poster->setScalingType(brls::ImageScalingType::FILL);
        this->poster->setCornerRadius(6);
        row->addView(this->poster);

        auto* info = new brls::Box();
        info->setAxis(brls::Axis::COLUMN);
        info->setGrow(1.0f);
        info->setMarginLeft(20);
        info->setJustifyContent(brls::JustifyContent::CENTER);
        this->name = new brls::Label();
        this->name->setFontSize(22);
        this->name->setIsWrapping(true);
        this->name->setMarginBottom(8);
        info->addView(this->name);
        this->quality = new brls::Label();
        this->quality->setFontSize(17);
        this->quality->setTextColor(theme::TEXT_SECONDARY);
        this->quality->setMarginBottom(6);
        info->addView(this->quality);
        this->detail = new brls::Label();
        this->detail->setFontSize(18);
        this->detail->setIsWrapping(true);
        info->addView(this->detail);
        row->addView(info);
        this->addView(row);
        this->track = new brls::Box();
        this->track->setHeight(5);
        this->track->setMarginTop(12);
        this->track->setBackgroundColor(theme::BG_TRACK);
        this->progress = new brls::Box();
        this->progress->setHeight(5);
        this->progress->setBackgroundColor(theme::PROGRESS_FILL);
        this->track->addView(this->progress);
        this->addView(this->track);
        this->registerAction("Cancel / Remove", brls::BUTTON_X, [this](brls::View*) {
            auto& manager = DownloadManager::instance();
            auto status = manager.findItem(this->id);
            if (status == DownloadStatus::Queued || status == DownloadStatus::Downloading)
                manager.cancelDownload(this->id);
            else if (status == DownloadStatus::Completed || status == DownloadStatus::Failed) {
                std::string id = this->id;
                Dialog::cancelable("Delete this download? This cannot be undone.", [id]() {
                    DownloadManager::instance().removeDownload(id);
                });
            }
            return true;
        });
    }

    ~DownloadCell() override { Image::cancel(this->poster); }
    void cacheForReuse() override {
        Image::cancel(this->poster);
        this->posterSource.clear();
    }

    void update(const DownloadItem& item) {
        this->id = item.id;
        // Episode names queued by StreamPicker already contain SxEy; inside a
        // series' own screen that's redundant with the header, but harmless.
        this->name->setText(item.name);
        this->quality->setText(item.qualityLabel);
        this->quality->setVisibility(item.qualityLabel.empty()
            ? brls::Visibility::GONE : brls::Visibility::VISIBLE);

        // getLocalPath already uses the manager's sanitized episode directory.
        // Derive the cached poster path from it instead of duplicating that rule.
        std::string imageSource = item.poster;
        bool localPoster = false;
        if (item.status == DownloadStatus::Completed) {
            std::string videoPath = DownloadManager::instance().getLocalPath(item.id);
            size_t slash = videoPath.find_last_of("/\\");
            if (slash != std::string::npos) {
                std::string thumb = videoPath.substr(0, slash + 1) + "thumb.jpg";
                if (std::ifstream(thumb).good()) {
                    imageSource = thumb;
                    localPoster = true;
                }
            }
        }
        if (imageSource != this->posterSource) {
            Image::cancel(this->poster);
            this->posterSource = imageSource;
            this->poster->setImageFromRes("img/video-card-bg.png");
            if (localPoster) {
                this->poster->setImageFromFile(imageSource);
            } else if (!imageSource.empty()) {
                this->poster->setFreeTexture(false);
                Image::with(this->poster, imageSource);
            }
        }
        std::string text;
        switch (item.status) {
        case DownloadStatus::Queued: text = "Queued · X: Cancel"; break;
        case DownloadStatus::Downloading:
            text = "Downloading · " + formatBytes(item.downloadedBytes);
            if (item.totalBytes > 0) text += " / " + formatBytes(item.totalBytes);
            if (item.totalBytes > 0)
                text += fmt::format(" ({:.0f}%)", std::min(100.0,
                    100.0 * static_cast<double>(item.downloadedBytes) / item.totalBytes));
            text += " · X: Cancel";
            break;
        case DownloadStatus::Completed:
            text = formatBytes(item.downloadedBytes > 0 ? item.downloadedBytes : item.totalBytes)
                + " · A: Play · X: Remove";
            break;
        case DownloadStatus::Failed:
            text = "Failed: " + item.errorMessage + " · X: Remove";
            break;
        case DownloadStatus::NotFound: text = "Unavailable"; break;
        }
        this->detail->setText(text);
        this->detail->setTextColor(item.status == DownloadStatus::Failed ? theme::STATUS_FAILED
            : item.status == DownloadStatus::Completed ? theme::STATUS_COMPLETED
            : item.status == DownloadStatus::Downloading ? theme::STATUS_ACTIVE
            : theme::TEXT_SECONDARY);
        this->track->setVisibility(item.status == DownloadStatus::Downloading
            ? brls::Visibility::VISIBLE : brls::Visibility::GONE);
        float percent = item.totalBytes > 0
            ? static_cast<float>(100.0 * static_cast<double>(item.downloadedBytes) / item.totalBytes) : 0.0f;
        this->progress->setWidthPercentage(std::clamp(percent, 0.0f, 100.0f));
    }

    std::string id;
private:
    brls::Label* name = nullptr;
    brls::Image* poster = nullptr;
    brls::Label* quality = nullptr;
    brls::Label* detail = nullptr;
    brls::Box* track = nullptr;
    brls::Box* progress = nullptr;
    std::string posterSource;
};

// One row per SERIES on the flat Downloads screen (instead of one row per
// episode) -- poster, name, per-status counts and size, opens the series' own
// episode list on selection.
class SeriesGroupCell : public RecyclingGridItem {
public:
    SeriesGroupCell() {
        this->setFocusable(true);
        this->setAxis(brls::Axis::ROW);
        this->setPadding(14, 20, 14, 20);
        this->setCornerRadius(8);
        this->setBackgroundColor(theme::BG_CARD);
        this->setMarginBottom(2);

        this->poster = new brls::Image();
        this->poster->setDimensions(88, 132);
        this->poster->setScalingType(brls::ImageScalingType::FILL);
        this->poster->setCornerRadius(6);
        this->addView(this->poster);

        auto* info = new brls::Box();
        info->setAxis(brls::Axis::COLUMN);
        info->setGrow(1.0f);
        info->setMarginLeft(20);
        info->setJustifyContent(brls::JustifyContent::CENTER);
        this->name = new brls::Label();
        this->name->setFontSize(22);
        this->name->setIsWrapping(true);
        this->name->setMarginBottom(8);
        info->addView(this->name);
        this->detail = new brls::Label();
        this->detail->setFontSize(18);
        this->detail->setTextColor(theme::TEXT_SECONDARY);
        info->addView(this->detail);
        this->addView(info);
    }

    ~SeriesGroupCell() override { Image::cancel(this->poster); }
    void cacheForReuse() override {
        Image::cancel(this->poster);
        this->posterSource.clear();
    }

    void update(const std::string& seriesName, const std::string& poster, size_t episodeCount,
        size_t readyCount, size_t activeCount, size_t failedCount, int64_t totalBytes) {
        this->name->setText(seriesName);
        std::string status;
        auto addCount = [&status](size_t count, const char* label) {
            if (!count) return;
            if (!status.empty()) status += " · ";
            status += fmt::format("{} {}", count, label);
        };
        addCount(readyCount, "ready");
        addCount(activeCount, "active");  // queued or downloading
        addCount(failedCount, "failed");
        if (status.empty()) status = fmt::format("{} episodes", episodeCount);
        this->detail->setText(fmt::format("{} · {} · A: Open", status, formatBytes(totalBytes)));
        if (poster != this->posterSource) {
            Image::cancel(this->poster);
            this->posterSource = poster;
            this->poster->setImageFromRes("img/video-card-bg.png");
            if (!poster.empty()) {
                this->poster->setFreeTexture(false);
                Image::with(this->poster, poster);
            }
        }
    }

private:
    brls::Image* poster = nullptr;
    brls::Label* name = nullptr;
    brls::Label* detail = nullptr;
    std::string posterSource;
};

// A row on the flat Downloads screen: either a standalone movie (rendered
// with DownloadCell, same as before) or a series summary (SeriesGroupCell).
// Series episodes are grouped here instead of listed individually -- opening
// the group is what shows them, in order, via StremioDownloadedSeries.
struct DownloadRow {
    bool isSeries = false;
    std::string key;  // diffing identity: item.id for movies, "series:"+seriesId for groups
    DownloadItem item;  // valid when !isSeries
    std::string seriesId, seriesName, poster;
    size_t episodeCount = 0;
    size_t readyCount = 0, activeCount = 0, failedCount = 0;
    int64_t totalBytes = 0;
};

std::vector<DownloadRow> buildRows(const std::vector<DownloadItem>& items) {
    std::vector<DownloadRow> rows;
    std::unordered_map<std::string, size_t> seriesIndex;
    auto countStatus = [](DownloadRow& row, DownloadStatus status) {
        switch (status) {
        case DownloadStatus::Completed: ++row.readyCount; break;
        case DownloadStatus::Queued:
        case DownloadStatus::Downloading: ++row.activeCount; break;
        case DownloadStatus::Failed: ++row.failedCount; break;
        default: break;
        }
    };
    for (auto& item : items) {
        int64_t bytes = item.downloadedBytes > 0 ? item.downloadedBytes : item.totalBytes;
        if (item.seriesId.empty()) {
            DownloadRow row;
            row.isSeries = false;
            row.key = item.id;
            row.item = item;
            rows.push_back(std::move(row));
            continue;
        }
        auto it = seriesIndex.find(item.seriesId);
        if (it == seriesIndex.end()) {
            DownloadRow row;
            row.isSeries = true;
            row.key = "series:" + item.seriesId;
            row.seriesId = item.seriesId;
            row.seriesName = item.seriesName.empty() ? item.seriesId : item.seriesName;
            row.poster = item.poster;
            row.episodeCount = 1;
            countStatus(row, item.status);
            row.totalBytes = bytes;
            seriesIndex[item.seriesId] = rows.size();
            rows.push_back(std::move(row));
        } else {
            auto& row = rows[it->second];
            row.episodeCount++;
            countStatus(row, item.status);
            row.totalBytes += bytes;
            if (row.poster.empty()) row.poster = item.poster;
        }
    }
    return rows;
}

// Playing a download used to go straight to RemoteView::play() with no
// ResumeEntry set -- meaning a downloaded episode never marked itself
// watched, never updated Continue Watching, and never triggered Smart
// Downloads, since all three key off ResumeTracker::onProgress() and that
// only fires when hasCurrent is true (see stremio_resume.cpp). This wires a
// downloaded item into exactly the same identity/preference setup the online
// stream picker already does (stremio_streams.cpp's onItemSelected), so
// "watch what I downloaded" and "watch it online" are the same title as far
// as the rest of the app is concerned -- not two disconnected experiences.
void playDownloadedItem(const DownloadItem& item) {
    auto path = DownloadManager::instance().getLocalPath(item.id);
    if (path.empty()) return;
    ResumeEntry key{item.streamType, item.id, item.name, item.poster};
    ResumeTracker::instance().setCurrent(key);
    SubLoader::instance().prepare(item.streamType, item.id);
    std::string alang = AppConfig::instance().getItem<std::string>(AppConfig::AUDIO_LANG_PREF, "eng,en");
    MPVCore::instance().command("set", "alang", alang.c_str());
    RemoteView::play(path, item.name, item.id);
    if (item.streamType == "series") {
        AutoplayManager::instance().arm(key);
    } else {
        AutoplayManager::instance().disarm();
    }
}

class DownloadSource : public RecyclingGridDataSource {
public:
    std::vector<DownloadRow> rows;
    size_t getItemCount() override { return this->rows.size(); }
    RecyclingGridItem* cellForRow(RecyclingView* recycler, size_t index) override {
        auto& row = this->rows.at(index);
        if (row.isSeries) {
            auto* cell = static_cast<SeriesGroupCell*>(recycler->dequeueReusableCell("Series"));
            cell->update(row.seriesName, row.poster, row.episodeCount,
                row.readyCount, row.activeCount, row.failedCount, row.totalBytes);
            return cell;
        }
        auto* cell = static_cast<DownloadCell*>(recycler->dequeueReusableCell("Item"));
        cell->update(row.item);
        return cell;
    }
    void onItemSelected(brls::Box*, size_t index) override {
        auto& row = this->rows.at(index);
        if (row.isSeries) {
            brls::Application::pushActivity(
                new brls::Activity(new StremioDownloadedSeries(row.seriesId, row.seriesName, row.poster)));
            return;
        }
        playDownloadedItem(row.item);
    }
    void clearData() override { this->rows.clear(); }
};

// Same episode row, filtered to one series and sorted by season/episode --
// used by StremioDownloadedSeries.
class SeriesEpisodeSource : public RecyclingGridDataSource {
public:
    std::vector<DownloadItem> items;
    size_t getItemCount() override { return this->items.size(); }
    RecyclingGridItem* cellForRow(RecyclingView* recycler, size_t index) override {
        auto* cell = static_cast<DownloadCell*>(recycler->dequeueReusableCell("Item"));
        cell->update(this->items.at(index));
        return cell;
    }
    void onItemSelected(brls::Box*, size_t index) override {
        playDownloadedItem(this->items.at(index));
    }
    void clearData() override { this->items.clear(); }
};

}  // namespace

StremioDownloads::StremioDownloads() {
    this->setAxis(brls::Axis::COLUMN);
    this->setDimensions(brls::Application::contentWidth, brls::Application::contentHeight);
    this->setBackgroundColor(brls::Application::getTheme()["brls/background"]);
    this->setPadding(20, 40, 20, 40);
    this->setFocusable(true);
    auto* title = new brls::Label();
    title->setText("Downloads");
    title->setFontSize(27);
    title->setMarginBottom(20);
    title->setTextColor(theme::TEXT_PRIMARY);
    this->addView(title);
    this->summaryLabel = new brls::Label();
    this->summaryLabel->setFontSize(18);
    this->summaryLabel->setTextColor(theme::TEXT_SECONDARY);
    this->summaryLabel->setMarginBottom(14);
    this->addView(this->summaryLabel);
    this->emptyLabel = new brls::Label();
    this->emptyLabel->setText("Nenhum download ainda");
    this->emptyLabel->setFontSize(20);
    this->addView(this->emptyLabel);
    this->recycler = new RecyclingGrid();
    this->recycler->setGrow(1.0f);
    this->recycler->setScrollingIndicatorVisible(false);
    this->recycler->spanCount = 1;
    this->recycler->estimatedRowHeight = 182;
    this->recycler->estimatedRowSpace = 10;
    this->recycler->registerCell("Item", []() -> RecyclingGridItem* { return new DownloadCell(); });
    this->recycler->registerCell("Series", []() -> RecyclingGridItem* { return new SeriesGroupCell(); });
    this->recycler->setDataSource(new DownloadSource());
    this->addView(this->recycler);
    this->registerAction("hints/back"_i18n, brls::BUTTON_B, [](brls::View*) {
        brls::Application::popActivity();
        return true;
    });
    auto& manager = DownloadManager::instance();
    // DownloadManager dispatches both events on the UI thread.
    this->progressSubscription = manager.getProgressEvent()->subscribe(
        [this](std::string, int64_t, int64_t) { this->refresh(); });
    this->statusSubscription = manager.getStatusEvent()->subscribe(
        [this](std::string, DownloadStatus) { this->refresh(); });
    this->refresh();
}

StremioDownloads::~StremioDownloads() {
    auto& manager = DownloadManager::instance();
    manager.getProgressEvent()->unsubscribe(this->progressSubscription);
    manager.getStatusEvent()->unsubscribe(this->statusSubscription);
}

void StremioDownloads::refresh() {
    auto items = DownloadManager::instance().getItems();
    auto rows = buildRows(items);
    auto* source = static_cast<DownloadSource*>(this->recycler->getDataSource());
    bool sameRows = rows.size() == source->rows.size();
    for (size_t i = 0; sameRows && i < rows.size(); ++i) sameRows = rows[i].key == source->rows[i].key;

    bool hasFocus = false;
    std::string focusedKey;
    size_t focusedIndex = 0;
    for (auto* view = brls::Application::getCurrentFocus(); view; view = view->getParent()) {
        if (auto* cell = dynamic_cast<DownloadCell*>(view)) {
            focusedKey = cell->id;
            focusedIndex = cell->getIndex();
        }
        if (view == this) { hasFocus = true; break; }
    }
    source->rows = std::move(rows);
    // Counted from the flat item list, not the grouped rows -- a series row
    // collapses several episodes (each with its own status) into one line.
    size_t active = 0, completed = 0, failed = 0;
    for (const auto& item : items) {
        if (item.status == DownloadStatus::Queued || item.status == DownloadStatus::Downloading) ++active;
        else if (item.status == DownloadStatus::Completed) ++completed;
        else if (item.status == DownloadStatus::Failed) ++failed;
    }
    this->summaryLabel->setText(fmt::format("{} active  ·  {} completed  ·  {} failed",
        active, completed, failed));
    bool empty = source->rows.empty();
    this->emptyLabel->setVisibility(empty ? brls::Visibility::VISIBLE : brls::Visibility::GONE);
    if (sameRows) {
        for (auto* view : this->recycler->getGridItems()) {
            size_t index = view->getIndex();
            if (index >= source->rows.size()) continue;
            auto& row = source->rows[index];
            if (row.isSeries) {
                if (auto* cell = dynamic_cast<SeriesGroupCell*>(view))
                    cell->update(row.seriesName, row.poster, row.episodeCount,
                        row.readyCount, row.activeCount, row.failedCount, row.totalBytes);
            } else if (auto* cell = dynamic_cast<DownloadCell*>(view)) {
                cell->update(row.item);
            }
        }
        return;
    }
    // Move focus away from cells before recycling them, without stealing it from the player.
    this->rebuilding = true;
    if (hasFocus) brls::Application::giveFocus(this);
    for (size_t i = 0; i < source->rows.size(); ++i)
        if (source->rows[i].key == focusedKey) { focusedIndex = i; break; }
    this->recycler->setDefaultCellFocus(empty ? 0 : std::min(focusedIndex, source->rows.size() - 1));
    this->recycler->reloadData();
    this->rebuilding = false;
    if (hasFocus && !empty) brls::Application::giveFocus(this->recycler);
}

void StremioDownloads::willAppear(bool resetState) {
    brls::Box::willAppear(resetState);
    this->refresh();
}

brls::View* StremioDownloads::getDefaultFocus() {
    if (this->rebuilding || this->recycler->getItemCount() == 0) return this;
    auto* focus = this->recycler->getDefaultFocus();
    return focus ? focus : this;
}

// ---- StremioDownloadedSeries -----------------------------------------------

StremioDownloadedSeries::StremioDownloadedSeries(std::string seriesId, std::string seriesName, std::string poster)
    : seriesId(std::move(seriesId)) {
    this->setAxis(brls::Axis::COLUMN);
    this->setDimensions(brls::Application::contentWidth, brls::Application::contentHeight);
    this->setBackgroundColor(brls::Application::getTheme()["brls/background"]);
    this->setPadding(20, 40, 20, 40);
    this->setFocusable(true);

    // Header: poster + series name + summary, before the episode list --
    // same "know what you're looking at before scrolling" idea as opening a
    // season from the series screen itself.
    auto* header = new brls::Box();
    header->setAxis(brls::Axis::ROW);
    header->setMarginBottom(20);
    auto* headerPoster = new brls::Image();
    headerPoster->setDimensions(110, 165);
    headerPoster->setScalingType(brls::ImageScalingType::FILL);
    headerPoster->setCornerRadius(8);
    headerPoster->setImageFromRes("img/video-card-bg.png");
    if (!poster.empty()) {
        headerPoster->setFreeTexture(false);
        Image::with(headerPoster, poster);
    }
    header->addView(headerPoster);
    auto* headerInfo = new brls::Box();
    headerInfo->setAxis(brls::Axis::COLUMN);
    headerInfo->setMarginLeft(20);
    headerInfo->setJustifyContent(brls::JustifyContent::CENTER);
    auto* title = new brls::Label();
    title->setText(seriesName);
    title->setFontSize(27);
    title->setMarginBottom(8);
    title->setTextColor(theme::TEXT_PRIMARY);
    headerInfo->addView(title);
    this->summaryLabel = new brls::Label();
    this->summaryLabel->setFontSize(18);
    this->summaryLabel->setTextColor(theme::TEXT_SECONDARY);
    headerInfo->addView(this->summaryLabel);
    header->addView(headerInfo);
    this->addView(header);

    this->recycler = new RecyclingGrid();
    this->recycler->setGrow(1.0f);
    this->recycler->setScrollingIndicatorVisible(false);
    this->recycler->spanCount = 1;
    this->recycler->estimatedRowHeight = 182;
    this->recycler->estimatedRowSpace = 10;
    this->recycler->registerCell("Item", []() -> RecyclingGridItem* { return new DownloadCell(); });
    this->recycler->setDataSource(new SeriesEpisodeSource());
    this->addView(this->recycler);

    this->registerAction("hints/back"_i18n, brls::BUTTON_B, [](brls::View*) {
        brls::Application::popActivity();
        return true;
    });

    auto& manager = DownloadManager::instance();
    this->progressSubscription = manager.getProgressEvent()->subscribe(
        [this](std::string, int64_t, int64_t) { this->refresh(); });
    this->statusSubscription = manager.getStatusEvent()->subscribe(
        [this](std::string, DownloadStatus) { this->refresh(); });
    this->refresh();
}

StremioDownloadedSeries::~StremioDownloadedSeries() {
    auto& manager = DownloadManager::instance();
    manager.getProgressEvent()->unsubscribe(this->progressSubscription);
    manager.getStatusEvent()->unsubscribe(this->statusSubscription);
}

void StremioDownloadedSeries::refresh() {
    auto all = DownloadManager::instance().getItems();
    std::vector<DownloadItem> items;
    for (auto& item : all)
        if (item.seriesId == this->seriesId) items.push_back(item);
    std::sort(items.begin(), items.end(), [](const DownloadItem& a, const DownloadItem& b) {
        return a.seasonIndex != b.seasonIndex ? a.seasonIndex < b.seasonIndex : a.episodeIndex < b.episodeIndex;
    });

    auto* source = static_cast<SeriesEpisodeSource*>(this->recycler->getDataSource());
    bool sameRows = items.size() == source->items.size();
    for (size_t i = 0; sameRows && i < items.size(); ++i) sameRows = items[i].id == source->items[i].id;

    bool hasFocus = false;
    std::string focusedId;
    size_t focusedIndex = 0;
    for (auto* view = brls::Application::getCurrentFocus(); view; view = view->getParent()) {
        if (auto* cell = dynamic_cast<DownloadCell*>(view)) {
            focusedId = cell->id;
            focusedIndex = cell->getIndex();
        }
        if (view == this) { hasFocus = true; break; }
    }
    source->items = items;
    int64_t totalBytes = 0;
    for (auto& item : items) totalBytes += item.downloadedBytes > 0 ? item.downloadedBytes : item.totalBytes;
    this->summaryLabel->setText(
        fmt::format("{} episode{} · {}", items.size(), items.size() == 1 ? "" : "s", formatBytes(totalBytes)));

    // No episodes left (all removed) -- nothing else to do here; the flat
    // Downloads screen behind this one will stop showing this series on its
    // own next refresh, so just back out.
    if (items.empty()) {
        brls::Application::popActivity();
        return;
    }
    if (sameRows) {
        for (auto* view : this->recycler->getGridItems()) {
            auto* cell = static_cast<DownloadCell*>(view);
            if (cell->getIndex() < source->items.size()) cell->update(source->items[cell->getIndex()]);
        }
        return;
    }
    this->rebuilding = true;
    if (hasFocus) brls::Application::giveFocus(this);
    for (size_t i = 0; i < source->items.size(); ++i)
        if (source->items[i].id == focusedId) { focusedIndex = i; break; }
    this->recycler->setDefaultCellFocus(std::min(focusedIndex, source->items.size() - 1));
    this->recycler->reloadData();
    this->rebuilding = false;
    if (hasFocus) brls::Application::giveFocus(this->recycler);
}

void StremioDownloadedSeries::willAppear(bool resetState) {
    brls::Box::willAppear(resetState);
    this->refresh();
}

brls::View* StremioDownloadedSeries::getDefaultFocus() {
    if (this->rebuilding || this->recycler->getItemCount() == 0) return this;
    auto* focus = this->recycler->getDefaultFocus();
    return focus ? focus : this;
}
