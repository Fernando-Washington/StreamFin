/*
    Stremio browse screen -- implementation.

    Built programmatically: a ScrollingFrame whose single content view is a
    column Box. Each row = a Header + an HRecyclerFrame (horizontal poster
    carousel) fed by StremioSource.
*/
#include "activity/stremio_home.hpp"
#include "activity/stremio_series.hpp"
#include "activity/stremio_detail.hpp"
#include "activity/stremio_streams.hpp"
#include "activity/stremio_search.hpp"
#include "activity/stremio_favourites.hpp"
#include "activity/stremio_resume.hpp"
#include "activity/stremio_catalogs.hpp"
#include "view/recycling_grid.hpp"
#include "view/h_recycling.hpp"
#include "view/video_card.hpp"
#include "view/svg_image.hpp"
#include "view/sidebar_menu.hpp"
#include "view/search_header.hpp"
#include "utils/image.hpp"
#include "utils/config.hpp"
#include "utils/theme.hpp"
#include "api/stremio.hpp"

#include <ctime>
#include <memory>

using namespace brls::literals;

namespace {

// First-run / on-demand prompt for the stream addon URL (on-screen keyboard).
void promptForAddon(const std::string& initial) {
    brls::Application::getImeManager()->openForText(
        [](const std::string& text) {
            std::string url = stremio::normalizeAddonUrl(text);
            if (url.empty()) {
                brls::Application::notify("No addon set — press − to set it later");
                return;
            }
            stremio::saveAddon(AppConfig::instance().configDir(), url);
            brls::Application::notify("Stream addon saved");
        },
        "Stream addon URL",
        "Type your addon URL — or easier: put it in a text file at switch/streamfin-addon.txt on the SD card and relaunch "
        "(one URL per line; multiple lines = multiple addons, keyboard sets one)",
        1024, initial, 0);
}

// Consistent, polished styling for every row title (Continue Watching,
// Favourites, and all catalog rows) — one place to tweak the look.
void styleRowHeader(brls::Label* h, const std::string& text) {
    h->setText(text);
    h->setFontSize(27);
    h->setMarginTop(28);
    h->setMarginBottom(12);
    h->setMarginLeft(6);
    h->setTextColor(theme::TEXT_PRIMARY);
}

// Data source: turns a list of Cinemeta metas into poster cards. Selecting a
// movie opens the stream picker; selecting a series opens its episode list.
class StremioSource : public RecyclingGridDataSource {
public:
    explicit StremioSource(const std::vector<stremio::Meta>& r) : list(std::move(r)) {}

    size_t getItemCount() override { return this->list.size(); }

    RecyclingGridItem* cellForRow(RecyclingView* recycler, size_t index) override {
        FavCardCell* cell = dynamic_cast<FavCardCell*>(recycler->dequeueReusableCell("Cell"));
        auto item = this->list.at(index);

        cell->labelTitle->setText(item.name);
        if (item.year.empty()) {
            cell->labelExt->setVisibility(brls::Visibility::GONE);
        } else {
            cell->labelExt->setText(item.year);
            cell->labelExt->setVisibility(brls::Visibility::VISIBLE);
        }
        // IMDb rating badge (bottom-right on the poster), straight from the
        // Cinemeta catalog data — no extra request. Hidden when a poster
        // provider (RPDB etc.) is set: those bake the rating into the image.
        if (item.imdbRating.empty() || !stremio::POSTER_TEMPLATE.empty()) {
            cell->labelRating->setVisibility(brls::Visibility::INVISIBLE);
        } else {
            cell->labelRating->setText("★ " + item.imdbRating);
            cell->labelRating->setVisibility(brls::Visibility::VISIBLE);
        }
        cell->badgeTopRight->setVisibility(brls::Visibility::GONE);
        cell->rectProgress->getParent()->setVisibility(brls::Visibility::GONE);

        // Heart badge reflects favourite state; X toggles it. The badge itself
        // is the feedback — no toast (rapid toggling stacked them on screen).
        bool fav = Favourites::instance().contains(item.id);
        cell->badgeFavorite->setVisibility(fav ? brls::Visibility::VISIBLE : brls::Visibility::INVISIBLE);
        cell->onToggleFav = [item, cell]() {
            bool nowFav = Favourites::instance().toggle(item);
            cell->badgeFavorite->setVisibility(nowFav ? brls::Visibility::VISIBLE : brls::Visibility::INVISIBLE);
        };

        std::string poster = stremio::posterUrl(item.id, item.poster);
        if (!poster.empty()) Image::with(cell->picture, poster);

        // Marks whichever card is currently bound to index 0, so the single
        // NAV_LEFT action in StremioHome's ctor knows when to open the
        // sidebar. Set on every bind (not just index 0) so a recycled cell
        // that moves off index 0 doesn't keep a stale marker.
        cell->setId(index == 0 ? HOME_ROW_FIRST_CARD_ID : "home/row-card");

        return cell;
    }

