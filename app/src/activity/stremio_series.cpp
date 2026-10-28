/*
    Stremio series + season screens -- implementation.
*/
#include "activity/stremio_series.hpp"
#include "activity/stremio_streams.hpp"
#include "activity/stremio_resume.hpp"
#include "view/recycling_grid.hpp"
#include "view/video_card.hpp"
#include "view/svg_image.hpp"
#include "utils/image.hpp"
#include "utils/config.hpp"
#include "utils/dialog.hpp"
#include "utils/theme.hpp"
#include "utils/watched.hpp"
#include "api/stremio.hpp"

#include <map>
#include <algorithm>

using namespace brls::literals;

namespace {

// ---- Season list cell (compact icon + name + episode count) --------------
class SeasonCell : public RecyclingGridItem {
public:
    SeasonCell() { this->inflateFromXMLRes("xml/view/dir_entry.xml"); }

    BRLS_BIND(SVGImage, icon, "file/icon");
    BRLS_BIND(brls::Label, name, "file/name");
    BRLS_BIND(brls::Label, detail, "file/misc");
};

struct SeasonEntry {
    std::string label;
    std::vector<stremio::Video> episodes;
};

class SeasonSource : public RecyclingGridDataSource {
public:
    SeasonSource(std::string seriesName, std::string background, std::vector<SeasonEntry> s)
        : seriesName(std::move(seriesName)), background(std::move(background)), list(std::move(s)) {}

    size_t getItemCount() override { return this->list.size(); }

    RecyclingGridItem* cellForRow(RecyclingView* recycler, size_t index) override {
        SeasonCell* cell = dynamic_cast<SeasonCell*>(recycler->dequeueReusableCell("Cell"));
        auto& s = this->list.at(index);
        cell->icon->setImageFromSVGRes("icon/ico-list.svg");
        cell->name->setText(s.label);
        cell->detail->setText(
            s.episodes.size() == 1 ? "1 episode" : fmt::format("{} episodes", s.episodes.size()));
        return cell;
    }

    void onItemSelected(brls::Box* recycler, size_t index) override {
        auto& s = this->list.at(index);
        brls::Application::pushActivity(
            new brls::Activity(new StremioSeason(this->seriesName, s.label, s.episodes, this->background)));
    }

    void clearData() override { this->list.clear(); }

private:
    std::string seriesName;
    std::string background;
    std::vector<SeasonEntry> list;
};

// ---- Episode list cell (wide card with thumbnail) ------------------------
class EpisodeCell : public BaseCardCell {
public:
    EpisodeCell() { this->inflateFromXMLRes("xml/view/episode_card.xml"); }

    BRLS_BIND(brls::Label, labelName, "episode/card/name");
    BRLS_BIND(brls::Label, labelOverview, "episode/card/overview");
};

class EpisodeSource : public RecyclingGridDataSource {
public:
    EpisodeSource(std::string seriesName, std::string background, const std::vector<stremio::Video>& v)
        : seriesName(std::move(seriesName)), background(std::move(background)), list(std::move(v)) {}
    ~EpisodeSource() override { *this->alive = false; }

    size_t getItemCount() override { return this->list.size(); }

    RecyclingGridItem* cellForRow(RecyclingView* recycler, size_t index) override {
        EpisodeCell* cell = dynamic_cast<EpisodeCell*>(recycler->dequeueReusableCell("Cell"));
        auto& v = this->list.at(index);

        cell->labelName->setText(fmt::format("E{} - {}", v.episode, v.name));
        // Prefix the air date (YYYY-MM-DD) to the overview when available.
        std::string date = v.released.size() >= 10 ? v.released.substr(0, 10) : "";
        std::string ov = v.overview;
        if (!date.empty()) ov = ov.empty() ? date : (date + "  ·  " + ov);
        cell->labelOverview->setText(this->loadingIndex == index ? "Loading streams..." : ov);
        if (WatchedTracker::instance().isWatched(v.id)) {
            cell->badgeTopRight->setImageFromSVGRes("icon/ico-checkmark.svg");
            cell->badgeTopRight->setVisibility(brls::Visibility::VISIBLE);
        } else {
            cell->badgeTopRight->setVisibility(brls::Visibility::GONE);
        }
        cell->rectProgress->getParent()->setVisibility(brls::Visibility::GONE);

        // Newly-aired episodes often have no still yet (metahub 404s); fall
        // back to the series backdrop instead of the grey placeholder.
        if (!v.thumbnail.empty())
            Image::with(cell->picture, v.thumbnail, this->background);
        else if (!this->background.empty())
            Image::with(cell->picture, this->background);

        return cell;
    }

