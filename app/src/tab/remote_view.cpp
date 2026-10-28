#include "tab/remote_view.hpp"
#include "view/recycling_grid.hpp"
#include "view/svg_image.hpp"
#include "view/video_view.hpp"
#include "view/video_profile.hpp"
#include "view/mpv_core.hpp"
#include "view/music_view.hpp"
#include "view/player_setting.hpp"
#include "client/local.hpp"
#include "utils/misc.hpp"
#include "utils/config.hpp"
#include "utils/autoplay.hpp"
#include "utils/intro_markers.hpp"
#include "utils/sub_loader.hpp"
#include "utils/theme.hpp"

// mpv_core.hpp pulls in windows.h (via glad.h) *after* borealis/core/view.hpp
// already did its own "#undef ABSOLUTE" -- windows.h's own ABSOLUTE macro
// (wingdi.h, graphics mode constant) comes back and shadows
// brls::PositionType::ABSOLUTE below. Same fix borealis uses internally.
#ifdef ABSOLUTE
#undef ABSOLUTE
#endif

using namespace brls::literals;

class RemotePlayer : public brls::Box {
public:
    // `streamId` ("ttID:season:episode" for a series episode) is what lets
    // the intro-skip markers below be scoped to "this season", not the whole
    // app -- see utils/intro_markers.hpp. Left empty for anything that isn't
    // a Stremio series episode (movies, downloads without series context,
    // webdav, ...), which quietly disables both marking and the auto-prompt.
    RemotePlayer(const remote::DirEntry& item, const std::string& streamId = "") {
        size_t p1 = streamId.find(':');
        size_t p2 = (p1 == std::string::npos) ? std::string::npos : streamId.find(':', p1 + 1);
        if (p1 != std::string::npos && p2 != std::string::npos) {
            this->seriesId = streamId.substr(0, p1);
            try {
                this->seasonIndex = std::stoi(streamId.substr(p1 + 1, p2 - p1 - 1));
            } catch (...) {
                this->seriesId.clear();
            }
        }

        float width = brls::Application::contentWidth;
        float height = brls::Application::contentHeight;
        view->setDimensions(width, height);
        view->setWidthPercentage(100);
        view->setHeightPercentage(100);
        view->setId("video");
        view->setTitie(item.name);
        view->hideVideoQuality();
        this->setDimensions(width, height);
        this->addView(view);

        // Bottom-left "Skip intro?" prompt, shown automatically once the
        // season has markers (see the UPDATE_PROGRESS case below). Built once
        // and toggled, not recreated each time, since it can show up many
        // times across a binge session.
        this->introToast = new brls::Box();
        this->introToast->setPositionType(brls::PositionType::ABSOLUTE);
        this->introToast->setPositionBottom(40);
        this->introToast->setPositionLeft(40);
        this->introToast->setAxis(brls::Axis::COLUMN);
        this->introToast->setPadding(14, 20, 14, 20);
        this->introToast->setCornerRadius(8);
        this->introToast->setBackgroundColor(theme::BG_CARD);
        this->introToast->setVisibility(brls::Visibility::GONE);
        auto* introTitle = new brls::Label();
        introTitle->setText("Skip intro?");
        introTitle->setFontSize(20);
        introTitle->setTextColor(theme::TEXT_PRIMARY);
        introTitle->setMarginBottom(4);
        this->introToast->addView(introTitle);
        this->introToastCountdown = new brls::Label();
        this->introToastCountdown->setFontSize(16);
        this->introToastCountdown->setTextColor(theme::TEXT_SECONDARY);
        this->introToast->addView(this->introToastCountdown);
        this->addView(this->introToast);

        if (item.type == remote::EntryType::PLAYLIST) {
            view->hideVideoProgressSlider();
        } else if (item.name.size() > 0) {
            titles.push_back(item.name);
        }

        auto& mpv = MPVCore::instance();
        eventSubscribeID = mpv.getEvent()->subscribe([this](MpvEventEnum event) {
            auto& mpv = MPVCore::instance();
            switch (event) {
            case MpvEventEnum::MPV_LOADED: {
                if (titles.empty()) this->loadList();
                view->getProfile()->init("Local");
                const char* flag = MPVCore::SUBS_FALLBACK ? "select" : "auto";
                for (auto& it : this->subtitles) {
                    mpv.command("sub-add", it.second.c_str(), flag, it.first.c_str());
                }
                this->introToastShownThisPlayback = false;
                this->pendingIntroMarkStart = -1;
                break;
            }
            case MpvEventEnum::UPDATE_PROGRESS: {
                if (this->seriesId.empty() || this->introToastShownThisPlayback || *this->introToastActive) break;
                IntroMarkers::Marker marker;
                if (!IntroMarkers::instance().get(this->seriesId, this->seasonIndex, marker)) break;
                if (mpv.playback_time >= marker.start && mpv.playback_time < marker.end) {
                    this->introToastShownThisPlayback = true;
                    this->introMarkerEnd = marker.end;
                    this->showIntroToast();
                }
                break;
            }
            default:;
            }
        });
        settingSubscribeID = view->getSettingEvent()->subscribe([]() {
            brls::View* setting = new PlayerSetting();
            brls::Application::pushActivity(new brls::Activity(setting));
        });
        // ZR is dual-purpose, both hidden from the OSD hint bar: if the
        // auto-prompt is showing, ZR confirms it (skip to the marked end);
        // otherwise, on a series episode, it captures a mark -- the first
        // press is "intro starts here", the second is "intro ends here",
        // saved for the whole season. Re-pressing after a save just starts a
        // fresh mark, in case the first attempt was off.
        this->registerAction("", brls::BUTTON_RT, [this](brls::View*) {
            auto& mpv = MPVCore::instance();
            if (*this->introToastActive) {
                *this->introToastActive = false;
                this->introToast->setVisibility(brls::Visibility::GONE);
                mpv.seek(this->introMarkerEnd, "absolute");
                brls::Application::notify("Skipped intro");
                return true;
            }
            if (this->seriesId.empty()) return false;
            if (this->pendingIntroMarkStart < 0) {
                this->pendingIntroMarkStart = mpv.playback_time;
                brls::Application::notify(
                    fmt::format("Intro start marked at {:.0f}s -- press ZR again where it ends",
                        this->pendingIntroMarkStart));
            } else {
                double start = this->pendingIntroMarkStart;
                double end = mpv.playback_time;
                this->pendingIntroMarkStart = -1;
                if (end <= start) {
                    brls::Application::notify("Intro end must be after the start -- try again");
                    return true;
                }
                IntroMarkers::instance().set(this->seriesId, this->seasonIndex, start, end);
                brls::Application::notify(
                    fmt::format("Intro markers saved for this season: {:.0f}s-{:.0f}s", start, end));
            }
            return true;
        });
    }