    void onItemSelected(brls::Box* recycler, size_t index) override {
        auto item = this->list.at(index);
        // Both movies and series open the detail screen (info + cast +
        // Watch/Episodes button).
        brls::Application::pushActivity(new brls::Activity(new StremioDetail(item)), brls::TransitionAnimation::NONE);
    }

    void clearData() override { this->list.clear(); }

private:
    std::vector<stremio::Meta> list;
};

// Continue Watching: poster cards with a progress bar; selecting one fetches
// streams and resumes where you left off.
class ContinueSource : public RecyclingGridDataSource {
public:
    explicit ContinueSource(const std::vector<ResumeEntry>& r) : list(r) {}
    ~ContinueSource() override { *this->alive = false; }

    size_t getItemCount() override { return this->list.size(); }

    RecyclingGridItem* cellForRow(RecyclingView* recycler, size_t index) override {
        FavCardCell* cell = dynamic_cast<FavCardCell*>(recycler->dequeueReusableCell("Cell"));
        auto& e = this->list.at(index);

        cell->labelTitle->setText(e.name);
        cell->labelExt->setVisibility(brls::Visibility::GONE);
        cell->labelRating->setVisibility(brls::Visibility::INVISIBLE);
        cell->badgeFavorite->setVisibility(brls::Visibility::INVISIBLE);
        cell->badgeTopRight->setVisibility(brls::Visibility::GONE);
        // X clears this item from Continue Watching. Deferred to the next frame
        // so the button press finishes before the row tears down and rebuilds.
        cell->onToggleFav = [id = e.streamId]() {
            brls::sync([id]() {
                ResumeTracker::instance().remove(id);
                brls::Application::notify("Removed from Continue Watching");
            });
        };

        if (e.duration > 0) {
            cell->rectProgress->setWidthPercentage((float)(e.position / e.duration * 100.0));
            cell->rectProgress->getParent()->setVisibility(brls::Visibility::VISIBLE);
        } else {
            cell->rectProgress->getParent()->setVisibility(brls::Visibility::GONE);
        }

        // posterUrl strips ":season:episode", so with a poster provider set,
        // series resolve to the SERIES poster instead of an episode thumb.
        std::string poster = stremio::posterUrl(e.streamId, e.poster);
        if (!poster.empty()) Image::with(cell->picture, poster);

        // See StremioSource::cellForRow for why this is set unconditionally.
        cell->setId(index == 0 ? HOME_ROW_FIRST_CARD_ID : "home/row-card");

        return cell;
    }

