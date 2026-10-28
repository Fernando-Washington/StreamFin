#pragma once

#include <borealis.hpp>
#include "utils/download.hpp"

class RecyclingGrid;

class StremioDownloads : public brls::Box {
public:
    StremioDownloads();
    ~StremioDownloads() override;
    void willAppear(bool resetState = false) override;
    brls::View* getDefaultFocus() override;

private:
    void refresh();
    bool rebuilding = false;
    RecyclingGrid* recycler = nullptr;
    brls::Label* emptyLabel = nullptr;
    brls::Label* summaryLabel = nullptr;
    DownloadManager::ProgressEvent::Subscription progressSubscription;
    DownloadManager::StatusEvent::Subscription statusSubscription;
};

// One series' downloaded episodes: poster/summary header, then the episode
// list (same row style as the flat Downloads screen), opened by tapping a
// series' row there instead of listing every episode inline.
class StremioDownloadedSeries : public brls::Box {
public:
    StremioDownloadedSeries(std::string seriesId, std::string seriesName, std::string poster);
    ~StremioDownloadedSeries() override;
    void willAppear(bool resetState = false) override;
    brls::View* getDefaultFocus() override;

private:
    void refresh();
    std::string seriesId;
    bool rebuilding = false;
    RecyclingGrid* recycler = nullptr;
    brls::Label* summaryLabel = nullptr;
    DownloadManager::ProgressEvent::Subscription progressSubscription;
    DownloadManager::StatusEvent::Subscription statusSubscription;
};
