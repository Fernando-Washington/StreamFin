#include "activity/stremio_addons.hpp"
#include "api/stremio.hpp"
#include "utils/config.hpp"
#include "utils/theme.hpp"
#include "view/ui_kit.hpp"

using namespace brls::literals;

namespace {

std::string addonHost(const std::string& url) {
    size_t scheme = url.find("://");
    size_t start = scheme == std::string::npos ? 0 : scheme + 3;
    size_t end = url.find_first_of("/?#", start);
    std::string host = url.substr(start, end == std::string::npos ? std::string::npos : end - start);
    size_t credentials = host.rfind('@');
    if (credentials != std::string::npos) host.erase(0, credentials + 1);
    return host.empty() ? "(invalid URL)" : host;
}

}  // namespace

StremioAddons::StremioAddons() {
    this->setAxis(brls::Axis::COLUMN);
    this->setDimensions(brls::Application::contentWidth, brls::Application::contentHeight);
    this->setBackgroundColor(brls::Application::getTheme()["brls/background"]);
    this->setPadding(20, 40, 20, 40);

    auto* title = new brls::Label();
    title->setText("Addons");
    title->setFontSize(27);
    title->setMarginBottom(4);
    title->setTextColor(theme::TEXT_PRIMARY);
    this->addView(title);

    auto* hint = new brls::Label();
    hint->setText("What StreamFin actually loaded (from streamfin-addon.txt, the SD-card drop, or manual entry). "
                   "To change these, edit that file or press - on the Home screen, then come back and reload.");
    hint->setFontSize(16);
    hint->setTextColor(theme::TEXT_MUTED);
    this->addView(hint);

    // Rebuilt fresh every reload -- keep a pointer to the section container so
    // "Reload" can clear and repopulate it instead of building a whole new
    // screen.
    auto* list = new brls::Box();
    list->setAxis(brls::Axis::COLUMN);
    list->setMarginTop(12);
    this->addView(list);

    auto populate = [list]() {
        list->clearViews();

        list->addView(ui::sectionHeader(fmt::format("Stream addons ({}) -- resolve \"Watch\"/\"Download\" links",
            stremio::STREAM_ADDONS.size())));
        if (stremio::STREAM_ADDONS.empty()) {
            list->addView(ui::textRow("(none configured)"));
        } else {
            for (auto& url : stremio::STREAM_ADDONS) list->addView(ui::textRow(addonHost(url)));
        }

        list->addView(ui::sectionHeader(
            fmt::format("Catalog addons ({}) -- show up as rows on Home", stremio::CATALOG_ADDONS.size())));
        if (stremio::CATALOG_ADDONS.empty()) {
            list->addView(ui::textRow("(none configured)"));
        } else {
            for (auto& url : stremio::CATALOG_ADDONS) list->addView(ui::textRow(addonHost(url)));
        }

        list->addView(ui::sectionHeader("Subtitles addon"));
        list->addView(ui::textRow(stremio::SUBTITLES_ADDON.empty()
            ? "(none configured)" : addonHost(stremio::SUBTITLES_ADDON)));
    };
    populate();

    ui::Row reload = ui::clickableRow("Reload from file (streamfin-addon.txt / addon.txt)", [populate]() {
        std::string dir = AppConfig::instance().configDir();
        stremio::importAddonFromFile(dir);
        stremio::refreshCatalogAddons(dir);
        populate();
        brls::Application::notify("Reloaded");
    });
    reload.box->setMarginTop(20);
    reload.box->setJustifyContent(brls::JustifyContent::CENTER);
    reload.label->setFontSize(20);
    this->addView(reload.box);

    this->registerAction("hints/back"_i18n, brls::BUTTON_B, [](brls::View*) {
        brls::Application::popActivity();
        return true;
    });
}