    void onItemSelected(brls::Box* recycler, size_t index) override {
        auto e = this->list.at(index);
        if (stremio::STREAM_ADDONS.empty()) {
            brls::Application::notify("Set your stream addon first (press −)");
            return;
        }
        brls::Application::notify("Finding streams…");
        auto alive = this->alive;
        auto stack = brls::Application::getActivitiesStack();
        auto* origin = stack.empty() ? nullptr : stack.back();
        auto picker = std::make_shared<StreamPickerHandle>();
        stremio::getStreams(e.streamType, e.streamId,
            [e, alive, origin, picker](std::vector<stremio::Stream> streams, size_t pending) {
                if (!*alive) return;
                if (!picker->opened()) {
                    auto stack = brls::Application::getActivitiesStack();
                    if (stack.empty() || stack.back() != origin) return;
                    auto* view = new StreamPicker(e.name, streams, e, false, {}, {}, pending);
                    picker->attach(view);
                    brls::Application::pushActivity(new brls::Activity(view));
                } else {
                    picker->append(std::move(streams), pending);
                }
            },
            [picker]() { picker->finish(); },
            [alive](const std::string& err) {
                if (*alive) brls::Application::notify("Stream error: " + err);
            });
    }

    void clearData() override { this->list.clear(); }

private:
    std::vector<ResumeEntry> list;
    std::shared_ptr<bool> alive = std::make_shared<bool>(true);
};

}  // namespace

StremioHome::~StremioHome() {
    stremio::CATALOGS_CHANGED.unsubscribe(this->catalogsSub);
    brls::Application::getRunLoopEvent()->unsubscribe(this->scrollSub);
}

