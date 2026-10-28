#pragma once

#include <borealis.hpp>

// StreamFin's own color palette for the Stremio-specific screens (Home,
// Detail, Search, Settings, Downloads, Addons, Series, the sidebar drawer,
// the search header). This is separate from borealis's own inherited
// Switchfin/Jellyfin light/dark theme system (brls::Application::getTheme(),
// configured in utils/config.cpp) -- that one supports switching themes at
// runtime; this is one fixed dark palette for the screens built for this
// fork, kept in one place the same way a stylesheet keeps `:root { --color-x:
// ...; }` custom properties -- change a value here and every screen that
// uses it updates together, instead of hunting down each `nvgRGB(...)` call
// by hand.
//
// These are the values already in use across the app as of this file's
// creation; a few near-duplicate shades that clearly meant the same color
// (e.g. three or four almost-identical "soft white" text colors) were
// consolidated into one here.
namespace theme {

// Text
inline const NVGcolor TEXT_PRIMARY = nvgRGB(240, 242, 248);    // titles, primary labels
inline const NVGcolor TEXT_SECONDARY = nvgRGB(168, 176, 192);  // subtitles, metadata, dimmed text
inline const NVGcolor TEXT_MUTED = nvgRGB(150, 155, 170);      // section headers, footnotes, hints

// Backgrounds
inline const NVGcolor BG_SCREEN = nvgRGB(16, 18, 24);  // full-screen background ("cinematic slate")
inline const NVGcolor BG_PANEL = nvgRGB(20, 22, 28);   // sidebar drawer / overlay panels
inline const NVGcolor BG_RAISED = nvgRGB(28, 30, 38);  // search header and other bars raised over BG_SCREEN
inline const NVGcolor BG_BUTTON = nvgRGB(40, 43, 52);  // secondary buttons (e.g. "Download" next to "Watch")
inline const NVGcolor BG_TRACK = nvgRGB(55, 59, 68);   // progress-bar tracks
inline const NVGcolor BG_CARD = nvgRGB(28, 31, 38);    // list-row "card" background (Downloads, etc.)
inline const NVGcolor BG_DISABLED = nvgRGB(85, 87, 94);  // buttons while an action is in flight

// Brand / status
inline const NVGcolor ACCENT = nvgRGB(245, 33, 42);            // StreamFin red -- primary actions, focus, brand
inline const NVGcolor STATUS_FAILED = nvgRGB(245, 110, 110);   // download failed
inline const NVGcolor STATUS_COMPLETED = nvgRGB(128, 208, 150);  // download completed
inline const NVGcolor STATUS_ACTIVE = nvgRGB(130, 190, 245);   // download in progress (status text)
inline const NVGcolor PROGRESS_FILL = nvgRGB(90, 170, 240);    // progress-bar fill (kept distinct from
                                                                // STATUS_ACTIVE: same family, different context)

// Segmented storage-usage bar (Settings screen): "how much of the SD card is
// StreamFin's completed downloads vs. all other used bytes (including partial
// downloads) vs. still free" -- same idea as
// a phone's Storage settings page.
inline const NVGcolor STORAGE_APP = ACCENT;                    // completed downloads
inline const NVGcolor STORAGE_OTHER = nvgRGB(70, 74, 84);      // other used bytes, including this app's partial downloads
inline const NVGcolor STORAGE_FREE = nvgRGB(46, 49, 56);       // still free

}  // namespace theme