    // Shows the bottom-left prompt and counts down 5s on its own; ZR (above)
    // confirms early, otherwise it just hides once time's up and the intro
    // plays out normally -- either way this playback's one-shot flag (set by
    // the caller) keeps it from popping up again for the same episode.
    void showIntroToast() {
        *this->introToastActive = true;
        this->introToast->setVisibility(brls::Visibility::VISIBLE);
        auto secondsLeft = std::make_shared<int>(5);
        this->introToastCountdown->setText(fmt::format("ZR to skip · {}s", *secondsLeft));

        auto active = this->introToastActive;
        brls::Box* toast = this->introToast;
        brls::Label* label = this->introToastCountdown;
        auto tick = std::make_shared<std::function<void()>>();
        *tick = [active, secondsLeft, toast, label, tick]() {
            if (!*active) return;  // confirmed via ZR already
            if (--(*secondsLeft) <= 0) {
                *active = false;
                toast->setVisibility(brls::Visibility::GONE);
                return;
            }
            label->setText(fmt::format("ZR to skip · {}s", *secondsLeft));
            brls::delay(1000, *tick);
        };
        brls::delay(1000, *tick);
    }

    ~RemotePlayer() override {
        auto& mpv = MPVCore::instance();
        mpv.getEvent()->unsubscribe(eventSubscribeID);
        view->getPlayEvent()->unsubscribe(playSubscribeID);
        view->getSettingEvent()->unsubscribe(settingSubscribeID);
        mpv.command("write-watch-later-config");
        // If this player suppressed its own end-of-file close for autoplay
        // (see setUrl() above) but is being destroyed anyway -- e.g. the user
        // pressed B while the next-episode lookup was still in flight --
        // invalidate that lookup/dialog so it doesn't act on some other
        // screen. Comparing generations (instead of unconditionally calling
        // disarm()) is what keeps this from also firing on the NORMAL
        // successful chain: there, AutoplayManager::startPlayback() pops this
        // same activity, but only after its completion callback already
        // called arm() for the new episode -- Application::popActivity runs
        // that callback BEFORE freeing/deleting the old activity -- so the
        // generation has already moved on and this correctly does nothing.
        if (this->autoplaySuppressedClose &&
            this->autoplayGenAtEOF == AutoplayManager::instance().currentGeneration()) {
            AutoplayManager::instance().disarm();
        }
    }

#ifdef ANDROID
    void willDisappear(bool resetState) override {
        if (brls::Application::getThemeVariant() == brls::ThemeVariant::LIGHT)
            brls::Application::getTheme().addColor("brls/clear", nvgRGBA(235, 235, 235, 255));
        else
            brls::Application::getTheme().addColor("brls/clear", nvgRGBA(45, 45, 45, 255));
    }