StremioHome::StremioHome() {
    brls::Logger::debug("StremioHome: create");
    this->setAxis(brls::Axis::COLUMN);
    this->setDimensions(brls::Application::contentWidth, brls::Application::contentHeight);
    this->setBackgroundColor(theme::BG_SCREEN);

    auto* scroll = new brls::ScrollingFrame();
    scroll->setGrow(1.0f);
    scroll->setScrollingIndicatorVisible(false);
    // NOTE: anchored (TV-style) scrolling was tried here in v0.4.0-rc1 and
    // reverted — on device it misplaced the focus highlight on the first row
    // and made left-input jump rows. The opt-in mode still exists in
    // borealis (setScrollingAnchored) for a future attempt with local
    // debugging; home stays on stock centered scrolling until then.

    this->boxHome = new brls::Box();
    this->boxHome->setAxis(brls::Axis::COLUMN);
    this->boxHome->setPadding(16, 50, 32, 50);  // more breathing room at the edges
    scroll->setContentView(this->boxHome);

    // Search bar pinned above the scroll (not inside it, so it can collapse
    // independently of the carousels). Selecting it opens the keyboard;
    // submitting a query pushes the existing search results screen. Wrapped
    // in a full-width row so the (deliberately narrow) pill centers itself
    // instead of stretching edge to edge.
    this->searchHeader = new SearchHeader();
    this->searchHeader->onSearch = [](const std::string& query) {
        brls::Application::pushActivity(new brls::Activity(new StremioSearch(query)));
    };
    auto* searchWrap = new brls::Box();
    searchWrap->setAxis(brls::Axis::ROW);
    searchWrap->setJustifyContent(brls::JustifyContent::CENTER);
    searchWrap->setPadding(20, 50, 0, 50);  // matches boxHome's left/right so it lines up with the carousels
    searchWrap->setMarginBottom(20);
    searchWrap->addView(this->searchHeader);
    this->addView(searchWrap);

    this->addView(scroll);
    // Drawer overlay, parked off-screen left; slides in when focus reaches it
    // (see SidebarMenu / the LEFT route set on every row's first card).
    this->addView(new SidebarMenu());

    // Collapse the search header the moment the page scrolls down at all.
    // ScrollingFrame has no scroll event, so this samples its (already
    // smoothed) offset once a frame instead.
    this->scrollSub = brls::Application::getRunLoopEvent()->subscribe([this, scroll]() {
        float y = scroll->getContentOffsetY();
        float delta = y - this->lastScrollOffsetY;
        this->lastScrollOffsetY = y;
        if (delta > 0.5f) this->searchHeader->setCollapsed(true);
    });

    // Reopening it is edge-triggered, same rule as the sidebar menu: a
    // single fresh Up press (not held/repeating) while already on the home
    // page's first row -- checked via scroll offset, since "first row" can
    // be Continue Watching, Favourites, or the first catalog row depending
    // on what's visible, but all three sit at offset 0 when focused. Holding
    // Up through several rows toward the top must NOT pop this open; only a
    // deliberate extra press once there should.
    this->registerAction(
        "", brls::BUTTON_NAV_UP,
        [this, scroll](brls::View*) {
            if (scroll->getContentOffsetY() > 0.5f) return false;
            this->searchHeader->setCollapsed(false);
            brls::Application::giveFocus(this->searchHeader);
            return true;
        },
        /*hidden*/ true);

    // Continue Watching (very top) + Favourites rows — both hidden until non-empty.
    Favourites::instance().load();
    ResumeTracker::instance().init();
    this->addContinueRow();
    this->addFavouritesRow();

    // Catalog carousels come from the persisted row registry: the built-in
    // Cinemeta rows plus every catalog offered by the user's catalog addons
    // (catalog=URL lines in streamfin-addon.txt), in the user's order.
    this->buildCatalogRows();

    // Manifest refreshes and Catalogs-screen edits rebuild the carousels.
    this->catalogsSub = stremio::CATALOGS_CHANGED.subscribe([this]() { this->rebuildCatalogRows(); });

    // Y opens the on-screen keyboard to search Cinemeta.
    this->registerAction("Search", brls::BUTTON_Y, [](brls::View*) {
        brls::Application::getImeManager()->openForText(
            [](const std::string& text) {
                if (!text.empty())
                    brls::Application::pushActivity(new brls::Activity(new StremioSearch(text)));
            },
            "Search movies & series", "", 64, "", 0);
        return true;
    });

    // − sets/changes the stream addon URL; also prompts automatically on
    // first launch (deferred a frame so the window is up).
    this->registerAction("Stream Addon", brls::BUTTON_BACK, [](brls::View*) {
        promptForAddon(stremio::STREAM_ADDONS.empty() ? "" : stremio::STREAM_ADDONS.front());
        return true;
    });

    // + opens the Catalogs screen: toggle home rows on/off and reorder them.
    this->registerAction("Catalogs", brls::BUTTON_START, [](brls::View*) {
        brls::Application::pushActivity(new brls::Activity(new StremioCatalogs()));
        return true;
    });

    // Open the sidebar menu on a genuine, single Left press while resting on
    // a row's first card. Registered once here (not per-cell) and with
    // allowRepeating left at its default (false), so this is skipped
    // entirely while the direction is held/auto-repeating -- holding Left
    // through the middle of a row must NOT fling you into the drawer, only
    // a fresh press from card 0 should. Returning false for every other
    // case lets the normal within-row navigation run unmodified.
    this->registerAction(
        "", brls::BUTTON_NAV_LEFT,
        [](brls::View*) {
            brls::View* focus = brls::Application::getCurrentFocus();
            if (!focus || focus->getId() != HOME_ROW_FIRST_CARD_ID) return false;
            brls::View* target = focus->getNearestView(SIDEBAR_MENU_FIRST_ITEM_ID);
            if (!target) return false;
            brls::Application::giveFocus(target);
            return true;
        },
        /*hidden*/ true);

    if (stremio::STREAM_ADDONS.empty())
        brls::sync([]() { promptForAddon(""); });
}

