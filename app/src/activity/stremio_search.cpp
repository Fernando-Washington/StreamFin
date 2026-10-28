/*
    Stremio search screen -- implementation.
*/
#include "activity/stremio_search.hpp"
#include "activity/stremio_series.hpp"
#include "activity/stremio_detail.hpp"
#include "activity/stremio_streams.hpp"
#include "activity/stremio_favourites.hpp"
#include "activity/stremio_resume.hpp"
#include "view/recycling_grid.hpp"
#include "view/h_recycling.hpp"
#include "view/video_card.hpp"
#include "view/svg_image.hpp"
#include "utils/image.hpp"
#include "utils/theme.hpp"
#include "api/stremio.hpp"

#include <algorithm>

using namespace brls::literals;

namespace {

// Percent-encode everything outside the unreserved set, so queries with
// commas, ampersands, etc. ("love, death & robots") reach Cinemeta intact.
std::string urlEncode(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if (isalnum(c) || c == '-' || c == '.' || c == '_' || c == '~') {
            out += (char)c;
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}

void styleSectionHeader(brls::Label* h, const std::string& text) {
    h->setText(text);
    h->setFontSize(24);
    h->setMarginTop(20);
    h->setMarginBottom(10);
    h->setMarginLeft(6);
    h->setTextColor(theme::TEXT_PRIMARY);
}

// Same behaviour as the home grid's source: every result opens the detail
// screen.
class SearchSource : public RecyclingGridDataSource {
public:
    explicit SearchSource(const std::vector<stremio::Meta>& r) : list(r) {}

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
        // IMDb rating badge, straight from the Cinemeta catalog data. Hidden
        // when a poster provider is set (rating is baked into the image).
        if (item.imdbRating.empty() || !stremio::POSTER_TEMPLATE.empty()) {
            cell->labelRating->setVisibility(brls::Visibility::INVISIBLE);
        } else {
            cell->labelRating->setText("★ " + item.imdbRating);
            cell->labelRating->setVisibility(brls::Visibility::VISIBLE);
        }
        cell->badgeTopRight->setVisibility(brls::Visibility::GONE);
        cell->rectProgress->getParent()->setVisibility(brls::Visibility::GONE);

        // Heart badge is the feedback — no toast (rapid toggling stacked them).
        bool fav = Favourites::instance().contains(item.id);
        cell->badgeFavorite->setVisibility(fav ? brls::Visibility::VISIBLE : brls::Visibility::INVISIBLE);
        cell->onToggleFav = [item, cell]() {
            bool nowFav = Favourites::instance().toggle(item);
            cell->badgeFavorite->setVisibility(nowFav ? brls::Visibility::VISIBLE : brls::Visibility::INVISIBLE);
        };

        std::string poster = stremio::posterUrl(item.id, item.poster);
        if (!poster.empty()) Image::with(cell->picture, poster);

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

}  // namespace

StremioSearch::StremioSearch(const std::string& query) {
    brls::Logger::debug("StremioSearch: {}", query);
    this->setAxis(brls::Axis::COLUMN);
    this->setDimensions(brls::Application::contentWidth, brls::Application::contentHeight);
    this->setBackgroundColor(brls::Application::getTheme()["brls/background"]);
    this->setPadding(20, 40, 0, 40);

    // Plain label, styled like the home-row titles. (brls::Header draws a
    // full-width underline that looked like a stray line above the posters.)
    auto* title = new brls::Label();
    title->setText(fmt::format("Search: {}", query));
    title->setFontSize(27);
    title->setMarginBottom(6);
    title->setMarginLeft(6);
    title->setTextColor(theme::TEXT_PRIMARY);
    this->addView(title);

    auto* scroll = new brls::ScrollingFrame();
    scroll->setGrow(1.0f);
    scroll->setScrollingIndicatorVisible(false);

    auto* box = new brls::Box();
    box->setAxis(brls::Axis::COLUMN);
    box->setPadding(0, 0, 32, 0);
    scroll->setContentView(box);
    this->addView(scroll);

    // Two rows, same shape as the home carousels -- kept separate (instead
    // of one interleaved grid) so movies and series read as distinct groups.
    this->moviesHeader = new brls::Label();
    styleSectionHeader(this->moviesHeader, "Movies");
    box->addView(this->moviesHeader);
    this->moviesRec = new HRecyclerFrame();
    this->moviesRec->setHeight(300);
    this->moviesRec->estimatedRowWidth = 175;
    this->moviesRec->registerCell("Cell", FavCardCell::create);
    box->addView(this->moviesRec);
    this->moviesRec->showSkeleton(6);

    this->seriesHeader = new brls::Label();
    styleSectionHeader(this->seriesHeader, "Series");
    box->addView(this->seriesHeader);
    this->seriesRec = new HRecyclerFrame();
    this->seriesRec->setHeight(300);
    this->seriesRec->estimatedRowWidth = 175;
    this->seriesRec->registerCell("Cell", FavCardCell::create);
    box->addView(this->seriesRec);
    this->seriesRec->showSkeleton(6);

    this->emptyLabel = new brls::Label();
    this->emptyLabel->setText("No results");
    this->emptyLabel->setFontSize(20);
    this->emptyLabel->setMarginTop(30);
    this->emptyLabel->setMarginLeft(6);
    this->emptyLabel->setTextColor(theme::TEXT_SECONDARY);
    this->emptyLabel->setVisibility(brls::Visibility::GONE);
    box->addView(this->emptyLabel);

    this->registerAction("hints/back"_i18n, brls::BUTTON_B, [](brls::View*) {
        brls::Application::popActivity();
        return true;
    });

    // Make sure something is focused so B (back) always works, even before
    // any results (or the skeletons) are in.
    brls::sync([this]() { brls::Application::giveFocus(this); });

    std::string q = urlEncode(query);
    this->fetchInto(stremio::CINEMETA + "/catalog/movie/top/search=" + q + ".json", false);
    this->fetchInto(stremio::CINEMETA + "/catalog/series/top/search=" + q + ".json", true);
}

void StremioSearch::fetchInto(const std::string& url, bool isSeries) {
    ASYNC_RETAIN
    stremio::getJSON<stremio::MetaList>(
        [ASYNC_TOKEN, isSeries](stremio::MetaList r) {
            ASYNC_RELEASE
            this->addResults(r.metas, isSeries);
        },
        [ASYNC_TOKEN, isSeries, url](const std::string& e) {
            ASYNC_RELEASE
            brls::Logger::warning("search fetch failed for {}: {}", url, e);
            this->addResults({}, isSeries);  // let the empty-state logic run
        },
        url);
}

void StremioSearch::addResults(const std::vector<stremio::Meta>& metas, bool isSeries) {
    (isSeries ? this->series : this->movies) = metas;
    this->pending--;
    this->rebuild();
}

void StremioSearch::rebuild() {
    bool anyMovies = !this->movies.empty();
    bool anySeries = !this->series.empty();

    this->moviesHeader->setVisibility(anyMovies ? brls::Visibility::VISIBLE : brls::Visibility::GONE);
    this->moviesRec->setVisibility(anyMovies ? brls::Visibility::VISIBLE : brls::Visibility::GONE);
    if (anyMovies) this->moviesRec->setDataSource(new SearchSource(this->movies));

    this->seriesHeader->setVisibility(anySeries ? brls::Visibility::VISIBLE : brls::Visibility::GONE);
    this->seriesRec->setVisibility(anySeries ? brls::Visibility::VISIBLE : brls::Visibility::GONE);
    if (anySeries) this->seriesRec->setDataSource(new SearchSource(this->series));

    bool nothingYet = !anyMovies && !anySeries;
    this->emptyLabel->setVisibility(
        nothingYet && this->pending <= 0 ? brls::Visibility::VISIBLE : brls::Visibility::GONE);

    // Only steer focus once, onto whichever row gets content first -- a
    // second batch arriving later (movies vs series can resolve in either
    // order) shouldn't yank focus away from a row the user may already be
    // browsing.
    if (this->focusGiven) return;
    if (anyMovies) {
        brls::Application::giveFocus(this->moviesRec);
        this->focusGiven = true;
    } else if (anySeries) {
        brls::Application::giveFocus(this->seriesRec);
        this->focusGiven = true;
    } else if (this->pending <= 0) {
        this->focusGiven = true;  // nothing to focus; B still works on `this`
    }
}