    void willAppear(bool resetState) override {
        brls::Application::getTheme().addColor("brls/clear", nvgRGBA(0, 0, 0, 0));
    }
#endif

    void setList(const DirList& list, size_t index, const std::string& extra) {
        // 播放列表
        DirList urls;
        for (size_t i = 1; i < list.size(); i++) {
            auto& it = list.at(i);
            if (it.type == remote::EntryType::VIDEO) {
                if (i == index) index = urls.size();
                titles.push_back(it.name);
                urls.push_back(it);
            }
        }
        if (titles.size() > 1) view->setList(titles, index);

        playSubscribeID = view->getPlayEvent()->subscribe([this, list, urls, extra](int index) {
            if (index < 0 || index >= (int)urls.size()) {
                return VideoView::close(true);
            }
            MPVCore::instance().reset();
            auto& item = urls.at(index);

            std::string name = item.name;
            auto pos = name.find_last_of(".");
            if (pos != std::string::npos) {
                name = name.substr(0, pos);
            }

            this->subtitles.clear();
            for (auto& s : list) {
                if (s.type == remote::EntryType::SUBTITLE) {
                    if (!s.name.rfind(name, 0)) {
                        this->subtitles.insert(std::make_pair(s.name.substr(pos), s.url()));
                    }
                }
            }
            this->url = item.url();
            MPVCore::instance().setUrl(this->url, extra);
            view->setTitie(name);
            return true;
        });

        view->getPlayEvent()->fire(index);
    }

    void setUrl(const std::string& path) {
        // No explicit reset() here: `view` is a freshly-constructed VideoView
        // (see its ctor) which already calls MPVCore::reset() once. A second
        // reset() here just queues a redundant async "stop" -- since stop is
        // async, its MPV_STOP could arrive late, after this title's own
        // setCurrent()/SubLoader::prepare() already ran, and wipe out ITS
        // resume/subtitle state instead of the old title's.
        playSubscribeID = view->getPlayEvent()->subscribe([this](int index) {
            auto& autoplay = AutoplayManager::instance();
            if (autoplay.willHandle()) {
                // It owns the transition from here (hold this frozen frame,
                // show "up next", chain or close itself once it knows there's
                // nothing to offer). Remember the generation so the
                // destructor below can tell whether that actually happened.
                this->autoplaySuppressedClose = true;
                this->autoplayGenAtEOF = autoplay.currentGeneration();
                return true;
            }
            return VideoView::close(true);
        });
        MPVCore::instance().setUrl(path);
    }

