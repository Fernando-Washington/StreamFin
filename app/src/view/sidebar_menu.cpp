#include "view/sidebar_menu.hpp"
#include "activity/stremio_addons.hpp"
#include "activity/stremio_downloads.hpp"
#include "activity/stremio_settings.hpp"
#include "utils/theme.hpp"

const char* const SIDEBAR_MENU_FIRST_ITEM_ID = "sidebar-menu/item-0";
const char* const SIDEBAR_MENU_RETURN_ID     = "home/first-row";
const char* const HOME_ROW_FIRST_CARD_ID     = "home/row-first-card";

namespace {

brls::Box* makeItem(const std::string& text, const std::string& id) {
    auto* item = new brls::Box();
    item->setId(id);
    item->setFocusable(true);
    item->setHeight(56);
    item->setMarginBottom(12);
    item->setCornerRadius(8);
    item->setPadding(0, 18, 0, 18);
    item->setJustifyContent(brls::JustifyContent::CENTER);

    auto* label = new brls::Label();
    label->setText(text);
    label->setFontSize(20);
    label->setTextColor(theme::TEXT_PRIMARY);
    item->addView(label);

    return item;
}

}  // namespace

SidebarMenu::SidebarMenu() {
    this->setAxis(brls::Axis::COLUMN);
    this->setPositionType(brls::PositionType::ABSOLUTE);
    this->setPositionTop(0);
    this->setPositionLeft(0);
    this->setWidth(WIDTH);
    this->setHeight(brls::Application::contentHeight);
    this->setBackgroundColor(theme::BG_PANEL);
    this->setPadding(32, 20, 32, 20);
    // Parked off-screen to the left until a row's first card routes focus in.
    this->setTranslationX(-WIDTH);

    auto* header = new brls::Label();
    header->setText("Menu");
    header->setFontSize(24);
    header->setMarginBottom(20);
    header->setTextColor(theme::TEXT_PRIMARY);
    this->addView(header);

    // Each entry routes
    // RIGHT back out of the drawer (see header comment on SIDEBAR_MENU_RETURN_ID).
    brls::Box* items[] = {
        makeItem("Downloads", SIDEBAR_MENU_FIRST_ITEM_ID),
        makeItem("Settings", "sidebar-menu/item-1"),
        makeItem("Addons", "sidebar-menu/item-2"),
    };
    items[0]->registerClickAction([](brls::View*) {
        brls::Application::pushActivity(new brls::Activity(new StremioDownloads()));
        return true;
    });
    items[1]->registerClickAction([](brls::View*) {
        brls::Application::pushActivity(new brls::Activity(new StremioSettings()));
        return true;
    });
    items[2]->registerClickAction([](brls::View*) {
        brls::Application::pushActivity(new brls::Activity(new StremioAddons()));
        return true;
    });
    for (auto* item : items) {
        item->setCustomNavigationRoute(brls::FocusDirection::RIGHT, std::string(SIDEBAR_MENU_RETURN_ID));
        this->addView(item);
    }
}

void SidebarMenu::slideTo(float target) {
    this->slideAnim.stop();
    this->slideAnim.reset(target == 0 ? -WIDTH : 0);
    this->slideAnim.addStep(target, 200, brls::EasingFunction::quadraticOut);
    this->slideAnim.setTickCallback([this] { this->setTranslationX(this->slideAnim.getValue()); });
    this->slideAnim.start();
}

void SidebarMenu::onChildFocusGained(brls::View* directChild, brls::View* focusedView) {
    brls::Box::onChildFocusGained(directChild, focusedView);
    // Also fires when focus moves BETWEEN items already inside the drawer
    // (e.g. Up/Down from Item 1 to Item 2) -- only animate on a real open.
    if (this->isOpen) return;
    this->isOpen = true;
    this->slideTo(0);
}

void SidebarMenu::onChildFocusLost(brls::View* directChild, brls::View* focusedView) {
    brls::Box::onChildFocusLost(directChild, focusedView);
    // At this point Application::currentFocus is still the view that's
    // LOSING focus -- the new one isn't assigned until after this call
    // returns (see Application::giveFocus). Check next frame instead of
    // here, otherwise moving focus between items inside the drawer (which
    // fires this exact callback for the outgoing item) would always read as
    // "left the drawer" and close it before the incoming item's focus-gained
    // ever lands.
    brls::sync([this]() {
        for (brls::View* f = brls::Application::getCurrentFocus(); f != nullptr; f = f->getParent())
            if (f == this) return;  // focus landed on another item inside the drawer
        this->isOpen = false;
        this->slideTo(-WIDTH);
    });
}