    void onItemSelected(brls::Box* recycler, size_t index) override {
        if (this->loading) return;
        auto v = this->list.at(index);
        if (stremio::STREAM_ADDONS.empty()) {
            brls::Application::notify("Set your stream addon first (press − on the home screen)");
            return;
        }
        this->loading = true;
        this->loadingIndex = index;
        auto* grid = static_cast<RecyclingGrid*>(recycler);
        if (auto* cell = dynamic_cast<EpisodeCell*>(grid->getGridItemByIndex(index)))
            cell->labelOverview->setText("Loading streams...");
        auto alive = this->alive;

        // Player OSD / resume label: include the series name so it reads
        // "Series · S1E5 · Episode name" instead of just the episode.
        std::string label = this->seriesName.empty()
            ? fmt::format("S{}E{} · {}", v.season, v.episode, v.name)
            : fmt::format("{} · S{}E{} · {}", this->seriesName, v.season, v.episode, v.name);
        ResumeEntry key{"series", v.id, label, v.thumbnail};
        auto picker = std::make_shared<StreamPickerHandle>();
        stremio::getStreams("series", v.id,
            [this, alive, grid, index, label, key, picker](std::vector<stremio::Stream> streams, size_t pending) {
                if (!*alive) return;
                if (!picker->opened()) {
                    this->finishLoading(grid, index);
                    auto* view = new StreamPicker(label, streams, key, false, {}, {}, pending);
                    picker->attach(view);
                    brls::Application::pushActivity(new brls::Activity(view));
                } else {
                    picker->append(std::move(streams), pending);
                }
            },
            [picker]() { picker->finish(); },
            [this, alive, grid, index](const std::string& e) {
                if (!*alive) return;
                this->finishLoading(grid, index);
                brls::Application::notify("Stream error: " + e);
            });
    }

    void clearData() override { this->list.clear(); }

private:
    void finishLoading(RecyclingGrid* grid, size_t index) {
        this->loading = false;
        this->loadingIndex = static_cast<size_t>(-1);
        if (auto* cell = dynamic_cast<EpisodeCell*>(grid->getGridItemByIndex(index))) {
            const auto& v = this->list.at(index);
            std::string date = v.released.size() >= 10 ? v.released.substr(0, 10) : "";
            cell->labelOverview->setText(date.empty() ? v.overview
                : (v.overview.empty() ? date : date + "  ·  " + v.overview));
        }
    }
    std::shared_ptr<bool> alive = std::make_shared<bool>(true);
    bool loading = false;
    size_t loadingIndex = static_cast<size_t>(-1);
    std::string seriesName;
    std::string background;
    std::vector<stremio::Video> list;
};

// Resolves and queues episodes one by one. Its callbacks retain the batch, so
// leaving the season screen does not cut a season download short.
class AutoSeasonBatch : public std::enable_shared_from_this<AutoSeasonBatch> {
public:
    AutoSeasonBatch(std::string name, std::string id, std::vector<stremio::Video> videos,
        std::function<void(size_t)> finished)
        : seriesName(std::move(name)), seriesId(std::move(id)), episodes(std::move(videos)),
          finished(std::move(finished)) {}

