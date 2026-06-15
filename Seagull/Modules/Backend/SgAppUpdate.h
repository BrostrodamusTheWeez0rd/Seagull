#pragma once
#include <QObject>
#include <QString>

class QNetworkAccessManager;

// Checks GitHub Releases for a newer Seagull build. Notify-only for now: it
// reports whether a newer release exists, its version, the release notes, and
// the release page URL. The UI shows a prompt and opens the page in the browser
// (this is the foundation step; full download-and-swap can layer on later).
//
// Beta channel: looks at the full /releases list (newest first) so pre-releases
// count. A future stable channel would use /releases/latest instead.
class SgAppUpdate : public QObject {
    Q_OBJECT
public:
    explicit SgAppUpdate(QObject* parent = nullptr);

    void checkForUpdate(); // async; emits exactly one of updateAvailable/upToDate/checkFailed

    // Download the newest release's zip asset, extract + verify it into a staging
    // folder, then emit readyToApply with that folder (the orchestrator hands it to
    // the swap helper). Reports progress; emits downloadFailed on any problem.
    void downloadAndApply();

    // SemVer-aware "is remote strictly newer than local". Handles a leading "v"
    // and pre-release precedence (0.9.0-beta < 0.9.0). Static for easy testing.
    static bool isNewer(const QString& remote, const QString& local);

signals:
    void updateAvailable(const QString& version, const QString& notes, const QString& pageUrl);
    void upToDate();
    void checkFailed(const QString& reason);

    void downloadProgress(qint64 received, qint64 total);
    void downloadFailed(const QString& reason);
    void readyToApply(const QString& stagedAppDir); // verified build, ready for the swap

private:
    QNetworkAccessManager* m_nam = nullptr;
    QString m_assetUrl; // newest release's downloadable .zip (set during checkForUpdate)
};
