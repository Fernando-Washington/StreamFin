/*
    Persistent search bar pinned above the home carousels.

    Selecting it (A) opens the on-screen keyboard; onSearch fires with
    whatever was typed. Collapses (height -> 0) on scroll down, expands again
    on scroll up -- driven externally via setCollapsed(), see StremioHome
    which tracks the home ScrollingFrame's offset to call it.
*/
#pragma once

#include <borealis.hpp>
#include <functional>

class SearchHeader : public brls::Box {
public:
    SearchHeader();

    void setCollapsed(bool collapsed);

    // Updates the label to show the last query (called after a search runs).
    void setQuery(const std::string& text);

    // Fired with the typed text once the user submits a non-empty query.
    std::function<void(const std::string&)> onSearch;

    static constexpr float HEIGHT = 48.0f;
    static constexpr float WIDTH = 420.0f;

private:
    void activate();

    brls::Label* label = nullptr;
    brls::Animatable heightAnim;
    bool collapsed = false;
    std::string query;
};
