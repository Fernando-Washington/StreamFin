#include "utils/intro_markers.hpp"
#include "utils/config.hpp"

#include <borealis/core/logger.hpp>
#include <nlohmann/json.hpp>
#include <fstream>
#include <sys/stat.h>

std::string IntroMarkers::key(const std::string& seriesId, int season) {
    return seriesId + ":" + std::to_string(season);
}

void IntroMarkers::init() {
    if (this->inited) return;
    this->inited = true;
    this->load();
}

bool IntroMarkers::get(const std::string& seriesId, int season, Marker& out) {
    this->init();
    auto it = this->markers.find(key(seriesId, season));
    if (it == this->markers.end()) return false;
    out = it->second;
    return true;
}

void IntroMarkers::set(const std::string& seriesId, int season, double start, double end) {
    this->init();
    if (end <= start) return;  // a marker with no length isn't useful, and shouldn't overwrite a good one
    this->markers[key(seriesId, season)] = Marker{start, end};
    this->save();
}

void IntroMarkers::load() {
    try {
        std::ifstream in(AppConfig::instance().configDir() + "/intro_markers.json");
        if (!in.is_open()) return;
        nlohmann::json j;
        in >> j;
        for (auto& [k, v] : j.items()) {
            if (!v.is_object()) continue;
            this->markers[k] = Marker{v.value("start", 0.0), v.value("end", 0.0)};
        }
    } catch (const std::exception& e) {
        brls::Logger::warning("IntroMarkers load: {}", e.what());
        this->markers.clear();
    }
}

void IntroMarkers::save() {
    std::string dir = AppConfig::instance().configDir();
#ifdef _WIN32
    ::mkdir(dir.c_str());
#else
    ::mkdir(dir.c_str(), 0777);
#endif
    try {
        nlohmann::json j;
        for (auto& [k, m] : this->markers) j[k] = {{"start", m.start}, {"end", m.end}};
        std::ofstream out(dir + "/intro_markers.json");
        if (out.is_open()) out << j.dump(2);
    } catch (const std::exception& e) {
        brls::Logger::warning("IntroMarkers save: {}", e.what());
    }
}
