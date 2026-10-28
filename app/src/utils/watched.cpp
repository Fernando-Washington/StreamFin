#include "utils/watched.hpp"
#include "utils/config.hpp"

#include <borealis/core/logger.hpp>
#include <nlohmann/json.hpp>
#include <fstream>
#include <sys/stat.h>

void WatchedTracker::init() {
    if (this->inited) return;
    this->inited = true;
    this->load();
}

bool WatchedTracker::isWatched(const std::string& streamId) {
    this->init();
    return this->watched.count(streamId) > 0;
}

void WatchedTracker::markWatched(const std::string& streamId) {
    this->init();
    if (streamId.empty() || !this->watched.insert(streamId).second) return;  // already recorded
    this->save();
}

void WatchedTracker::markUnwatched(const std::string& streamId) {
    this->init();
    if (this->watched.erase(streamId) == 0) return;
    this->save();
}

void WatchedTracker::load() {
    try {
        std::ifstream in(AppConfig::instance().configDir() + "/watched.json");
        if (in.is_open()) {
            nlohmann::json j;
            in >> j;
            this->watched = j.get<std::set<std::string>>();
        }
    } catch (const std::exception& e) {
        brls::Logger::warning("WatchedTracker load: {}", e.what());
        this->watched.clear();
    }
}

void WatchedTracker::save() {
    std::string dir = AppConfig::instance().configDir();
#ifdef _WIN32
    ::mkdir(dir.c_str());
#else
    ::mkdir(dir.c_str(), 0777);
#endif
    try {
        std::ofstream out(dir + "/watched.json");
        if (out.is_open()) {
            nlohmann::json j = this->watched;
            out << j.dump(2);
        }
    } catch (const std::exception& e) {
        brls::Logger::warning("WatchedTracker save: {}", e.what());
    }
}
