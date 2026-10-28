#include "utils/autoplay.hpp"
#include "utils/config.hpp"
#include "utils/sub_loader.hpp"
#include "api/stremio.hpp"
#include "view/mpv_core.hpp"
#include "view/ui_kit.hpp"
#include "tab/remote_view.hpp"

#include <borealis/views/dialog.hpp>
#include <borealis/core/thread.hpp>

#include <algorithm>
#include <functional>
#include <memory>

AutoplayManager& AutoplayManager::instance() {
    static AutoplayManager inst;
    return inst;
}

void AutoplayManager::init() {
    if (this->inited) return;
    this->inited = true;
    MPVCore::instance().getEvent()->subscribe([this](MpvEventEnum e) {
        if (e == MpvEventEnum::END_OF_FILE) this->onEndOfFile();
    });
}

void AutoplayManager::arm(const ResumeEntry& key) {
    this->init();
    ++this->generation;  // invalidate any lookup still in flight for the old context
    this->armed = true;
    this->currentKey = key;
}

void AutoplayManager::disarm() {
    ++this->generation;
    this->armed = false;
}

void AutoplayManager::onEndOfFile() {
    if (!this->armed) return;
    this->armed = false;  // this arm is consumed either way
    if (!AppConfig::instance().getItem<bool>(AppConfig::AUTOPLAY_NEXT_EPISODE, true)) return;

    const std::string& id = this->currentKey.streamId;
    size_t p1 = id.find(':');
    size_t p2 = (p1 == std::string::npos) ? std::string::npos : id.find(':', p1 + 1);
    if (p1 == std::string::npos || p2 == std::string::npos) {
        this->giveUp();
        return;
    }

    int season = 0, episode = 0;
    try {
        season = std::stoi(id.substr(p1 + 1, p2 - p1 - 1));
        episode = std::stoi(id.substr(p2 + 1));
    } catch (...) {
        this->giveUp();
        return;
    }
    std::string base = id.substr(0, p1);
    std::string nextId = fmt::format("{}:{}:{}", base, season, episode + 1);
    this->offerNext(nextId, season, episode + 1);
}

void AutoplayManager::offerNext(const std::string& nextId, int season, int episode) {
    if (stremio::STREAM_ADDONS.empty()) {
        this->giveUp();
        return;
    }
    std::string label = fmt::format("S{}E{}", season, episode);
    int gen = this->generation;

    // NOTE: deliberately no "try next season" fallback here. An empty/failed
    // lookup for episode+1 doesn't prove the season is over -- it could just
    // as easily be a temporary addon hiccup or a filtered result for an
    // episode that does exist. Guessing wrong and jumping to a different
    // season's episode 1 would be worse than just not offering autoplay for
    // that one transition.
    stremio::getStreams("series", nextId,
        [this, gen, nextId, label](std::vector<stremio::Stream> streams) {
            if (gen != this->generation) return;  // stale: something else already took over
            const stremio::Stream* best = stremio::pickBestStream(streams);
            if (!best) {
                this->giveUp();
                return;
            }
            this->showCountdown(nextId, label, best->url);
        },
        [this, gen](const std::string&) {
            if (gen != this->generation) return;
            this->giveUp();
        });
}

void AutoplayManager::giveUp() {
    // Nothing to offer -- restore the normal end-of-file behavior that this
    // feature suppressed (see willHandle()/remote_view.cpp): close the
    // (frozen) player activity that's still sitting on top.
    brls::Application::popActivity(brls::TransitionAnimation::NONE);
}

void AutoplayManager::showCountdown(const std::string& nextId, const std::string& label, const std::string& url) {
    constexpr int kSeconds = 8;
    auto resolved = std::make_shared<bool>(false);
    auto secondsLeft = std::make_shared<int>(kSeconds);
    auto pendingDelay = std::make_shared<size_t>(0);

    auto* content = new brls::Box();
    content->setAxis(brls::Axis::COLUMN);
    content->setAlignItems(brls::AlignItems::CENTER);
    content->setPadding(20, 30, 20, 30);

    auto* title = new brls::Label();
    title->setFontSize(24);
    title->setText(fmt::format("Up next: {}", label));
    title->setHorizontalAlign(brls::HorizontalAlign::CENTER);
    content->addView(title);

    auto* counter = new brls::Label();
    counter->setFontSize(20);
    counter->setMarginTop(8);
    counter->setMarginBottom(16);
    counter->setText(fmt::format("Playing in {}s...", kSeconds));
    counter->setHorizontalAlign(brls::HorizontalAlign::CENTER);
    content->addView(counter);

    auto* buttonRow = new brls::Box();
    buttonRow->setAxis(brls::Axis::ROW);
    buttonRow->setJustifyContent(brls::JustifyContent::CENTER);
    content->addView(buttonRow);

    auto* dialog = new brls::Dialog(content);
    dialog->setCancelable(false);

    // Resolution is a single "first one wins" gate, checked/set synchronously
    // -- at the moment of a click, or the countdown reaching zero -- never
    // from inside a delayed callback that already ran its course. This is
    // what actually prevents a click and the last tick landing on the same
    // 1-second boundary from both acting: whichever runs first flips
    // `resolved` and cancels the other's pending delay immediately, all
    // still on the main thread before either does anything further.
    auto resolve = [resolved, pendingDelay](std::function<void()> action) {
        if (*resolved) return;
        *resolved = true;
        brls::cancelDelay(*pendingDelay);
        action();
    };

    buttonRow->addView(ui::pillButton("Cancel", [this, resolve, dialog]() {
        resolve([this, dialog]() { dialog->close([this]() { this->giveUp(); }); });
    }));

    brls::Box* playBtn = ui::pillButton("Play now", [this, resolve, dialog, nextId, label, url]() {
        resolve([this, dialog, nextId, label, url]() {
            dialog->close([this, nextId, label, url]() { this->startPlayback(nextId, label, url); });
        });
    });
    buttonRow->addView(playBtn);

    auto tick = std::make_shared<std::function<void()>>();
    *tick = [this, secondsLeft, counter, dialog, nextId, label, url, resolve, pendingDelay, tick]() {
        if (--(*secondsLeft) <= 0) {
            resolve([this, dialog, nextId, label, url]() {
                dialog->close([this, nextId, label, url]() { this->startPlayback(nextId, label, url); });
            });
            return;
        }
        counter->setText(fmt::format("Playing in {}s...", *secondsLeft));
        *pendingDelay = brls::delay(1000, *tick);
    };
    *pendingDelay = brls::delay(1000, *tick);

    dialog->open();
    brls::Application::giveFocus(playBtn);
}

void AutoplayManager::startPlayback(const std::string& nextId, const std::string& label, const std::string& url) {
    // Pop the finished episode's player before pushing the next one so a long
    // binge doesn't pile up one activity per episode.
    brls::Application::popActivity(brls::TransitionAnimation::NONE, [this, nextId, label, url]() {
        ResumeEntry key { "series", nextId, label, "" };
        ResumeTracker::instance().setCurrent(key);
        SubLoader::instance().prepare("series", nextId);
        std::string alang = AppConfig::instance().getItem<std::string>(AppConfig::AUDIO_LANG_PREF, "eng,en");
        MPVCore::instance().command("set", "alang", alang.c_str());
        RemoteView::play(url, label, nextId);
        this->arm(key);
    });
}