void StremioHome::addRow(const std::string& title, const std::string& url) {
    auto* header = new brls::Label();
    styleRowHeader(header, title);
    this->boxHome->addView(header);

    auto* rec = new HRecyclerFrame();
    rec->setHeight(300);
    rec->estimatedRowWidth = 175;
    rec->registerCell("Cell", FavCardCell::create);
    this->boxHome->addView(rec);
    this->catalogViews.push_back(header);
    this->catalogViews.push_back(rec);
    if (!this->firstRowRec) {
        this->firstRowRec = rec;  // safe parking spot for focus
        rec->setId(SIDEBAR_MENU_RETURN_ID);  // also where the sidebar menu sends focus back
    }
    rec->showSkeleton(8);

    ASYNC_RETAIN
    stremio::getJSON<stremio::MetaList>(
        [ASYNC_TOKEN, rec](stremio::MetaList r) {
            ASYNC_RELEASE
            // The registry may have rebuilt the carousels while this fetch was
            // in flight (manifest refresh on a cold start) — the recycler this
            // callback captured could be destroyed. Only touch it if it's
            // still one of ours.
            auto& live = this->catalogViews;
            if (std::find(live.begin(), live.end(), (brls::View*)rec) == live.end()) return;
            if (r.metas.empty()) return;
            bool focusHere = false;
            for (brls::View* f = brls::Application::getCurrentFocus(); f != nullptr; f = f->getParent())
                if (f == rec) {
                    focusHere = true;
                    break;
                }
            rec->setDataSource(new StremioSource(r.metas));
            // Focus sat on this row's skeleton (parked there during a rebuild):
            // re-land it on the real first cell now that the data is in.
            if (focusHere) brls::sync([this, rec]() {
                auto& v = this->catalogViews;
                if (std::find(v.begin(), v.end(), (brls::View*)rec) != v.end())
                    brls::Application::giveFocus(rec);
            });
        },
        [ASYNC_TOKEN, rec, url](const std::string& e) {
            ASYNC_RELEASE
            brls::Logger::warning("catalog row fetch failed for {}: {}", url, e);
        },
        url);
}

void StremioHome::buildCatalogRows() {
    for (auto& def : stremio::CATALOG_ROWS)
        if (def.enabled) this->addRow(def.title, def.url);
}

void StremioHome::rebuildCatalogRows() {
    // While another screen is on top (Catalogs, detail, player), borealis'
    // focus stack holds a pointer to the home view that had focus — and
    // popActivity gives focus back to that exact pointer. Destroying rows now
    // would leave it dangling (ghost highlight, then a crash on back).
    // Defer; onChildFocusGained fires the rebuild once focus returns to home.
    auto stack = brls::Application::getActivitiesStack();
    if (!stack.empty() && this->getParentActivity() != stack.back()) {
        this->pendingRebuild = true;
        return;
    }
    this->pendingRebuild = false;

    // Same focus-parking discipline as refreshFavourites/refreshContinue:
    // never leave focus on a view that's about to be destroyed.
    bool focusHere = false;
    for (brls::View* f = brls::Application::getCurrentFocus(); f != nullptr; f = f->getParent())
        if (std::find(this->catalogViews.begin(), this->catalogViews.end(), f) != this->catalogViews.end()) {
            focusHere = true;
            break;
        }

    // Build the new rows first (appended after the old ones), so there is
    // always a live row to park focus on before the old cells are destroyed.
    std::vector<brls::View*> old = std::move(this->catalogViews);
    this->catalogViews.clear();
    this->firstRowRec = nullptr;
    this->buildCatalogRows();

    if (focusHere) {
        if (this->firstRowRec)
            brls::Application::giveFocus(this->firstRowRec);
        else
            brls::Application::giveFocus(this);  // all rows toggled off
    }

    for (auto* v : old) this->boxHome->removeView(v);
    // Force a clean relayout — stale yoga incremental layout after toggling
    // row visibility is this screen's oldest bug (see refreshFavourites).
    this->boxHome->invalidate();
}

