/*
    Stremio addons -- read-only view of what's currently configured.

    Exists because there's no other way to see which addons the app actually
    picked up (from streamfin-addon.txt / the SD-card drop / manual entry)
    without digging through stremio_addon.json by hand on the SD card.
*/
#pragma once

#include <borealis.hpp>

class StremioAddons : public brls::Box {
public:
    StremioAddons();
};
