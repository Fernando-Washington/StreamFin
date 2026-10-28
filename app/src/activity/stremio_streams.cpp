/*
    Stremio stream picker -- implementation.

    A compact, scrollable, selectable list of streams. Uses the file-list row
    layout (small static icon + text, no thumbnails to load) so it scrolls fast.
*/
#include "activity/stremio_streams.hpp"
#include "activity/stremio_resume.hpp"
#include "view/recycling_grid.hpp"
#include "view/svg_image.hpp"
#include "view/mpv_core.hpp"
#include "tab/remote_view.hpp"
#include "api/stremio.hpp"
#include "utils/autoplay.hpp"
#include "utils/config.hpp"
#include "utils/download.hpp"
#include "utils/theme.hpp"
#include "utils/dialog.hpp"
#include "utils/misc.hpp"
#include "utils/sub_loader.hpp"

#include <algorithm>

using namespace brls::literals;

namespace {

// Clean text row built in code: name on top, full description below.
class StreamCell : public RecyclingGridItem {
public:
    explicit StreamCell(bool downloadMode) {
        this->setFocusable(true);
        this->setAxis(brls::Axis::COLUMN);
        this->setPadding(12, 20, 12, 20);
        this->setCornerRadius(6);

        this->name = new brls::Label();
        this->name->setFontSize(24);
        this->addView(this->name);

        this->detail = new brls::Label();
        this->detail->setFontSize(18);
        this->addView(this->detail);

        // A already downloads in download mode; only watch mode offers X as
        // a secondary download action. Cells are recycled within one picker.
        if (!downloadMode)
            this->registerAction("Download", brls::BUTTON_X, [this](brls::View*) {
                if (this->onDownload) this->onDownload();
                return true;
            });
    }

    brls::Label* name = nullptr;
    brls::Label* detail = nullptr;
    std::function<void()> onDownload;
};

class StreamSource : public RecyclingGridDataSource {
public:
    StreamSource(const ResumeEntry& key, const std::vector<stremio::Stream>& s, bool downloadMode,
        std::function<void()> onDownloadQueued, std::function<void()> onChosen)
        : key(key), list(s), downloadMode(downloadMode), onDownloadQueued(std::move(onDownloadQueued)),
          onChosen(std::move(onChosen)) {}

    size_t getItemCount() override { return this->list.size(); }

    void append(std::vector<stremio::Stream> streams) {
        for (auto& stream : streams) this->list.push_back(std::move(stream));
    }

    RecyclingGridItem* cellForRow(RecyclingView* recycler, size_t index) override {
        StreamCell* cell = dynamic_cast<StreamCell*>(recycler->dequeueReusableCell("Cell"));
        auto& s = this->list.at(index);

        // parseStream() pulls provider/resolution/codec/audio/language/quality
        // tags out of the (often huge, emoji-laden) addon description. Fold
        // everything it found into one clean detail line; when it recognized
        // nothing, fall back to the raw description exactly as before.
        stremio::StreamInfo info = stremio::parseStream(s);

        std::vector<std::string> parts;
        parts.insert(parts.end(), info.meta.begin(), info.meta.end());
        parts.insert(parts.end(), info.audio.begin(), info.audio.end());
        if (!info.langs.empty()) {
            std::string langs;
            for (size_t i = 0; i < info.langs.size(); i++) {
                if (i) langs += "/";
                langs += info.langs[i];
            }
            parts.push_back(langs);
        }
        if (!info.badges.empty()) {
            std::string badges;
            for (size_t i = 0; i < info.badges.size(); i++) {
                if (i) badges += " ";
                badges += info.badges[i];
            }
            parts.push_back(badges);
        }
        if (!info.source.empty()) parts.push_back(info.source);

        std::string desc;
        for (size_t i = 0; i < parts.size(); i++) {
            if (i) desc += "  ·  ";
            desc += parts[i];
        }
        if (desc.empty()) {
            desc = !s.description.empty() ? s.description : s.title;
            std::replace(desc.begin(), desc.end(), '\n', ' ');
        }

        cell->name->setText(info.title);
        cell->detail->setText(desc);
        // In downloadMode the primary action (A) already downloads; the X
        // shortcut only makes sense as a secondary action in Watch mode.
        if (this->downloadMode)
            cell->onDownload = nullptr;
        else
            cell->onDownload = [this, s]() { this->startDownload(s); };
        return cell;
    }