void StremioHome::onChildFocusGained(brls::View* directChild, brls::View* focusedView) {
    brls::Box::onChildFocusGained(directChild, focusedView);
    // Focus coming back into home (e.g. the Catalogs screen was just popped)
    // with deferred changes: apply them now. Next frame, so the focus
    // restore fully settles first.
    if (this->pendingRebuild) brls::sync([this]() { this->rebuildCatalogRows(); });
    if (this->pendingFavRefresh) brls::sync([this]() { this->refreshFavourites(); });
    if (this->pendingContinueRefresh) brls::sync([this]() { this->refreshContinue(); });
}

void StremioHome::addFavouritesRow() {
    this->favHeader = new brls::Label();
    styleRowHeader(this->favHeader, "Favourites");
    this->boxHome->addView(this->favHeader);

    this->favRec = new HRecyclerFrame();
    this->favRec->setHeight(300);
    this->favRec->estimatedRowWidth = 175;
    this->favRec->registerCell("Cell", FavCardCell::create);
    this->boxHome->addView(this->favRec);

    // Deferred one frame so the X press finishes before the row (and possibly
    // the pressed cell itself) is torn down and rebuilt.
    Favourites::instance().changed()->subscribe([this]() {
        brls::sync([this]() { this->refreshFavourites(); });
    });
    this->refreshFavourites();
}

void StremioHome::refreshFavourites() {
    // Never toggle row visibility while another activity is on top: the
    // relayout resolves against stale state and everything except this row's
    // header comes back shifted a full padding-width right (the "double
    // indent" bug's latest variant — favouriting from the detail screen).
    // Defer, like rebuildCatalogRows; onChildFocusGained runs it on return.
    auto stack = brls::Application::getActivitiesStack();
    if (!stack.empty() && this->getParentActivity() != stack.back()) {
        this->pendingFavRefresh = true;
        return;
    }
    this->pendingFavRefresh = false;

    // Same focus-parking dance as refreshContinue(): if focus is inside this
    // row, park it on a stable row BEFORE the cells are destroyed, or the
    // focus highlight is left stranded at stale coordinates.
    bool focusHere = false;
    for (brls::View* f = brls::Application::getCurrentFocus(); f != nullptr; f = f->getParent())
        if (f == this->favRec) {
            focusHere = true;
            break;
        }
    if (focusHere && this->firstRowRec) brls::Application::giveFocus(this->firstRowRec);

    auto& favs = Favourites::instance().all();
    if (favs.empty()) {
        this->favHeader->setVisibility(brls::Visibility::GONE);
        this->favRec->setVisibility(brls::Visibility::GONE);
        // Row is gone; focus stays parked.
    } else {
        this->favHeader->setVisibility(brls::Visibility::VISIBLE);
        this->favRec->setVisibility(brls::Visibility::VISIBLE);
        this->favRec->setDataSource(new StremioSource(favs));
        if (focusHere) brls::sync([this]() { brls::Application::giveFocus(this->favRec); });
    }
    // Force a clean relayout (see refreshContinue for why).
    this->boxHome->invalidate();
}

void StremioHome::addContinueRow() {
    this->continueHeader = new brls::Label();
    styleRowHeader(this->continueHeader, "Continue Watching");
    this->boxHome->addView(this->continueHeader);

    this->continueRec = new HRecyclerFrame();
    this->continueRec->setHeight(300);
    this->continueRec->estimatedRowWidth = 175;
    this->continueRec->registerCell("Cell", FavCardCell::create);
    this->boxHome->addView(this->continueRec);

    ResumeTracker::instance().changed()->subscribe([this]() { this->refreshContinue(); });
    this->refreshContinue();
}