    void next() {
        auto& manager = DownloadManager::instance();
        while (this->index < this->episodes.size()) {
            const auto& episode = this->episodes[this->index];
            if (!episode.id.empty() && manager.findItem(episode.id) == DownloadStatus::NotFound) break;
            ++this->index;
        }
        if (this->index >= this->episodes.size()) {
            if (this->finished) this->finished(this->unavailable);
            if (this->unavailable)
                brls::Application::notify(fmt::format("{} episodes had no playable stream", this->unavailable));
            return;
        }

        auto episode = this->episodes[this->index];
        auto self = this->shared_from_this();
        stremio::getStreams("series", episode.id,
            [self, episode](std::vector<stremio::Stream> streams) {
                auto playable = std::find_if(streams.begin(), streams.end(), [](const stremio::Stream& s) {
                    return !s.url.empty() && s.name.find("⚡") != std::string::npos;
                });
                if (playable == streams.end())
                    playable = std::find_if(streams.begin(), streams.end(), [](const stremio::Stream& s) {
                        return !s.url.empty();
                    });
                if (playable == streams.end()) {
                    ++self->unavailable;
                } else {
                    DownloadItem item;
                    item.id = episode.id;
                    item.name = fmt::format("{} · S{}E{} · {}",
                        self->seriesName, episode.season, episode.episode, episode.name);
                    item.streamType = "series";
                    item.seriesId = self->seriesId;
                    item.seriesName = self->seriesName;
                    item.seasonIndex = episode.season;
                    item.episodeIndex = episode.episode;
                    item.poster = episode.thumbnail;
                    item.qualityLabel = stremio::parseStream(*playable).title;
                    item.sourceUrl = playable->url;
                    // Avoid a separate HEAD: some debrid links are single-use.
                    // The manager obtains the size from the GET itself.
                    DownloadManager::instance().addDownload(item);
                }
                ++self->index;
                self->next();
            },
            [self](const std::string& error) {
                brls::Logger::warning("Season download stream lookup: {}", error);
                ++self->unavailable;
                ++self->index;
                self->next();
            });
    }

private:
    std::string seriesName;
    std::string seriesId;
    std::vector<stremio::Video> episodes;
    std::function<void(size_t)> finished;
    size_t index = 0;
    size_t unavailable = 0;
};

}  // namespace

// ---- StremioSeries: lists seasons ----------------------------------------
void StremioSeries::init() {
    this->setAxis(brls::Axis::COLUMN);
    this->setDimensions(brls::Application::contentWidth, brls::Application::contentHeight);
    this->setBackgroundColor(brls::Application::getTheme()["brls/background"]);
    this->setPadding(20, 40, 20, 40);

    this->recycler = new RecyclingGrid();
    this->recycler->setGrow(1.0f);
    this->recycler->setScrollingIndicatorVisible(false);
    this->recycler->spanCount = 1;
    this->recycler->estimatedRowHeight = 60;
    this->recycler->estimatedRowSpace = 10;
    this->recycler->registerCell("Cell", []() -> RecyclingGridItem* { return new SeasonCell(); });
    this->addView(this->recycler);
    brls::sync([this]() { brls::Application::giveFocus(this->recycler); });

    this->registerAction("hints/back"_i18n, brls::BUTTON_B, [](brls::View*) {
        brls::Application::popActivity();
        return true;
    });
}

void StremioSeries::setSeasons(
    const std::string& name, const std::vector<stremio::Video>& videos, const std::string& background) {
    if (videos.empty()) {
        this->recycler->setError("No episodes found");
        return;
    }
    // Group episodes by season (std::map keeps seasons in order).
    std::map<int, std::vector<stremio::Video>> bySeason;
    for (auto& v : videos) bySeason[v.season].push_back(v);

    std::vector<SeasonEntry> seasons;
    for (auto& kv : bySeason) {
        if (kv.first == 0) continue;  // skip specials (season 0)
        seasons.push_back({fmt::format("Season {}", kv.first), kv.second});
    }
    if (seasons.empty()) {
        this->recycler->setError("No episodes found");
        return;
    }
    this->recycler->setDataSource(new SeasonSource(name, background, std::move(seasons)));
    brls::Application::giveFocus(this->recycler);
}

StremioSeries::StremioSeries(const stremio::Meta& series) {
    brls::Logger::debug("StremioSeries: create {}", series.id);
    this->init();
    this->recycler->showSkeleton();

    std::string seriesName = series.name;
    ASYNC_RETAIN
    stremio::getJSON<stremio::MetaResult>(
        [ASYNC_TOKEN, seriesName](stremio::MetaResult r) {
            ASYNC_RELEASE
            // Prefer the (English) name from the meta response; fall back to the
            // catalog name we were opened with.
            this->setSeasons(r.meta.name.empty() ? seriesName : r.meta.name, r.meta.videos, r.meta.background);
        },
        [ASYNC_TOKEN, seriesName](const std::string& e) {
            ASYNC_RELEASE
            brls::Logger::warning("series meta fetch failed for {}: {}", seriesName, e);
            this->recycler->setError(e);
        },
        stremio::CINEMETA + "/meta/series/" + series.id + ".json");
}

