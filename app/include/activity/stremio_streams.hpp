/*
    Stremio stream picker

    A scrollable, selectable list of streams for a chosen movie/episode. Built
    on the same RecyclingGrid that the rest of the app uses (so it works with
    both touch and the D-pad). Selecting a row plays that stream.
*/
#pragma once

#include <borealis.hpp>
#include "api/stremio.hpp"
#include "activity/stremio_resume.hpp"
#include <functional>
#include <memory>

class RecyclingGrid;
class RecyclingGridDataSource;

class StreamPicker : public brls::Box {
public:
    // downloadMode: entered via the Detail screen's explicit "Download"
    // button (as opposed to "Watch") -- selecting a row downloads it instead
    // of playing it, and the header/hint say so up front instead of relying
    // on a hidden secondary button.
    StreamPicker(const std::string& title, const std::vector<stremio::Stream>& streams, const ResumeEntry& resumeKey,
        bool downloadMode = false, std::function<void()> onDownloadQueued = {},
        std::function<void()> onDownloadCancelled = {}, size_t pendingSources = 0);
    ~StreamPicker() override;

    void appendStreams(std::vector<stremio::Stream> streams, size_t pendingSources);
    void finishLoading();
    std::shared_ptr<bool> lifetimeToken() const { return alive; }

private:
    RecyclingGrid* recycler = nullptr;
    RecyclingGridDataSource* source = nullptr;  // owned by recycler
    brls::Label* progressLabel = nullptr;
    ResumeEntry key;
    bool downloadMode = false;
    bool torrentOnly = false;
    bool acceptingUpdates = true;
    bool emptyHintShown = false;
    std::function<void()> onDownloadQueued;
    std::shared_ptr<bool> alive = std::make_shared<bool>(true);
};

// Callbacks can outlive the picker; this handle never dereferences it once its
// Activity has been destroyed, and never reopens a picker the user closed.
struct StreamPickerHandle {
    void attach(StreamPicker* picker);
    bool opened() const { return view != nullptr; }
    void append(std::vector<stremio::Stream> streams, size_t pendingSources);
    void finish();

private:
    StreamPicker* view = nullptr;
    std::weak_ptr<bool> alive;
};