void StremioHome::refreshContinue() {
    // Deferred while covered, same as refreshFavourites — this refresh often
    // fires right when playback ends, with the player still on top.
    auto stack = brls::Application::getActivitiesStack();
    if (!stack.empty() && this->getParentActivity() != stack.back()) {
        this->pendingContinueRefresh = true;
        return;
    }
    this->pendingContinueRefresh = false;

    // Was focus inside this row? (true when the user just cleared an item;
    // false when the row is refreshing because playback stopped elsewhere.)
    bool focusHere = false;
    for (brls::View* f = brls::Application::getCurrentFocus(); f != nullptr; f = f->getParent())
        if (f == this->continueRec) {
            focusHere = true;
            break;
        }

    // Park focus on a stable row BEFORE we tear down the old cells, so the focus
    // highlight is never left pointing at a cell that's about to be destroyed.
    if (focusHere && this->firstRowRec) brls::Application::giveFocus(this->firstRowRec);

    auto cw = ResumeTracker::instance().continueWatching();
    if (cw.empty()) {
        this->continueHeader->setVisibility(brls::Visibility::GONE);
        this->continueRec->setVisibility(brls::Visibility::GONE);
        // Row is gone now; leave focus on the parked row.
    } else {
        this->continueHeader->setVisibility(brls::Visibility::VISIBLE);
        this->continueRec->setVisibility(brls::Visibility::VISIBLE);
        this->continueRec->setDataSource(new ContinueSource(cw));
        if (focusHere) brls::sync([this]() { brls::Application::giveFocus(this->continueRec); });
        this->enrichContinue();  // resolve any non-English / stale titles, then rebuild once
    }
    // Force a clean relayout of the whole column: a row toggling between
    // hidden and visible mid-frame left everything below it with a doubled
    // left padding (the "band on the left side" bug).
    this->boxHome->invalidate();
}

// Resolve English titles (and series "Name · S1E5") from Cinemeta for any
// Continue-Watching entry not yet enriched, persist them, and rebuild the row
// once when done. Only ever touches the (retained) activity and its row — never
// a recycled cell pointer — so it's safe against scrolling/teardown.
void StremioHome::enrichContinue() {
    if (this->continueEnriching) return;  // a pass is already running

    // Movies only: this fixes localized/original-language movie titles (e.g. a
    // Polish title) by pulling the English name from Cinemeta. Series already
    // get a clean English "Series · S1E5 · Episode" label built on the series
    // screen, and we don't want to shorten that here.
    auto cw = ResumeTracker::instance().continueWatching();
    std::vector<ResumeEntry> todo;
    for (auto& e : cw)
        if (!e.enriched && e.streamType == "movie") todo.push_back(e);
    if (todo.empty()) return;

    this->continueEnriching = true;
    auto pending = std::make_shared<int>((int)todo.size());
    auto changed = std::make_shared<bool>(false);

    auto finalize = [this, changed]() {
        this->continueEnriching = false;
        if (!*changed) return;
        auto fresh = ResumeTracker::instance().continueWatching();
        if (fresh.empty()) return;
        bool focusHere = false;
        for (brls::View* f = brls::Application::getCurrentFocus(); f != nullptr; f = f->getParent())
            if (f == this->continueRec) {
                focusHere = true;
                break;
            }
        if (focusHere && this->firstRowRec) brls::Application::giveFocus(this->firstRowRec);
        this->continueRec->setDataSource(new ContinueSource(fresh));
        if (focusHere) brls::sync([this]() { brls::Application::giveFocus(this->continueRec); });
    };

    for (auto& e : todo) {
        const std::string id = e.streamId;  // movie id == ttID (no ":season:episode")
        const std::string oldName = e.name;

        std::string url = stremio::CINEMETA + "/meta/movie/" + id + ".json";
        ASYNC_RETAIN
        stremio::getJSON<stremio::MetaResult>(
            [ASYNC_TOKEN, id, oldName, pending, changed, finalize](stremio::MetaResult r) {
                ASYNC_RELEASE
                if (!r.meta.name.empty()) {
                    ResumeTracker::instance().updateMeta(id, r.meta.name, "");
                    if (r.meta.name != oldName) *changed = true;
                }
                if (--(*pending) == 0) finalize();
            },
            [ASYNC_TOKEN, pending, finalize](const std::string&) {
                ASYNC_RELEASE
                if (--(*pending) == 0) finalize();
            },
            url);
    }
}
