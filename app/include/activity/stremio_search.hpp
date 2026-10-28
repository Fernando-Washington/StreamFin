/*
    Stremio search screen

    Searches Cinemeta for movies and series matching a query and shows the
    results as two labeled carousels (Movies / Series), same layout language
    as the home screen. Selecting a result behaves like the home grid
    (movie -> stream picker, series -> season list).
*/
#pragma once

#include <borealis.hpp>
#include "api/stremio.hpp"

class HRecyclerFrame;

class StremioSearch : public brls::Box {
public:
    explicit StremioSearch(const std::string& query);

    // Stores one batch of results (movies or series) and rebuilds the rows.
    void addResults(const std::vector<stremio::Meta>& metas, bool isSeries);

private:
    void fetchInto(const std::string& url, bool isSeries);
    void rebuild();  // fill/hide the Movies and Series rows from what's arrived

    std::vector<stremio::Meta> movies, series;
    int pending = 2;      // outstanding search requests (movies + series)
    bool focusGiven = false;  // only steer focus once, onto the first row that gets content

    brls::Label* moviesHeader = nullptr;
    HRecyclerFrame* moviesRec = nullptr;
    brls::Label* seriesHeader = nullptr;
    HRecyclerFrame* seriesRec = nullptr;
    brls::Label* emptyLabel = nullptr;  // "No results" -- shown only once both are empty
};
