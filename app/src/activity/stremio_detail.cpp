/*
    Stremio movie detail screen -- implementation.

    Built programmatically (like the home screen) to avoid the XML-inflation
    ghosting issue. Layout: poster on the left; title, meta line, Watch button,
    description, cast and director in a column on the right.
*/
#include "activity/stremio_detail.hpp"
#include "activity/stremio_series.hpp"
#include "activity/stremio_streams.hpp"
#include "activity/stremio_favourites.hpp"
#include "activity/stremio_resume.hpp"
#include "utils/image.hpp"
#include "utils/theme.hpp"
#include "api/stremio.hpp"

#include <set>

using namespace brls::literals;

namespace {

// Palette shared with the rest of the app -- see utils/theme.hpp. Kept as
// local aliases since this file uses these names dozens of times below.
const NVGcolor& COL_BG = theme::BG_SCREEN;
const NVGcolor& COL_TEXT = theme::TEXT_PRIMARY;
const NVGcolor& COL_DIM = theme::TEXT_SECONDARY;
const NVGcolor& COL_ACCENT = theme::ACCENT;

// Fixed width for the Watch/Download pills -- wide enough to fit "Download"
// (the longest label) comfortably with its padding.
constexpr float BUTTON_WIDTH = 260.0f;

// Join a string list: {"A","B"} -> "A, B".
std::string join(const std::vector<std::string>& v, const char* sep = ", ") {
    std::string out;
    for (auto& s : v) {
        if (s.empty()) continue;
        if (!out.empty()) out += sep;
        out += s;
    }
    return out;
}

}  // namespace

