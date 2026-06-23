#pragma once
#include <QObject>
#include <QString>
#include <QStringList>

class QNetworkAccessManager;
class QNetworkReply;

// Keeps the bundled tools current: yt-dlp, Deno, ffmpeg, AtomicParsley. Two-phase:
//
//   1. checkForUpdates() — resolves the latest version of each tool (GitHub
//      "latest" redirects + gyan.dev) and compares with the local exes. Emits
//      checkFinished() with a human-readable line per outdated/missing tool
//      (empty = everything current). Blocking network calls — runs on the
//      dedicated updater thread (see Seagull).
//
//   2. applyUpdates() — downloads and installs whatever the last check flagged,
//      SHA-256 verified before install, reporting applyProgress() per tool and
//      applyFinished() at the end. The UI decides whether/when to call this
//      (auto-update setting, or the user accepting the update prompt).
class SgUpdater : public QObject {
    Q_OBJECT
public:
    explicit SgUpdater(QObject* parent = nullptr);

    // Phase 1. ignoreCooldown skips the repeat-check suppression (the first-run
    // setup dialog needs an immediate answer).
    void checkForUpdates(bool ignoreCooldown = false);

    // Phase 2 — install everything the last checkForUpdates() found.
    void applyUpdates();

signals:
    void updateStatus(const QString& message);          // log lines (Queue console)
    void checkFinished(const QStringList& pending);     // "tool: old -> new" per tool
    void applyProgress(const QString& tool, int percent);
    void applyFinished(bool allOk);

private slots:
    void onDownloadProgress(qint64 received, qint64 total);
    void onExeDownloadFinished(QNetworkReply* reply);

private:
    void applyNext();        // pops m_applyQueue and starts that tool's download
    void downloadFfmpeg();   // streamed zip download + tar extract

    // Blocking helpers (fine on the updater thread).
    QString resolveLatestTag(const QString& latestReleaseUrl) const;
    QString fetchRemoteText(const QString& url) const;
    QString computeFileSha256(const QString& filePath) const;
    bool    verifyHash(const QString& filePath, const QString& expectedHash, const QString& label);

    QString localYtDlpVersion() const;
    QString localDenoVersion() const;
    QString localFfmpegVersion() const;
    QString localAtomicParsleyVersion() const;

    void downloadNewExe(const QString& exeUrl);
    void downloadNewDeno(const QString& zipUrl);
    void downloadNewAtomicParsley(const QString& zipUrl);
    void onAtomicParsleyDownloadFinished(QNetworkReply* reply);
    bool extractZip(const QString& zipPath, const QString& targetDir);

    QNetworkAccessManager* m_nam = nullptr;
    QStringList m_applyQueue;   // subset of {"yt-dlp","deno","ffmpeg","atomicparsley"}, from the check
    bool m_applyOk = true;
};
