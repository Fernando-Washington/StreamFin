#pragma once

#include <borealis.hpp>

class StremioSettings : public brls::Box {
public:
    StremioSettings();
    void willAppear(bool resetState = false) override;
    void onChildFocusGained(brls::View* directChild, brls::View* focusedView) override;

private:
    void refresh();
    brls::Box* storageSlot = nullptr;
    int64_t shownTotal = -2;
    int64_t shownFree = -2;
    int64_t shownOwn = -2;
    brls::Label* seasonMode = nullptr;
    brls::Label* autoplayMode = nullptr;
    brls::Label* audioLangMode = nullptr;
    brls::Label* smartDownloadsMode = nullptr;
};
