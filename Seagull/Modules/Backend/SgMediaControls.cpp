#include "SgMediaControls.h"

#include <QDir>
#include <QUrl>
#include <QMetaObject>
#include <QStandardPaths>
#include <QCoreApplication>

#include <string>
#include <string_view>
#include <cstring>
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
#include <shellapi.h>      // ShellExecuteW (elevated launch for the Defender exclusion)
#include <shobjidl_core.h> // SetCurrentProcessExplicitAppUserModelID
#include <shlobj.h>        // IShellLinkW, CLSID_ShellLink, IPersistFile
#include <propsys.h>       // IPropertyStore (interface only; no propsys.lib needed)
#include <propidl.h>       // PROPVARIANT
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

// The process AppUserModelID — must match the Start-menu shortcut for Windows to
// resolve our name/icon on the SMTC card.
constexpr wchar_t kAumid[] = L"Seagull.MediaPlayer";

// PKEY_AppUserModel_ID = {9F4C2855-9F79-4B39-A8D0-E1D42DE1D5F3}, pid 5 (propkey.h).
// Defined inline so we don't need to pull in the propkey GUID lib / INITGUID.
const PROPERTYKEY kPkeyAppUserModelId =
    { { 0x9F4C2855, 0x9F79, 0x4B39, { 0xA8,0xD0,0xE1,0xD4,0x2D,0xE1,0xD5,0xF3 } }, 5 };

inline const wchar_t* wstr(const QString& s) {
    return reinterpret_cast<const wchar_t*>(s.utf16());
}

// Write a .lnk to `target`, stamped with the AppUserModelID (so SMTC/taskbar
// resolve our identity). Best-effort; returns false on any COM failure.
bool writeShortcut(const QString& lnkPath, const QString& target, const QString& workdir) {
    IShellLinkW* link = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
            IID_IShellLinkW, reinterpret_cast<void**>(&link))) || !link)
        return false;

    link->SetPath(wstr(target));
    if (!workdir.isEmpty()) link->SetWorkingDirectory(wstr(workdir));
    link->SetDescription(L"Seagull media player");

    // Stamp the AUMID via the property store.
    IPropertyStore* store = nullptr;
    if (SUCCEEDED(link->QueryInterface(IID_PPV_ARGS(&store))) && store) {
        PROPVARIANT pv;
        PropVariantInit(&pv);
        pv.vt = VT_LPWSTR;
        const size_t bytes = (wcslen(kAumid) + 1) * sizeof(wchar_t);
        pv.pwszVal = static_cast<PWSTR>(CoTaskMemAlloc(bytes));
        if (pv.pwszVal) {
            std::memcpy(pv.pwszVal, kAumid, bytes);
            store->SetValue(kPkeyAppUserModelId, pv);
            store->Commit();
        }
        PropVariantClear(&pv);
        store->Release();
    }

    bool ok = false;
    IPersistFile* file = nullptr;
    if (SUCCEEDED(link->QueryInterface(IID_IPersistFile, reinterpret_cast<void**>(&file))) && file) {
        ok = SUCCEEDED(file->Save(wstr(lnkPath), TRUE));
        file->Release();
    }
    link->Release();
    return ok;
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
    // (see createStartMenuShortcut); the metadata card populates regardless.
    SetCurrentProcessExplicitAppUserModelID(kAumid);
}

void SgMediaControls::createDesktopShortcut() {
    const QString exe = QDir::toNativeSeparators(QCoreApplication::applicationFilePath());
    const QString dir = QDir::toNativeSeparators(QCoreApplication::applicationDirPath());
    const QString lnk = QDir(QStandardPaths::writableLocation(QStandardPaths::DesktopLocation))
                            .filePath(QStringLiteral("Seagull.lnk"));
    writeShortcut(QDir::toNativeSeparators(lnk), exe, dir);
}

void SgMediaControls::createStartMenuShortcut() {
    const QString exe = QDir::toNativeSeparators(QCoreApplication::applicationFilePath());
    const QString dir = QDir::toNativeSeparators(QCoreApplication::applicationDirPath());
    // ApplicationsLocation on Windows == the per-user Start Menu\Programs folder.
    const QString lnk = QDir(QStandardPaths::writableLocation(QStandardPaths::ApplicationsLocation))
                            .filePath(QStringLiteral("Seagull.lnk"));
    writeShortcut(QDir::toNativeSeparators(lnk), exe, dir);
}

namespace {
// This app's install folder, with any single quote escaped so it can't break out
// of a PowerShell single-quoted string literal.
QString defenderExclusionDir() {
    QString dir = QDir::toNativeSeparators(QCoreApplication::applicationDirPath());
    dir.replace(QLatin1Char('\''), QLatin1String("''"));
    return dir;
}

// Run a one-line Defender PowerShell command elevated (UAC). "runas" shows the
// consent prompt; we wait for the hidden PowerShell to exit so the caller can
// trust the result. Returns true only if it launched and exited cleanly (0) —
// declining UAC or any failure returns false and leaves Defender unchanged.
bool runDefenderCommandElevated(const QString& innerCommand) {
    const QString args = QStringLiteral(
        "-NoProfile -WindowStyle Hidden -Command \"%1\"").arg(innerCommand);
    const std::wstring wargs = args.toStdWString();

    SHELLEXECUTEINFOW sei{};
    sei.cbSize       = sizeof(sei);
    sei.fMask        = SEE_MASK_NOCLOSEPROCESS; // keep the handle so we can wait on it
    sei.lpVerb       = L"runas";
    sei.lpFile       = L"powershell.exe";
    sei.lpParameters = wargs.c_str();
    sei.nShow        = SW_HIDE;

    if (!ShellExecuteExW(&sei) || !sei.hProcess) return false; // declined / failed to launch
    WaitForSingleObject(sei.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(sei.hProcess, &code);
    CloseHandle(sei.hProcess);
    return code == 0;
}
} // namespace

bool SgMediaControls::addDefenderExclusion() {
    return runDefenderCommandElevated(
        QStringLiteral("Add-MpPreference -ExclusionPath '%1'").arg(defenderExclusionDir()));
}

bool SgMediaControls::removeDefenderExclusion() {
    return runDefenderCommandElevated(
        QStringLiteral("Remove-MpPreference -ExclusionPath '%1'").arg(defenderExclusionDir()));
}

QString SgMediaControls::defenderExclusionQueryCommand() {
    // No elevation needed to read the exclusion list. -contains is case-insensitive,
    // and we added the path exactly as defenderExclusionDir() produces it, so a plain
    // membership test matches. Prints YES / NO on stdout.
    return QStringLiteral(
        "$d='%1'; $e=(Get-MpPreference).ExclusionPath; "
        "if ($e -and ($e -contains $d)) { 'YES' } else { 'NO' }")
        .arg(defenderExclusionDir());
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