StremioSeries::StremioSeries(
    const std::string& name, const std::vector<stremio::Video>& videos, const std::string& background) {
    brls::Logger::debug("StremioSeries: create from {} prefetched episodes", videos.size());
    this->init();
    this->setSeasons(name, videos, background);
}

// ---- StremioSeason: lists episodes of one season -------------------------
StremioSeason::StremioSeason(const std::string& seriesName, const std::string& title,
    const std::vector<stremio::Video>& episodes, const std::string& background)
    : seriesName(seriesName), background(background), episodes(episodes) {
    brls::Logger::debug("StremioSeason: {} ({} eps)", title, episodes.size());
    this->setAxis(brls::Axis::COLUMN);
    this->setDimensions(brls::Application::contentWidth, brls::Application::contentHeight);
    this->setBackgroundColor(brls::Application::getTheme()["brls/background"]);
    this->setPadding(20, 40, 20, 40);

    if (!episodes.empty()) this->seriesId = episodes.front().id.substr(0, episodes.front().id.find(':'));
    auto* downloadButton = new brls::Box();
    downloadButton->setFocusable(true);
    downloadButton->setPadding(12, 20, 12, 20);
    downloadButton->setMarginBottom(8);
    downloadButton->setCornerRadius(6);
    downloadButton->setBackgroundColor(theme::BG_BUTTON);
    this->downloadLabel = new brls::Label();
    this->downloadLabel->setText("⬇  Download season");
    this->downloadLabel->setFontSize(22);
    downloadButton->addView(this->downloadLabel);
    downloadButton->registerClickAction([this](brls::View*) {
        this->startSeasonDownload();
        return true;
    });
    this->addView(downloadButton);

    this->progressLabel = new brls::Label();
    this->progressLabel->setFontSize(17);
    this->progressLabel->setTextColor(theme::TEXT_SECONDARY);
    this->progressLabel->setMarginBottom(12);
    this->addView(this->progressLabel);

    this->recycler = new RecyclingGrid();
    this->recycler->setGrow(1.0f);
    this->recycler->setScrollingIndicatorVisible(false);
    this->recycler->spanCount = 1;
    this->recycler->isFlowMode = true;
    this->recycler->estimatedRowSpace = 10;
    this->recycler->registerCell("Cell", []() -> RecyclingGridItem* { return new EpisodeCell(); });
    this->addView(this->recycler);

    this->registerAction("hints/back"_i18n, brls::BUTTON_B, [](brls::View*) {
        brls::Application::popActivity();
        return true;
    });

    this->recycler->setDataSource(new EpisodeSource(seriesName, background, episodes));
    this->statusSubscription = DownloadManager::instance().getStatusEvent()->subscribe(
        [this](std::string, DownloadStatus) { this->refreshProgress(); });
    this->refreshProgress();
    brls::sync([this]() { brls::Application::giveFocus(this->recycler); });
}

void StremioSeason::willAppear(bool resetState) {
    brls::Box::willAppear(resetState);
    this->refreshWatchedBadges();
}

void StremioSeason::refreshWatchedBadges() {
    auto& watched = WatchedTracker::instance();
    for (RecyclingGridItem* view : this->recycler->getGridItems()) {
        auto* cell = dynamic_cast<EpisodeCell*>(view);
        if (!cell) continue;
        size_t index = cell->getIndex();
        if (index >= this->episodes.size()) continue;
        if (watched.isWatched(this->episodes[index].id)) {
            cell->badgeTopRight->setImageFromSVGRes("icon/ico-checkmark.svg");
            cell->badgeTopRight->setVisibility(brls::Visibility::VISIBLE);
        } else {
            cell->badgeTopRight->setVisibility(brls::Visibility::GONE);
        }
    }
}

StremioSeason::~StremioSeason() {
    *this->alive = false;
    DownloadManager::instance().getStatusEvent()->unsubscribe(this->statusSubscription);
}

