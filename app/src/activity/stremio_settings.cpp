#include "activity/stremio_settings.hpp"
#include "utils/config.hpp"
#include "utils/download.hpp"
#include "utils/theme.hpp"
#include "view/ui_kit.hpp"

#include <algorithm>
#include <functional>
#include <vector>

using namespace brls::literals;

namespace {
// value -> display label, in cycle order. mpv's "alang" takes a
// comma-separated priority list; kept short and controller-picked (no
// on-screen keyboard) since most addons don't expose per-track language
// metadata for a smarter picker.
const std::vector<std::pair<std::string, std::string>>& audioLangSteps() {
    static const std::vector<std::pair<std::string, std::string>> steps = {
        {"eng,en", "English"},
        {"por,pt,pt-BR", "Português"},
        {"spa,es", "Español"},
        {"jpn,ja", "日本語"},
    };
    return steps;
}
}  // namespace

StremioSettings::StremioSettings() {
    this->setAxis(brls::Axis::COLUMN);
    this->setDimensions(brls::Application::contentWidth, brls::Application::contentHeight);
    this->setBackgroundColor(brls::Application::getTheme()["brls/background"]);
    this->setPadding(20, 40, 20, 40);

    auto* title = new brls::Label();
    title->setText("Settings");
    title->setFontSize(27);
    title->setMarginBottom(20);
    title->setTextColor(theme::TEXT_PRIMARY);
    this->addView(title);

    this->storageSlot = new brls::Box();
    this->storageSlot->setAxis(brls::Axis::COLUMN);
    this->addView(this->storageSlot);

    auto addRow = [this](std::function<void()> onClick) {
        ui::Row row = ui::clickableRow("", onClick);
        this->addView(row.box);
        return row.label;
    };

    this->seasonMode = addRow([this]() {
        auto& config = AppConfig::instance();
        config.setItem<bool>(AppConfig::SEASON_DOWNLOAD_AUTO,
            !config.getItem<bool>(AppConfig::SEASON_DOWNLOAD_AUTO, true));
        this->refresh();
    });
    this->autoplayMode = addRow([this]() {
        auto& config = AppConfig::instance();
        config.setItem<bool>(AppConfig::AUTOPLAY_NEXT_EPISODE,
            !config.getItem<bool>(AppConfig::AUTOPLAY_NEXT_EPISODE, true));
        this->refresh();
    });
    this->audioLangMode = addRow([this]() {
        auto& steps = audioLangSteps();
        auto& config = AppConfig::instance();
        std::string cur = config.getItem<std::string>(AppConfig::AUDIO_LANG_PREF, "eng,en");
        auto it = std::find_if(steps.begin(), steps.end(), [&](auto& p) { return p.first == cur; });
        size_t next = (it == steps.end()) ? 0 : ((it - steps.begin()) + 1) % steps.size();
        config.setItem<std::string>(AppConfig::AUDIO_LANG_PREF, steps[next].first);
        this->refresh();
    });
    // No on/off toggle for skip-intro: it's set per season, in context, by
    // marking two points during playback (ZR, twice), not a global setting --
    // see RemotePlayer in remote_view.cpp. This is just a pointer to that.
    auto* skipIntroHint = ui::textRow("Skip intro: while watching a series, press ZR where the intro "
                                       "starts, then ZR again where it ends -- saved for that season.");
    skipIntroHint->setIsWrapping(true);
    skipIntroHint->setTextColor(theme::TEXT_MUTED);
    skipIntroHint->setFontSize(16);
    this->addView(skipIntroHint);

    this->smartDownloadsMode = addRow([this]() {
        auto& config = AppConfig::instance();
        config.setItem<bool>(AppConfig::SMART_DOWNLOADS,
            !config.getItem<bool>(AppConfig::SMART_DOWNLOADS, false));
        this->refresh();
    });

    this->registerAction("hints/back"_i18n, brls::BUTTON_B, [](brls::View*) {
        brls::Application::popActivity();
        return true;
    });
    this->refresh();
}

void StremioSettings::refresh() {
    int64_t total = downloadDiskTotalBytes();
    int64_t free = downloadDiskFreeBytes();
    int64_t own = DownloadManager::instance().totalOwnBytes();
    if (total != this->shownTotal || free != this->shownFree || own != this->shownOwn) {
        this->shownTotal = total;
        this->shownFree = free;
        this->shownOwn = own;
        this->storageSlot->clearViews();
        if (total > 0 && free >= 0 && free <= total && own >= 0 && own <= total - free) {
            this->storageSlot->addView(ui::storageBar(own, total - free - own, free));
        } else {
            auto* fallback = new brls::Label();
            fallback->setText(free < 0 ? "SD: free space unavailable"
                : fmt::format("{:.1f} GB free", static_cast<double>(free) / (1024.0 * 1024.0 * 1024.0)));
            fallback->setFontSize(20);
            fallback->setMarginBottom(20);
            this->storageSlot->addView(fallback);
        }
    }
    bool automatic = AppConfig::instance().getItem<bool>(AppConfig::SEASON_DOWNLOAD_AUTO, true);
    this->seasonMode->setText(automatic ? "Season downloads: Automatic" : "Season downloads: Manual");
    bool autoplay = AppConfig::instance().getItem<bool>(AppConfig::AUTOPLAY_NEXT_EPISODE, true);
    this->autoplayMode->setText(autoplay ? "Autoplay next episode: On" : "Autoplay next episode: Off");
    std::string curLang = AppConfig::instance().getItem<std::string>(AppConfig::AUDIO_LANG_PREF, "eng,en");
    auto& steps = audioLangSteps();
    auto it = std::find_if(steps.begin(), steps.end(), [&](auto& p) { return p.first == curLang; });
    this->audioLangMode->setText(
        fmt::format("Preferred audio: {}", it == steps.end() ? curLang : it->second));
    bool smart = AppConfig::instance().getItem<bool>(AppConfig::SMART_DOWNLOADS, false);
    this->smartDownloadsMode->setText(smart ? "Smart Downloads: On (delete watched, queue next)"
                                             : "Smart Downloads: Off");
}

void StremioSettings::willAppear(bool resetState) {
    brls::Box::willAppear(resetState);
    this->refresh();
}

void StremioSettings::onChildFocusGained(brls::View* directChild, brls::View* focusedView) {
    brls::Box::onChildFocusGained(directChild, focusedView);
    this->refresh();
}
