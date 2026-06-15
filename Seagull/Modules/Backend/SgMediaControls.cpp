#include "SgMediaControls.h"

#include <QDir>
#include <QUrl>
#include <QMetaObject>

#include <string_view>
#include <chrono>

// C++/WinRT consumption of the SMTC. The WinRT headers ship with the Windows SDK
// (the cppwinrt include dir the MSVC Developer environment adds); the ABI interop
// interface that hands us an SMTC for an HWND lives in the SDK's um headers.
// (Qt's own headers are included first, above, so NOMINMAX can't break them.)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shobjidl_core.h> // SetCurrentProcessExplicitAppUserModelID
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.Media.h>
#include <systemmediatransportcontrolsinterop.h>

using namespace winrt;
using namespace winrt::Windows::Foundation;
using namespace winrt::Windows::Media;
using namespace winrt::Windows::Storage::Streams;

namespace {
// QString (UTF-16) -> winrt::hstring without a transcode (wchar_t is 16-bit here).
winrt::hstring toHString(const QString& s) {
    return winrt::hstring{ std::wstring_view{
        reinterpret_cast<const wchar_t*>(s.utf16()), static_cast<size_t>(s.size()) } };
}
}

struct SgMediaControls::Impl {
    SystemMediaTransportControls               smtc{ nullptr };
    SystemMediaTransportControlsDisplayUpdater updater{ nullptr };
    SystemMediaTransportControls::ButtonPressed_revoker buttonRevoker;
    bool available = false;

    // Cached display metadata, re-asserted whenever the session goes active. A
    // DisplayUpdater.Update() that lands while PlaybackStatus is Closed is dropped
    // by Windows, leaving the card blank; re-pushing on enable/Playing fixes it.
    QString title;
    QString artist;
    bool    isVideo = false;
    bool    hasMeta = false;
};

void SgMediaControls::registerAppIdentity() {
    // The id just needs to be a stable, unique, shell-legal string. Windows shows
    // a friendlier name/icon only if a Start-menu shortcut carries the same id
    // (an installer concern); the metadata card populates regardless once set.
    SetCurrentProcessExplicitAppUserModelID(L"Seagull.MediaPlayer");
}

SgMediaControls::SgMediaControls(QObject* parent) : QObject(parent), d(new Impl) {}

SgMediaControls::~SgMediaControls() {
    if (d) {
        d->buttonRevoker.revoke(); // no callbacks land after we're gone
        delete d;
    }
}

bool SgMediaControls::isAvailable() const { return d->available; }

void SgMediaControls::attachToWindow(void* hwnd) {
    if (!hwnd || d->available) return;
    try {
        // Desktop apps get their SMTC per top-level window, via the interop factory
        // (there's no app-wide MediaPlayer to hang it off of).
        auto interop = get_activation_factory<SystemMediaTransportControls,
                                              ISystemMediaTransportControlsInterop>();
        SystemMediaTransportControls smtc{ nullptr };
        check_hresult(interop->GetForWindow(static_cast<HWND>(hwnd),
            guid_of<SystemMediaTransportControls>(), put_abi(smtc)));

        d->smtc    = smtc;
        d->updater = smtc.DisplayUpdater();

        smtc.IsEnabled(false);       // off until media is loaded
        smtc.IsPlayEnabled(true);
        smtc.IsPauseEnabled(true);
        smtc.IsNextEnabled(true);
        smtc.IsPreviousEnabled(true);
        smtc.IsStopEnabled(false);   // the player has no neutral "stop", only teardown

        // ButtonPressed fires on a WinRT thread-pool thread; marshal each press to
        // this object's (GUI) thread before touching anything Qt.
        auto* self = this;
        d->buttonRevoker = smtc.ButtonPressed(winrt::auto_revoke,
            [self](SystemMediaTransportControls const&,
                   SystemMediaTransportControlsButtonPressedEventArgs const& args) {
                const auto button = args.Button();
                QMetaObject::invokeMethod(self, [self, button]() {
                    switch (button) {
                    case SystemMediaTransportControlsButton::Play:     emit self->playPressed();     break;
                    case SystemMediaTransportControlsButton::Pause:    emit self->pausePressed();    break;
                    case SystemMediaTransportControlsButton::Next:     emit self->nextPressed();     break;
                    case SystemMediaTransportControlsButton::Previous: emit self->previousPressed(); break;
                    case SystemMediaTransportControlsButton::Stop:     emit self->stopPressed();     break;
                    default: break;
                    }
                }, Qt::QueuedConnection);
            });

        d->available = true;
    } catch (...) {
        d->available = false; // unsupported OS / no SMTC — stay inert
    }
}

