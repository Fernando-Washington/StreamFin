/*
    Home sidebar menu -- UI only (no item functionality yet).

    A drawer that lives off-screen to the left of the home carousels. Reaching
    the left edge of any row (pressing Left on its first card) slides it in;
    pressing Right from inside it slides it back out. Purely focus-driven: no
    button/gesture wiring, so it works the same on Switch and desktop.
*/
#pragma once

#include <borealis.hpp>

// Stable id for the drawer's first item -- StremioHome's single NAV_LEFT
// action jumps here (by id, resolved at press time) when it decides to open.
extern const char* const SIDEBAR_MENU_FIRST_ITEM_ID;

// Marker id StremioHome puts on whichever card is currently at index 0 of its
// row. Checked (not searched) against Application::getCurrentFocus()->getId()
// -- reused across every row's first card on purpose, never looked up by id,
// so the duplication is harmless.
extern const char* const HOME_ROW_FIRST_CARD_ID;

// Stable id the drawer's items route RIGHT back to when closing. StremioHome
// tags its first row with this id -- the drawer always returns focus there,
// regardless of which row it was opened from (good enough for a UI-only drawer).
extern const char* const SIDEBAR_MENU_RETURN_ID;

class SidebarMenu : public brls::Box {
public:
    SidebarMenu();

    void onChildFocusGained(brls::View* directChild, brls::View* focusedView) override;
    void onChildFocusLost(brls::View* directChild, brls::View* focusedView) override;

    static constexpr float WIDTH = 300.0f;

private:
    void slideTo(float target);

    brls::Animatable slideAnim;
    bool isOpen = false;
};