    // Checks size + free space, confirms with the user, then queues it.
    // Runs the HEAD request off the main thread since it blocks.
    void startDownload(const stremio::Stream& s) {
        if (s.url.empty()) {
            brls::Application::notify("This stream has no URL");
            return;
        }
        if (DownloadManager::instance().findItem(this->key.streamId) != DownloadStatus::NotFound) {
            brls::Application::notify("Already downloading or downloaded");
            return;
        }

        if (this->onChosen) this->onChosen();
        ResumeEntry key = this->key;
        std::string url = s.url;
        std::string qualityLabel = stremio::parseStream(s).title;
        auto onQueued = this->onDownloadQueued;
        auto activities = brls::Application::getActivitiesStack();
        auto* pickerActivity = activities.empty() ? nullptr : activities.back();

        brls::Application::notify("Checking size…");
        brls::async([key, url, qualityLabel, onQueued, pickerActivity]() {
            int64_t size = remoteContentLength(url);
            brls::sync([key, url, qualityLabel, size, onQueued, pickerActivity]() {
                // A slow HEAD must not open a confirmation over a different
                // screen after the user has already left this picker.
                auto stack = brls::Application::getActivitiesStack();
                if (stack.empty() || stack.back() != pickerActivity) return;
                int64_t free = downloadDiskFreeBytes();
                std::string sizeStr = size > 0 ? misc::formatSize(size) : "unknown size";

                if (size > 0 && free >= 0 && size > free) {
                    Dialog::show(fmt::format(
                        "Not enough space on the SD card: needs {}, only {} free.", sizeStr, misc::formatSize(free)));
                    return;
                }

                std::string prompt = free >= 0 ? fmt::format("Download this stream? {} — {} free on SD card.",
                                                      sizeStr, misc::formatSize(free))
                                                : fmt::format("Download this stream? {}.", sizeStr);

                Dialog::cancelable(prompt, [key, url, qualityLabel, size, onQueued, pickerActivity]() {
                    DownloadItem item;
                    item.id = key.streamId;
                    item.name = key.name;
                    item.streamType = key.streamType;
                    item.poster = key.poster;
                    item.qualityLabel = qualityLabel;
                    item.sourceUrl = url;
                    item.totalBytes = size > 0 ? size : 0;

                    if (key.streamType == "series") {
                        // streamId is "ttID:season:episode"; name is
                        // "Series · S1E5 · Episode" (see enrichContinue).
                        size_t p1 = key.streamId.find(':');
                        if (p1 != std::string::npos) {
                            item.seriesId = key.streamId.substr(0, p1);
                            size_t p2 = key.streamId.find(':', p1 + 1);
                            if (p2 != std::string::npos) {
                                try {
                                    item.seasonIndex = std::stoi(key.streamId.substr(p1 + 1, p2 - p1 - 1));
                                    item.episodeIndex = std::stoi(key.streamId.substr(p2 + 1));
                                } catch (...) {
                                }
                            }
                        }
                        size_t sep = key.name.find(" · ");
                        item.seriesName = sep != std::string::npos ? key.name.substr(0, sep) : key.name;
                    }

                    DownloadManager::instance().addDownload(item);
                    brls::Application::notify("Download queued");
                    auto stack = brls::Application::getActivitiesStack();
                    if (onQueued && !stack.empty() && stack.back() == pickerActivity)
                        brls::Application::popActivity(brls::TransitionAnimation::NONE, onQueued);
                });
            });
        });
    }

    void onItemSelected(brls::Box* recycler, size_t index) override {
        auto s = this->list.at(index);
        if (s.url.empty()) {
            brls::Application::notify("This stream has no URL");
            return;
        }
        if (this->downloadMode) {
            this->startDownload(s);
            return;
        }
        if (this->onChosen) this->onChosen();
        // Remember what we're playing so the tracker can resume/record by id.
        ResumeTracker::instance().setCurrent(this->key);
        // Ask the subtitles addon (if configured) for subs for this title.
        SubLoader::instance().prepare(this->key.streamType, this->key.streamId);
        // Apply the user's preferred audio language for this (and subsequent) playback.
        std::string alang = AppConfig::instance().getItem<std::string>(AppConfig::AUDIO_LANG_PREF, "eng,en");
        MPVCore::instance().command("set", "alang", alang.c_str());
        // Show the media title (movie name / "Series · S1E5 · Episode") on the
        // player OSD, not the raw stream/source name.
        std::string title = this->key.name.empty() ? s.name : this->key.name;
        RemoteView::play(s.url, title, this->key.streamId);
        if (this->key.streamType == "series") {
            AutoplayManager::instance().arm(this->key);
        } else {
            AutoplayManager::instance().disarm();
        }
    }

    void clearData() override { this->list.clear(); }

private:
    ResumeEntry key;
    std::vector<stremio::Stream> list;
    bool downloadMode;
    std::function<void()> onDownloadQueued;
    std::function<void()> onChosen;
};

}  // namespace

