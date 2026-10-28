#pragma once

#include <borealis.hpp>
#include <functional>
#include <cstdint>
#include <string>

// Small reusable builders for the hand-rolled (non-XML) Stremio screens --
// Settings, Addons, the autoplay "up next" dialog, and whatever comes next --
// so each one stops re-declaring its own near-identical "focusable row with
// a label" or "section header". Not a framework, just shared building
// blocks: the closest thing this codebase has to a tiny component library.
// Pair with utils/theme.hpp for colors.
namespace ui {

// A section header label ("Stream addons", "Season downloads", ...): small,
// muted, spaced above/below its neighbors.
brls::Label* sectionHeader(const std::string& text);

// A plain, non-interactive row of text (an addon URL, a status line, ...).
brls::Label* textRow(const std::string& text);

// A focusable, clickable row spanning the available width -- a settings
// toggle, a menu entry, anything that's "tap this line to do something".
// The label is returned alongside the row so callers that need to update
// their own text later (a toggle switching from "Automatic" to "Manual")
// can keep a pointer to it without reaching into the row's children.
struct Row {
    brls::Box* box;
    brls::Label* label;
};
Row clickableRow(const std::string& text, std::function<void()> onClick);

// A small pill-shaped button, fixed width, centered label -- for inline
// choices like a confirmation dialog's "Cancel" / "Confirm". Meant to sit
// inside a Box with axis ROW alongside its sibling button(s).
brls::Box* pillButton(const std::string& text, std::function<void()> onClick);

// SD-card usage: proportional StreamFin / other / free segments with a legend.
// All three arguments are non-negative byte counts from the same volume.
brls::Box* storageBar(int64_t usedByApp, int64_t usedByOther, int64_t free);

}  // namespace ui
