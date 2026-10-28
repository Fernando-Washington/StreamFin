#pragma once

#include <borealis/core/singleton.hpp>
#include <string>
#include <unordered_map>

// Per-season "where the opening starts/ends" markers, set once by watching an
// episode and marking the two points (see RemotePlayer's ZR handling in
// remote_view.cpp) -- then reused automatically for every other episode in
// that season: a small on-screen prompt offers to skip straight from start to
// end, instead of a single fixed "skip the first N seconds" that has no idea
// where the opening actually ends.
class IntroMarkers : public brls::Singleton<IntroMarkers> {
public:
    struct Marker {
        double start = 0;
        double end = 0;
    };

    void init();  // load once; safe to call repeatedly

    // false if this season has no markers yet.
    bool get(const std::string& seriesId, int season, Marker& out);
    void set(const std::string& seriesId, int season, double start, double end);

private:
    void load();
    void save();
    static std::string key(const std::string& seriesId, int season);

    bool inited = false;
    std::unordered_map<std::string, Marker> markers;  // key: "seriesId:season"
};