StreamPicker::StreamPicker(const std::string& title, const std::vector<stremio::Stream>& streams,
    const ResumeEntry& resumeKey, bool downloadMode, std::function<void()> onDownloadQueued,
    std::function<void()> onDownloadCancelled, size_t pendingSources)
    : key(resumeKey), downloadMode(downloadMode), onDownloadQueued(std::move(onDownloadQueued)) {
    brls::Logger::debug("StreamPicker: {} streams", streams.size());
    this->setAxis(brls::Axis::COLUMN);
    this->setDimensions(brls::Application::contentWidth, brls::Application::contentHeight);
    this->setBackgroundColor(brls::Application::getTheme()["brls/background"]);
    this->setPadding(20, 40, 20, 40);

    // Explicit about which mode this screen is in -- no more guessing whether
    // picking a row plays it or downloads it.
    auto* header = new brls::Label();
    header->setText(downloadMode ? "⬇  Choose what to download" : "▶  Choose what to watch");
    header->setFontSize(24);
    header->setMarginBottom(14);
    header->setMarginLeft(6);
    header->setTextColor(theme::TEXT_PRIMARY);
    this->addView(header);

    this->recycler = new RecyclingGrid();
    this->recycler->setGrow(1.0f);
    this->recycler->setScrollingIndicatorVisible(false);
    this->recycler->spanCount = 1;
    this->recycler->isFlowMode = true;
    this->recycler->estimatedRowSpace = 10;
    this->recycler->registerCell("Cell", [downloadMode]() -> RecyclingGridItem* {
        return new StreamCell(downloadMode);
    });
    this->addView(this->recycler);

    this->progressLabel = new brls::Label();
    this->progressLabel->setFontSize(16);
    this->progressLabel->setTextColor(theme::TEXT_SECONDARY);
    this->progressLabel->setMarginTop(8);
    this->addView(this->progressLabel);

    this->appendStreams(streams, pendingSources);
    if (pendingSources == 0) this->finishLoading();

    // Defer focus until after the activity is on screen. The lifetime token
    // also protects this callback if the user closes the picker immediately.
    brls::sync([this, alive = this->alive]() {
        auto stack = brls::Application::getActivitiesStack();
        if (*alive && this->source && !stack.empty() && stack.back()->getContentView() == this)
            brls::Application::giveFocus(this->recycler);
    });

    this->registerAction("hints/back"_i18n, brls::BUTTON_B, [onDownloadCancelled](brls::View*) {
        if (onDownloadCancelled) onDownloadCancelled();
        brls::Application::popActivity();
        return true;
    });
}

StreamPicker::~StreamPicker() { *this->alive = false; }

void StreamPicker::appendStreams(std::vector<stremio::Stream> streams, size_t pendingSources) {
    if (!this->acceptingUpdates) return;

    std::vector<stremio::Stream> playable;
    for (auto& stream : streams) {
        if (!stream.url.empty())
            playable.push_back(std::move(stream));
        else if (!stream.infoHash.empty())
            this->torrentOnly = true;
    }

    if (!playable.empty()) {
        if (!this->source) {
            auto* next = new StreamSource(this->key, playable, this->downloadMode, this->onDownloadQueued,
                [this]() {
                    this->acceptingUpdates = false;
                    this->progressLabel->setVisibility(brls::Visibility::GONE);
                });
            this->recycler->setDataSource(next);
            this->source = next;
            brls::sync([this, alive = this->alive]() {
                auto stack = brls::Application::getActivitiesStack();
                if (*alive && this->acceptingUpdates && !stack.empty() &&
                    stack.back()->getContentView() == this)
                    brls::Application::giveFocus(this->recycler);
            });
        } else {
            static_cast<StreamSource*>(this->source)->append(std::move(playable));
            // The grid extends its height without rebuilding mounted cells or
            // resetting its focus/scroll position.
            this->recycler->notifyDataChanged();
        }
    } else if (!this->source && !this->emptyHintShown) {
        this->recycler->setEmpty("Waiting for playable streams...");
        this->emptyHintShown = true;
    }

    if (pendingSources > 0) {
        this->progressLabel->setText("Searching more sources...");
        this->progressLabel->setVisibility(brls::Visibility::VISIBLE);
    } else {
        this->progressLabel->setVisibility(brls::Visibility::GONE);
    }
}

void StreamPicker::finishLoading() {
    this->progressLabel->setVisibility(brls::Visibility::GONE);
    if (this->source) return;
    this->recycler->setEmpty(
        this->torrentOnly ? "These are raw torrent streams, which this app can't play.\n"
                            "Configure your addon with a debrid service (Real-Debrid, Premiumize, "
                            "AllDebrid...) so it returns direct links."
                          : "No playable streams - check your addon's quality/resolution filters");
}

void StreamPickerHandle::attach(StreamPicker* picker) {
    this->view = picker;
    this->alive = picker->lifetimeToken();
}

void StreamPickerHandle::append(std::vector<stremio::Stream> streams, size_t pendingSources) {
    auto token = this->alive.lock();
    if (token && *token) this->view->appendStreams(std::move(streams), pendingSources);
}

void StreamPickerHandle::finish() {
    auto token = this->alive.lock();
    if (token && *token) this->view->finishLoading();
}