    void loadList() {
        auto& mpv = MPVCore::instance();
        int64_t count = mpv.getInt("playlist-count");
        for (int64_t n = 0; n < count; n++) {
            auto key = fmt::format("playlist/{}/title", n);
            titles.push_back(mpv.getString(key));
        }
        if (titles.size() > 1) view->setList(titles, 0);
        view->setTitie(titles.front());

        playSubscribeID = view->getPlayEvent()->subscribe([this, &mpv](int index) {
            if (index < 0 || index >= (int)titles.size()) {
                return VideoView::close();
            }
            MPVCore::instance().reset();
            view->setTitie(titles.at(index));
            mpv.command("playlist-play-index", std::to_string(index).c_str());
            return true;
        });
    }

private:
    VideoView* view = new VideoView();
    std::string url;
    std::vector<std::string> titles;
    std::unordered_map<std::string, std::string> subtitles;
    MPVEvent::Subscription eventSubscribeID;
    brls::Event<int>::Subscription playSubscribeID;
    brls::VoidEvent::Subscription settingSubscribeID;
    bool autoplaySuppressedClose = false;
    int autoplayGenAtEOF = 0;

    std::string seriesId;
    int seasonIndex = 0;
    double pendingIntroMarkStart = -1;
    double introMarkerEnd = 0;
    bool introToastShownThisPlayback = false;
    std::shared_ptr<bool> introToastActive = std::make_shared<bool>(false);
    brls::Box* introToast = nullptr;
    brls::Label* introToastCountdown = nullptr;
};

class FileCard : public RecyclingGridItem {
public:
    FileCard() { this->inflateFromXMLRes("xml/view/dir_entry.xml"); }

    void setCard(const remote::DirEntry& item) {
        if (item.type == remote::EntryType::UP) {
            this->icon->setImageFromSVGRes("icon/ico-folder-up.svg");
            this->name->setText("main/remote/up"_i18n);
            this->size->setText("");
            return;
        }
        this->name->setText(item.name);
        if (item.type == remote::EntryType::DIR) {
            this->icon->setImageFromSVGRes("icon/ico-folder.svg");
            this->size->setText("main/remote/folder"_i18n);
            return;
        }
        if (item.type == remote::EntryType::DEVICE) {
            this->icon->setImageFromSVGRes("icon/ico-folder.svg");
            this->size->setText(item.path);
            return;
        }
        this->size->setText(misc::formatSize(item.fileSize));
        switch (item.type) {
        case remote::EntryType::VIDEO:
            this->icon->setImageFromSVGRes("icon/ico-file-video.svg");
            break;
        case remote::EntryType::AUDIO:
            this->icon->setImageFromSVGRes("icon/ico-file-audio.svg");
            break;
        case remote::EntryType::IMAGE:
            this->icon->setImageFromSVGRes("icon/ico-file-image.svg");
            break;
        case remote::EntryType::PLAYLIST:
            this->icon->setImageFromSVGRes("icon/ico-list.svg");
            break;
        default:
            this->icon->setImageFromSVGRes("icon/ico-file.svg");
        }
    }

private:
    BRLS_BIND(SVGImage, icon, "file/icon");
    BRLS_BIND(brls::Label, name, "file/name");
    BRLS_BIND(brls::Label, size, "file/misc");
};

static std::set<std::string> videoExt = {
    ".mp4", ".mkv", ".avi", ".flv", ".mov", ".wmv", ".webm", ".rm", ".rmvb", ".mpg"};