void StremioSeason::refreshProgress() {
    auto counts = DownloadManager::instance().findSeries(this->seriesId);
    this->progressLabel->setText(fmt::format("Series downloads: {} complete / {} queued or saved",
        counts.second, counts.first));
}

void StremioSeason::startSeasonDownload() {
    if (this->seasonRunning || this->episodes.empty()) return;
    if (stremio::STREAM_ADDONS.empty()) {
        brls::Application::notify("Set your stream addon first (press − on the home screen)");
        return;
    }
    this->automatic = AppConfig::instance().getItem<bool>(AppConfig::SEASON_DOWNLOAD_AUTO, true);
    size_t remaining = 0;
    for (const auto& episode : this->episodes)
        if (!episode.id.empty() && DownloadManager::instance().findItem(episode.id) == DownloadStatus::NotFound)
            ++remaining;
    if (!remaining) {
        brls::Application::notify("All episodes are already in Downloads");
        return;
    }
    Dialog::cancelable(fmt::format("Download {} episodes from this season? Stream selection: {}.",
        remaining, this->automatic ? "automatic" : "manual"), [this]() {
        this->seasonRunning = true;
        this->nextEpisode = 0;
        this->unavailable = 0;
        if (this->automatic) {
            this->downloadLabel->setText("Queueing season episodes...");
            auto alive = this->alive;
            auto batch = std::make_shared<AutoSeasonBatch>(this->seriesName, this->seriesId,
                this->episodes, [this, alive](size_t) {
                    if (!*alive) return;
                    this->seasonRunning = false;
                    this->downloadLabel->setText("⬇  Download season");
                    this->refreshProgress();
                });
            batch->next();
            return;
        }
        this->queueNextEpisode();
    });
}

void StremioSeason::queueNextEpisode() {
    auto& manager = DownloadManager::instance();
    while (this->nextEpisode < this->episodes.size()) {
        const auto& episode = this->episodes[this->nextEpisode];
        if (!episode.id.empty() && manager.findItem(episode.id) == DownloadStatus::NotFound) break;
        ++this->nextEpisode;
    }
    if (this->nextEpisode >= this->episodes.size()) {
        this->seasonRunning = false;
        this->downloadLabel->setText("⬇  Download season");
        this->refreshProgress();
        if (this->unavailable)
            brls::Application::notify(fmt::format("{} episodes had no playable stream", this->unavailable));
        return;
    }

    const auto episode = this->episodes[this->nextEpisode];
    this->downloadLabel->setText(fmt::format("Finding stream for episode {}...", episode.episode));
    auto picker = std::make_shared<StreamPickerHandle>();
    ASYNC_RETAIN
    stremio::getStreams("series", episode.id,
        [ASYNC_TOKEN, picker, episode](std::vector<stremio::Stream> streams, size_t pending) {
            if (*token) return;
            if (!picker->opened()) {
                bool playable = std::any_of(streams.begin(), streams.end(),
                    [](const stremio::Stream& stream) { return !stream.url.empty(); });
                if (!playable) return;
                std::string label = fmt::format("{} · S{}E{} · {}",
                    this->seriesName, episode.season, episode.episode, episode.name);
                this->downloadLabel->setText(fmt::format("Choose stream for episode {}", episode.episode));
                ResumeEntry key{"series", episode.id, label, episode.thumbnail};
                auto alive = this->alive;
                auto* view = new StreamPicker(label, streams, key, true,
                    [this, alive]() {
                        if (!*alive) return;
                        ++this->nextEpisode;
                        this->refreshProgress();
                        this->queueNextEpisode();
                    },
                    [this, alive]() {
                        if (!*alive) return;
                        this->seasonRunning = false;
                        this->downloadLabel->setText("⬇  Download season");
                    }, pending);
                picker->attach(view);
                brls::Application::pushActivity(new brls::Activity(view));
            } else {
                picker->append(std::move(streams), pending);
            }
        },
        [ASYNC_TOKEN, picker]() {
            picker->finish();
            bool opened = picker->opened();
            ASYNC_RELEASE
            if (!opened) {
                ++this->unavailable;
                ++this->nextEpisode;
                this->queueNextEpisode();
            }
        },
        [ASYNC_TOKEN](const std::string& e) {
            if (*token) return;
            brls::Logger::warning("Season download stream lookup: {}", e);
        });
}
