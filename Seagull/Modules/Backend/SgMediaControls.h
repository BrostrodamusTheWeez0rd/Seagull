#ifndef SGMEDIACONTROLS_H
#define SGMEDIACONTROLS_H

#include <QObject>
#include <QString>
#include <QImage>

// Bridges playback to the Windows System Media Transport Controls (SMTC): the OS
// "now playing" widget (the media flyout above the volume keys, the lock screen)
// and the hardware + keyboard media keys (Play/Pause, Next, Previous). It owns no
// playback. It mirrors the player's state out to Windows and turns OS button
// presses into Qt signals the orchestrator wires back to the player.
//
// All WinRT/C++/WinRT lives in the .cpp behind a pimpl so the rest of the app
// never sees a winrt:: type. Inert (every call a no-op) if SMTC can't be bound
// — e.g. an OS that doesn't support it — so callers needn't special-case it.
class SgMediaControls : public QObject {
    Q_OBJECT
public:
    enum class Status { Closed, Playing, Paused, Stopped };

    explicit SgMediaControls(QObject* parent = nullptr);
    ~SgMediaControls() override;

    // Give the process an explicit AppUserModelID. Windows keys the global SMTC
    // session to this id; without it the now-playing card shows "unknown app" and
    // refuses to display our metadata. Call once, as early in main() as possible
    // (before any window or the SMTC session is created).
    static void registerAppIdentity();

    // Create .lnk shortcuts to this exe. The Start-menu one carries the same
    // AppUserModelID, which is what lets Windows resolve our name/icon on the SMTC
    // card (instead of "unknown app"). Best-effort; return true only if the .lnk
    // was written (Settings uses this to confirm the action to the user).
    static bool createDesktopShortcut();
    static bool createStartMenuShortcut();

    // Delete the .lnk written by the create* calls above, so Settings can offer a
    // symmetric Add/Remove toggle. Returns true if the file is gone afterwards
    // (including the already-absent case). Best-effort.
    static bool removeDesktopShortcut();
    static bool removeStartMenuShortcut();

    // Whether the corresponding .lnk currently exists. Cheap file check (no COM),
    // so it's safe to call on show to label the Settings buttons Add vs Remove.
    static bool desktopShortcutExists();
    static bool startMenuShortcutExists();

    // Outcome of an attempt to change Defender's exclusion list. The elevated step
    // re-reads the list after the change and only reports Success if it actually
    // stuck, so Blocked is distinguishable from a silent no-op (the usual cause is
    // Tamper Protection, which refuses programmatic exclusion edits even from admin).
    enum class DefenderResult {
        Success,   // the change was applied and verified
        Declined,  // the UAC prompt was dismissed / elevation never launched
        Blocked,   // ran but the change didn't take (Tamper Protection most likely)
        Error      // the cmdlet threw (Defender unavailable / access denied)
    };

    // Add this app's install folder to the Windows Defender exclusion list. Defender
    // rescans Seagull's many DLLs and VLC plugins on a cold launch (after a reboot or
    // long idle), which is the main cause of the slow-then-fast first start; excluding
    // the folder skips that rescan. Adding an exclusion needs admin, so this raises a
    // UAC prompt. The elevated step verifies the exclusion is really present before
    // returning, so the result reflects the persisted state, not just "the command ran".
    static DefenderResult addDefenderExclusion();

    // Remove this app's install folder from the Defender exclusion list (the inverse
    // of addDefenderExclusion). Also needs admin -> UAC prompt; verifies the removal.
    static DefenderResult removeDefenderExclusion();

    // User-facing guidance for a result, or an empty string for Success/Declined
    // (nothing worth interrupting the user over). Blocked/Error explain the likely
    // Tamper Protection cause and point at the Windows Security UI. No trademarks
    // beyond the Defender/Windows Security names the user needs to find the setting.
    static QString defenderResultMessage(DefenderResult result);

    // Open the Windows Security "Virus & threat protection settings" page, where the
    // user can toggle Tamper Protection and manage exclusions by hand. Best-effort.
    static void openDefenderSettings();

    // The PowerShell one-liner that prints "YES" / "NO" depending on whether this
    // app's install folder is in Defender's exclusion list. Reading needs no
    // elevation; callers run it (e.g. via QProcess) off the GUI thread since
    // Get-MpPreference can take a second or two. Pass as the argument to -Command.
    static QString defenderExclusionQueryCommand();

    bool isAvailable() const;        // SMTC bound OK?
    void attachToWindow(void* hwnd); // bind to the app's top-level HWND (once)

public slots:
    void setEnabled(bool on);                          // media present -> controls live
    void setPlaybackStatus(Status status);
    void setMetadata(const QString& title, const QString& artist, bool isVideo);
    void setThumbnail(const QImage& image);            // cover art / video poster
    void setTimeline(qint64 positionMs, qint64 durationMs); // overlay scrubber
    void clear();                                      // no media — blank the widget

signals:
    void playPressed();
    void pausePressed();
    void nextPressed();
    void previousPressed();
    void stopPressed();

private:
    void applyDisplay(); // (re)push the cached metadata into the DisplayUpdater

    struct Impl;
    Impl* d;
};

#endif // SGMEDIACONTROLS_H