static std::set<std::string> audioExt = {".mp3", ".flac", ".wav", ".ogg", ".m4a", ".aac", ".wma", ".ape"};
static std::set<std::string> imageExt = {".jpg", ".jpeg", ".png", ".bmp", ".gif", ".webp"};
static std::set<std::string> playlistExt = {".m3u", ".m3u8"};
static std::set<std::string> subtitleExt = {".srt", ".ass", ".ssa", ".sub", ".smi"};

class FileDataSource : public RecyclingGridDataSource {
public:
    FileDataSource(const DirList& r, RemoteView::Client c) : list(std::move(r)), client(c) {
        for (auto& it : this->list) {
            if (it.type != remote::EntryType::FILE) continue;

            auto pos = it.name.find_last_of('.');
            if (pos == std::string::npos) continue;
            std::string ext = it.name.substr(pos);
            std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
            if (videoExt.count(ext)) {
                it.type = remote::EntryType::VIDEO;
            } else if (audioExt.count(ext)) {
                it.type = remote::EntryType::AUDIO;
            } else if (imageExt.count(ext)) {
                it.type = remote::EntryType::IMAGE;
            } else if (subtitleExt.count(ext)) {
                it.type = remote::EntryType::SUBTITLE;
            } else if (playlistExt.count(ext)) {
                it.type = remote::EntryType::PLAYLIST;
            }
        }
    }

    size_t getItemCount() override { return this->list.size(); }

    RecyclingGridItem* cellForRow(RecyclingView* recycler, size_t index) override {
        FileCard* cell = dynamic_cast<FileCard*>(recycler->dequeueReusableCell("Cell"));
        auto& item = this->list.at(index);
        cell->setCard(item);
        return cell;
    }

    void onItemSelected(brls::Box* recycler, size_t index) override {
        auto& item = this->list.at(index);
        if (item.type == remote::EntryType::UP) {
            recycler->getParent()->dismiss();
            return;
        }

        if (item.type == remote::EntryType::DIR || item.type == remote::EntryType::DEVICE) {
            auto* view = dynamic_cast<RemoteView*>(recycler->getParent());
            if (view) view->push(item.path);
            return;
        }

        if (item.type == remote::EntryType::VIDEO) {
            RemotePlayer* view = new RemotePlayer(item);
            view->setList(this->list, index, client->extraOption());
            brls::Application::pushActivity(new brls::Activity(view), brls::TransitionAnimation::NONE);
            return;
        }

        if (item.type == remote::EntryType::AUDIO) {
            DirList urls;
            for (size_t i = 1; i < this->list.size(); i++) {
                auto& it = this->list.at(i);
                if (it.type == remote::EntryType::AUDIO) {
                    if (i == index) index = urls.size();
                    urls.push_back(it);
                }
            }
            MusicView::instance().load(urls, index, client->extraOption());
            return;
        }

        if (item.type == remote::EntryType::IMAGE) {
            return;
        }

        if (item.type == remote::EntryType::PLAYLIST) {
            RemotePlayer* view = new RemotePlayer(item);
            MPVCore::instance().setUrl(item.url(), client->extraOption());
            brls::Application::pushActivity(new brls::Activity(view), brls::TransitionAnimation::NONE);
        }
    }

    void clearData() override { this->list.clear(); }

private:
    DirList list;
    RemoteView::Client client;
};

UmsView::UmsView() : RemoteView(std::make_shared<remote::Local>()) {
    RecyclingGrid* view = this->newRecycler();
    this->stack.push_back(view);
    this->setContent(view);

    auto ev = Ums::instance().getEvent();
    deviceSubscribeID = ev->subscribe([this, view](const Ums::DeviceList& r) {
        DirList dirs;
        dirs.reserve(r.size());
        for (auto& it : r) {
            remote::DirEntry entry;
            entry.type = it.id < 0 ? remote::EntryType::DIR : remote::EntryType::DEVICE;
            entry.name = it.name;
            entry.path = it.mount + "/";
            dirs.push_back(entry);
        }
        view->setDataSource(new FileDataSource(dirs, this->client));
    });
    ev->fire(Ums::instance().getDevice());
}

