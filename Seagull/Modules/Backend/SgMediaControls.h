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
    // card (instead of "unknown app"). Best-effort; failures are silent.
    static void createDesktopShortcut();
    static void createStartMenuShortcut();

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