StremioDetail::StremioDetail(const stremio::Meta& item) : item(item) {
    brls::Logger::debug("StremioDetail: {}", item.id);
    this->setAxis(brls::Axis::COLUMN);
    this->setDimensions(brls::Application::contentWidth, brls::Application::contentHeight);
    this->setBackgroundColor(COL_BG);
    this->setPadding(40, 60, 40, 60);

    auto* row = new brls::Box();
    row->setAxis(brls::Axis::ROW);
    row->setGrow(1.0f);
    this->addView(row);

    // ---- Poster (left) ----------------------------------------------------
    this->poster = new brls::Image();
    this->poster->setDimensions(340, 510);
    this->poster->setCornerRadius(10);
    this->poster->setScalingType(brls::ImageScalingType::FILL);
    // The texture is shared with the home-grid cell via the URL-keyed
    // TextureCache (refcounted). The view must never delete it itself, or the
    // grid poster goes white when this screen is popped. Image::with only sets
    // this on the download path, not on a cache hit, so set it explicitly.
    this->poster->setFreeTexture(false);
    row->addView(this->poster);
    std::string posterSrc = stremio::posterUrl(item.id, item.poster);
    if (!posterSrc.empty()) Image::with(this->poster, posterSrc);

    // ---- Info column (right) ----------------------------------------------
    auto* info = new brls::Box();
    info->setAxis(brls::Axis::COLUMN);
    info->setGrow(1.0f);
    info->setMarginLeft(44);
    row->addView(info);

    this->labelTitle = new brls::Label();
    this->labelTitle->setText(item.name);
    this->labelTitle->setFontSize(40);
    this->labelTitle->setTextColor(COL_TEXT);
    this->labelTitle->setIsWrapping(true);
    info->addView(this->labelTitle);

    this->labelMeta = new brls::Label();
    this->labelMeta->setText(item.year);
    this->labelMeta->setFontSize(22);
    this->labelMeta->setTextColor(COL_ACCENT);
    this->labelMeta->setMarginTop(10);
    info->addView(this->labelMeta);

    this->labelGenres = new brls::Label();
    this->labelGenres->setFontSize(20);
    this->labelGenres->setTextColor(COL_DIM);
    this->labelGenres->setMarginTop(6);
    this->labelGenres->setVisibility(brls::Visibility::GONE);
    info->addView(this->labelGenres);

    // Watch button: a focusable pill, same construction style as StreamCell.
    auto* btnRow = new brls::Box();
    btnRow->setAxis(brls::Axis::ROW);
    btnRow->setMarginTop(22);
    info->addView(btnRow);

    this->btnWatch = new brls::Box();
    this->btnWatch->setFocusable(true);
    this->btnWatch->setAxis(brls::Axis::ROW);
    this->btnWatch->setJustifyContent(brls::JustifyContent::CENTER);
    this->btnWatch->setPadding(14, 40, 14, 40);
    // Fixed width so Watch and Download always match exactly -- padding
    // alone doesn't do it since "Download" is longer than "Watch"/"Episodes"
    // and each box was sizing to fit its own label.
    this->btnWatch->setWidth(BUTTON_WIDTH);
    this->btnWatch->setCornerRadius(8);
    this->btnWatch->setBackgroundColor(COL_ACCENT);  // brand red
    auto* btnLabel = new brls::Label();
    this->labelWatchButton = btnLabel;
    // ▶ is in the Switch system font; ☰ is not (renders as a crossed box).
    btnLabel->setText(item.type == "series" ? "▶  Episodes" : "▶  Watch");
    btnLabel->setFontSize(24);
    btnLabel->setTextColor(COL_TEXT);
    this->btnWatch->addView(btnLabel);
    this->btnWatch->registerClickAction([this](brls::View*) {
        this->onAction();
        return true;
    });
    this->btnWatch->addGestureRecognizer(new brls::TapGestureRecognizer(this->btnWatch));
    btnRow->addView(this->btnWatch);

    // Explicit, visible "Download" action right below Watch -- movies only
    // for now (a series downloads per-episode, from inside Episodes). This
    // used to be a hidden X shortcut inside the stream picker, which nobody
    // could reasonably discover on their own.
    if (item.type != "series") {
        // Wrapped in its own row, same as Watch above -- a Box added directly
        // to `info` (COLUMN, default stretch) fills the whole column width
        // instead of sizing to its own content, which is what made this
        // button balloon out edge-to-edge.
        auto* dlRow = new brls::Box();
        dlRow->setAxis(brls::Axis::ROW);
        dlRow->setMarginTop(12);
        info->addView(dlRow);

        this->btnDownload = new brls::Box();
        this->btnDownload->setFocusable(true);
        this->btnDownload->setAxis(brls::Axis::ROW);
        this->btnDownload->setJustifyContent(brls::JustifyContent::CENTER);
        this->btnDownload->setPadding(14, 40, 14, 40);
        this->btnDownload->setWidth(BUTTON_WIDTH);
        this->btnDownload->setCornerRadius(8);
        this->btnDownload->setBackgroundColor(theme::BG_BUTTON);
        auto* dlLabel = new brls::Label();
        this->labelDownloadButton = dlLabel;
        dlLabel->setText("⬇  Download");
        dlLabel->setFontSize(24);
        dlLabel->setTextColor(COL_TEXT);
        this->btnDownload->addView(dlLabel);
        this->btnDownload->registerClickAction([this](brls::View*) {
            this->openStreams(true);
            return true;
        });
        this->btnDownload->addGestureRecognizer(new brls::TapGestureRecognizer(this->btnDownload));
        dlRow->addView(this->btnDownload);
    }

    this->labelDesc = new brls::Label();
    this->labelDesc->setText("Loading…");
    this->labelDesc->setFontSize(21);
    this->labelDesc->setTextColor(COL_DIM);
    this->labelDesc->setIsWrapping(true);
    this->labelDesc->setMarginTop(24);
    info->addView(this->labelDesc);

    this->labelCastHead = new brls::Label();
    this->labelCastHead->setText("Cast");
    this->labelCastHead->setFontSize(24);
    this->labelCastHead->setTextColor(COL_TEXT);
    this->labelCastHead->setMarginTop(26);
    this->labelCastHead->setVisibility(brls::Visibility::GONE);
    info->addView(this->labelCastHead);

    this->labelCast = new brls::Label();
    this->labelCast->setFontSize(21);
    this->labelCast->setTextColor(COL_DIM);
    this->labelCast->setIsWrapping(true);
    this->labelCast->setMarginTop(8);
    this->labelCast->setVisibility(brls::Visibility::GONE);
    info->addView(this->labelCast);

    this->labelDirector = new brls::Label();
    this->labelDirector->setFontSize(21);
    this->labelDirector->setTextColor(COL_DIM);
    this->labelDirector->setMarginTop(14);
    this->labelDirector->setVisibility(brls::Visibility::GONE);
    info->addView(this->labelDirector);

    // ---- Actions -----------------------------------------------------------
    this->registerAction("hints/back"_i18n, brls::BUTTON_B, [](brls::View*) {
        brls::Application::popActivity();
        return true;
    });

    // X toggles favourite here too, matching the poster grids.
    this->registerAction("Favourite", brls::BUTTON_X, [this](brls::View*) {
        bool nowFav = Favourites::instance().toggle(this->item);
        brls::Application::notify(nowFav ? "Added to Favourites" : "Removed from Favourites");
        return true;
    });

    // Something must hold focus so A/B work immediately.
    brls::sync([this]() { brls::Application::giveFocus(this->btnWatch); });

    // ---- Fetch full metadata from Cinemeta ---------------------------------
    ASYNC_RETAIN
    stremio::getJSON<stremio::MetaResult>(
        [ASYNC_TOKEN](stremio::MetaResult r) {
            ASYNC_RELEASE
            this->applyMeta(r.meta);
        },
        [ASYNC_TOKEN, id = item.id](const std::string& err) {
            ASYNC_RELEASE
            brls::Logger::warning("meta fetch failed for {}: {}", id, err);
            this->labelDesc->setText("No details available.");
        },
        stremio::CINEMETA + "/meta/" + item.type + "/" + item.id + ".json");
}