UmsView::~UmsView() { Ums::instance().getEvent()->unsubscribe(deviceSubscribeID); }

RemoteView::RemoteView(Client c) : client(c) { brls::Logger::debug("RemoteView: create"); }

RemoteView::~RemoteView() {
    brls::Logger::debug("RemoteView: deleted");
    this->setDimensions(View::AUTO, View::AUTO);
    PlayerSetting::selectedSubtitle = 0;
    PlayerSetting::selectedAudio = 0;

    /// 通知 MusicView 已关闭
    MusicView::instance().setParent(nullptr);
}

brls::View* RemoteView::getDefaultFocus() { return this->recycler; }

void RemoteView::push(const std::string& path) {
    RecyclingGrid* view = this->newRecycler();
    this->stack.push_back(view);
    this->setContent(view);

    ASYNC_RETAIN
    brls::async([ASYNC_TOKEN, &path]() {
        try {
            auto r = client->list(path);
            brls::sync([ASYNC_TOKEN, r]() {
                ASYNC_RELEASE
                this->recycler->setDataSource(new FileDataSource(r, client));
                if (this->stack.size() > 1) brls::Application::giveFocus(this->recycler);
            });
        } catch (const std::exception& ex) {
            std::string error = ex.what();
            brls::sync([ASYNC_TOKEN, error]() {
                ASYNC_RELEASE
                this->recycler->setError(error);
            });
        }
    });
}

void RemoteView::dismiss(std::function<void(void)> cb) {
    if (this->stack.size() > 1) {
        brls::View* lastView = this->recycler;
        this->stack.pop_back();
        this->setContent(this->stack.back());
        cb();
        lastView->freeView();
    } else if (brls::Application::getInputType() == brls::InputType::TOUCH) {
        brls::View::dismiss(cb);
    } else {
        AutoTabFrame::focus2Sidebar(this);
    }
}

void RemoteView::setContent(RecyclingGrid* view) {
    if (this->recycler) {
        this->removeView(this->recycler, false);
        this->recycler = nullptr;
    }

    this->recycler = view;
    this->recycler->setDimensions(View::AUTO, View::AUTO);
    this->recycler->setGrow(1.0f);
    this->addView(this->recycler);
    brls::Application::giveFocus(this->recycler);
}

RecyclingGrid* RemoteView::newRecycler() {
    RecyclingGrid* view = new RecyclingGrid();
    view->spanCount = 1;
    view->estimatedRowHeight = 48;
    view->estimatedRowSpace = 10;
    view->setDefaultCellFocus(1);
    view->registerCell("Cell", []() { return new FileCard(); });
    view->registerAction("hints/back"_i18n, brls::BUTTON_B, [this](...) {
        this->dismiss();
        return true;
    });
    return view;
}

void RemoteView::play(const std::string& path, const std::string& name, const std::string& streamId) {
    // Default to no autoplay-next-episode for whatever's about to play; the
    // Stremio stream picker re-arms it right after this call for series
    // episodes specifically. Without this, a stale arm from a previously
    // watched episode could fire a "next episode" countdown over an unrelated
    // video (a downloaded movie, a webdav file, ...) that just happens to end.
    AutoplayManager::instance().disarm();
    // Same reasoning for subtitles: a request left in flight from whatever
    // played before (Stremio streams call SubLoader::prepare() themselves
    // right after this) would otherwise attach a stale, wrong-title subtitle
    // to this playback once it resolves -- most relevantly, opening a
    // download or webdav file never calls prepare() itself.
    SubLoader::instance().cancelPending();
    RemotePlayer* view = new RemotePlayer({remote::EntryType::VIDEO, name, path}, streamId);
    brls::Application::pushActivity(new brls::Activity(view), brls::TransitionAnimation::NONE);
    view->setUrl(path);
}
