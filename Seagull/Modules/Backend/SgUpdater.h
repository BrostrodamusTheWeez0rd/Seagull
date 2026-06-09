#pragma once
#include <QObject>
#include <QString>

class QNetworkAccessManager;
class QNetworkReply;

// Keeps the bundled tools current: yt-dlp -> Deno -> ffmpeg, chained. Version
// checks via GitHub "latest" redirects + gyan.dev; downloads are SHA-256 verified
// before install. Owns its own network manager and runs on a dedicated thread
// (see Seagull). Independent of the yt-dlp jobs in SgYtDlp.
class SgUpdater : public QObject {
    Q_OBJECT
public:
    explicit SgUpdater(QObject* parent = nullptr);

    // Entry point — call once at startup. Chains yt-dlp -> Deno -> ffmpeg.
    void checkForUpdates();

signals:
    void updateStatus(const QString& message);

private slots:
    void onReleaseInfoReceived(QNetworkReply* reply);
    void onDownloadProgress(qint64 received, qint64 total);
    void onExeDownloadFinished(QNetworkReply* reply);

private:
    void checkForDenoUpdate();
    void checkForFfmpegUpdate();
    void resolveLatestVersion(const QString& latestReleaseUrl, const QString& kind);

    QString computeFileSha256(const QString& filePath) const;
    QString fetchRemoteText(const QString& url) const;
    bool    verifyHash(const QString& filePath, const QString& expectedHash, const QString& label);

    QString localYtDlpVersion() const;
    QString localDenoVersion() const;
    QString localFfmpegVersion() const;

    void downloadNewExe(const QString& exeUrl);
    void downloadNewDeno(const QString& zipUrl);
    bool extractDenoZip(const QString& zipPath, const QString& targetDir);

    QNetworkAccessManager* m_nam = nullptr;
};