// Release our reference on the shared poster texture (and abort any in-flight
// request) — same contract as BaseCardCell.
StremioDetail::~StremioDetail() { Image::cancel(this->poster); }

void StremioDetail::applyMeta(const stremio::MetaDetail& meta) {
    // Prefer the (English) meta name over the catalog one — for the title AND
    // for the resume/OSD label used when Watch is pressed.
    if (!meta.name.empty()) {
        this->labelTitle->setText(meta.name);
        this->item.name = meta.name;
    }

    // Keep the episodes so the Episodes button can open the season list
    // without refetching the meta, and the backdrop as episode-thumb fallback.
    this->videos = meta.videos;
    this->background = meta.background;

    // Year · runtime · ★ rating (· N seasons for series)
    std::vector<std::string> parts;
    std::string year = !meta.releaseInfo.empty() ? meta.releaseInfo : this->item.year;
    if (!year.empty()) parts.push_back(year);
    if (!meta.runtime.empty()) parts.push_back(meta.runtime);
    if (!meta.imdbRating.empty()) parts.push_back("★ " + meta.imdbRating);
    if (this->item.type == "series" && !meta.videos.empty()) {
        std::set<int> seasons;
        for (auto& v : meta.videos)
            if (v.season > 0) seasons.insert(v.season);
        if (!seasons.empty())
            parts.push_back(seasons.size() == 1 ? "1 season" : fmt::format("{} seasons", seasons.size()));
    }
    if (!parts.empty()) this->labelMeta->setText(join(parts, "   ·   "));

    if (!meta.genres.empty()) {
        this->labelGenres->setText(join(meta.genres, "  ·  "));
        this->labelGenres->setVisibility(brls::Visibility::VISIBLE);
    }

    this->labelDesc->setText(!meta.description.empty() ? meta.description : "No description available.");

    if (!meta.cast.empty()) {
        this->labelCast->setText(join(meta.cast));
        this->labelCastHead->setVisibility(brls::Visibility::VISIBLE);
        this->labelCast->setVisibility(brls::Visibility::VISIBLE);
    }

    if (!meta.director.empty()) {
        this->labelDirector->setText("Directed by " + join(meta.director));
        this->labelDirector->setVisibility(brls::Visibility::VISIBLE);
    }
}

void StremioDetail::onAction() {
    if (this->item.type == "series") {
        // Reuse the episodes we already fetched; fall back to the fetching
        // constructor if the meta hasn't arrived (or failed).
        if (!this->videos.empty())
            brls::Application::pushActivity(
                new brls::Activity(new StremioSeries(this->item.name, this->videos, this->background)),
                brls::TransitionAnimation::NONE);
        else
            brls::Application::pushActivity(
                new brls::Activity(new StremioSeries(this->item)), brls::TransitionAnimation::NONE);
        return;
    }
    this->openStreams();
}

void StremioDetail::openStreams(bool downloadMode) {
    if (this->fetching) return;  // ignore repeat presses while a request runs
    if (stremio::STREAM_ADDONS.empty()) {
        brls::Application::notify("Set your stream addon first (press − on the home screen)");
        return;
    }
    this->setFetching(true);

    std::string name = this->item.name;
    ResumeEntry key{this->item.type, this->item.id, this->item.name, this->item.poster};

    auto picker = std::make_shared<StreamPickerHandle>();
    ASYNC_RETAIN
    stremio::getStreams(this->item.type, this->item.id,
        [ASYNC_TOKEN, picker, name, key, downloadMode](std::vector<stremio::Stream> streams, size_t pending) {
            if (*token) return;
            if (!picker->opened()) {
                this->setFetching(false);
                auto* view = new StreamPicker(name, streams, key, downloadMode, {}, {}, pending);
                picker->attach(view);
                brls::Application::pushActivity(new brls::Activity(view));
            } else {
                picker->append(std::move(streams), pending);
            }
        },
        [ASYNC_TOKEN, picker]() {
            picker->finish();
            ASYNC_RELEASE
            this->setFetching(false);
        },
        [ASYNC_TOKEN](const std::string& e) {
            if (*token) return;
            brls::Application::notify("Stream error: " + e);
        });
}

void StremioDetail::setFetching(bool fetching) {
    this->fetching = fetching;
    this->labelWatchButton->setText(fetching ? "Loading streams..."
        : (this->item.type == "series" ? "▶  Episodes" : "▶  Watch"));
    if (this->labelDownloadButton)
        this->labelDownloadButton->setText(fetching ? "Loading streams..." : "⬇  Download");
    this->btnWatch->setBackgroundColor(fetching ? theme::BG_DISABLED : COL_ACCENT);
    if (this->btnDownload)
        this->btnDownload->setBackgroundColor(fetching ? theme::BG_DISABLED : theme::BG_BUTTON);
}