// Push the cached title/artist into the DisplayUpdater. Safe to call repeatedly;
// re-asserting after the session is enabled / goes Playing is exactly the point.
void SgMediaControls::applyDisplay() {
    if (!d->available || !d->hasMeta) return;
    try {
        if (d->isVideo) {
            d->updater.Type(MediaPlaybackType::Video);
            auto props = d->updater.VideoProperties();
            props.Title(toHString(d->title));
            if (!d->artist.isEmpty()) props.Subtitle(toHString(d->artist));
        } else {
            d->updater.Type(MediaPlaybackType::Music);
            auto props = d->updater.MusicProperties();
            props.Title(toHString(d->title));
            props.Artist(toHString(d->artist));
        }
        d->updater.Update();
    } catch (...) {}
}

void SgMediaControls::setEnabled(bool on) {
    if (!d->available) return;
    try { d->smtc.IsEnabled(on); } catch (...) {}
    if (on) applyDisplay(); // re-assert metadata now that the session is live
}

void SgMediaControls::setPlaybackStatus(Status status) {
    if (!d->available) return;
    MediaPlaybackStatus s = MediaPlaybackStatus::Closed;
    switch (status) {
    case Status::Playing: s = MediaPlaybackStatus::Playing; break;
    case Status::Paused:  s = MediaPlaybackStatus::Paused;  break;
    case Status::Stopped: s = MediaPlaybackStatus::Stopped; break;
    case Status::Closed:  s = MediaPlaybackStatus::Closed;  break;
    }
    try { d->smtc.PlaybackStatus(s); } catch (...) {}
    // Going active: re-push the display, in case the first Update landed while the
    // session was still Closed (Windows drops those, leaving the card blank).
    if (status == Status::Playing || status == Status::Paused) applyDisplay();
}

void SgMediaControls::setMetadata(const QString& title, const QString& artist, bool isVideo) {
    if (!d->available) return;
    d->title   = title;
    d->artist  = artist;
    d->isVideo = isVideo;
    d->hasMeta = true;
    applyDisplay();
}

void SgMediaControls::setThumbnail(const QImage& image) {
    if (!d->available || image.isNull()) return;
    try {
        // Write to a stable temp PNG and hand SMTC a file URI. CreateFromUri is
        // synchronous (the stream is opened lazily by the OS when it draws the
        // widget), so we avoid blocking on a WinRT async from the STA GUI thread.
        const QString path = QDir(QDir::tempPath()).filePath(QStringLiteral("seagull_smtc_thumb.png"));
        if (!image.save(path, "PNG")) return;
        Uri uri{ toHString(QUrl::fromLocalFile(path).toString()) }; // file:///C:/...
        d->updater.Thumbnail(RandomAccessStreamReference::CreateFromUri(uri));
        d->updater.Update();
    } catch (...) {}
}

void SgMediaControls::setTimeline(qint64 positionMs, qint64 durationMs) {
    if (!d->available || durationMs <= 0) return;
    try {
        using namespace std::chrono;
        SystemMediaTransportControlsTimelineProperties tl;
        tl.StartTime(milliseconds(0));
        tl.MinSeekTime(milliseconds(0));
        tl.Position(milliseconds(positionMs));
        tl.MaxSeekTime(milliseconds(durationMs));
        tl.EndTime(milliseconds(durationMs));
        d->smtc.UpdateTimelineProperties(tl);
    } catch (...) {}
}

void SgMediaControls::clear() {
    if (!d->available) return;
    d->hasMeta = false;
    d->title.clear();
    d->artist.clear();
    try {
        d->updater.ClearAll();
        d->updater.Update();
        d->smtc.PlaybackStatus(MediaPlaybackStatus::Closed);
        d->smtc.IsEnabled(false);
    } catch (...) {}
}
