/*
    Stremio series screens

    StremioSeries: fetches a series' metadata and lists its SEASONS.
    StremioSeason: lists the EPISODES of one season; selecting an episode
                   fetches its streams and opens the picker.
*/
#pragma once

#include <borealis.hpp>
#include "api/stremio.hpp"
#include "utils/download.hpp"
#include <memory>

class RecyclingGrid;

class StremioSeries : public brls::Box {
public:
    // Fetches the series meta from Cinemeta, then lists its seasons.
    explicit StremioSeries(const stremio::Meta& series);
    // Uses already-fetched episodes (e.g. from the detail screen) — no refetch.
    // background: series backdrop, used when an episode thumbnail is missing.
    StremioSeries(const std::string& name, const std::vector<stremio::Video>& videos, const std::string& background);

private:
    void init();
    void setSeasons(
        const std::string& name, const std::vector<stremio::Video>& videos, const std::string& background);

    RecyclingGrid* recycler = nullptr;
};

class StremioSeason : public brls::Box {
public:
    StremioSeason(const std::string& seriesName, const std::string& title,
        const std::vector<stremio::Video>& episodes, const std::string& background);
    ~StremioSeason() override;

    void willAppear(bool resetState = false) override;

private:
    void startSeasonDownload();
    void queueNextEpisode();
    void refreshProgress();
    // Updates the "watched" checkmark on episode cells currently mounted on
    // screen -- called on willAppear (e.g. returning from watching an
    // episode) without resetting scroll position the way reloadData() would.
    void refreshWatchedBadges();

    RecyclingGrid* recycler = nullptr;
    brls::Label* downloadLabel = nullptr;
    brls::Label* progressLabel = nullptr;
    std::string seriesName;
    std::string seriesId;
    std::string background;
    std::vector<stremio::Video> episodes;
    size_t nextEpisode = 0;
    size_t unavailable = 0;
    bool seasonRunning = false;
    bool automatic = true;
    std::shared_ptr<bool> alive = std::make_shared<bool>(true);
    DownloadManager::StatusEvent::Subscription statusSubscription;
};
