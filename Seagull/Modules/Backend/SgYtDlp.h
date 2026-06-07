#pragma once
#include <QObject>
#include <QProcess>
#include <QString>
#include <QStringList>
#include <QByteArray>
#include <QUrl>
#include <QIODevice>
#include <QVariantMap>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QFile>

struct StreamOption {
    QString formatId;
    QString label;
    bool isAudioOnly;
};

class FfmpegStreamDevice : public QIODevice {
    Q_OBJECT
public:
    explicit FfmpegStreamDevice(QProcess* process, QObject* parent = nullptr)
        : QIODevice(parent), m_process(process) {
    }
    bool isSequential() const override { return true; }
    bool open(OpenMode mode) override {
        if (!(mode & QIODevice::ReadOnly)) return false;
        return QIODevice::open(mode);
    }
    qint64 bytesAvailable() const override {
        return m_process->bytesAvailable() + QIODevice::bytesAvailable();
    }
protected:
    qint64 readData(char* data, qint64 maxSize) override {
        return m_process->read(data, maxSize);
    }
    qint64 writeData(const char*, qint64) override { return -1; }
private:
    QProcess* m_process;
};

class SgYtDlp : public QObject {
    Q_OBJECT
public:
    explicit SgYtDlp(QObject* parent = nullptr);
    ~SgYtDlp();

    void download(const QString& url);
    void fetchMetadataAndStreamUrl(const QString& url, const QString& formatId = QString());
    void probeAvailableQualities(const QString& url);
    void fetchPlaylistEntries(const QString& playlistUrl);
    void cancel();

    // Call once at startup — chains yt-dlp -> Deno -> ffmpeg
    void checkForYtDlpUpdate();
    void checkForDenoUpdate();
    void checkForFfmpegUpdate();

signals:
    void logMessage(const QString& message);
    void progressUpdated(double percentage);
    void finished(bool success);
    void metadataReady(const QString& title, const QString& uploader, const QString& duration,
        const QString& viewCount, const QString& uploadDate, const QString& thumbUrl);
    void availableQualitiesFound(const QList<StreamOption>& options);
    void streamUrlReady(const QUrl& videoUrl, const QUrl& audioUrl = QUrl());
    void playlistEntriesReady(const QList<QString>& urls);
    void ytDlpUpdateStatus(const QString& message);

private slots:
    void handleReadyRead();
    void handleProcessFinished(int exitCode, QProcess::ExitStatus exitStatus);
    void onReleaseInfoReceived(QNetworkReply* reply);
    void onDownloadProgress(qint64 received, qint64 total);
    void onExeDownloadFinished(QNetworkReply* reply);

private:
    QProcess* m_process;
    QNetworkAccessManager* m_nam = nullptr;

    enum class JobMode { Idle, Downloading, FetchingMetadata, Probing, FetchingPlaylist };
    JobMode currentMode = JobMode::Idle;

    QByteArray processBuffer;
    QStringList buildDownloadArgs(const QString& url);

    void resolveLatestVersion(const QString& latestReleaseUrl, const QString& kind);
    QString computeFileSha256(const QString& filePath) const;
    QString fetchRemoteText(const QString& url) const;
    bool verifyHash(const QString& filePath, const QString& expectedHash, const QString& label);
    QString localYtDlpVersion() const;
    QString localDenoVersion() const;
    QString localFfmpegVersion() const;
    void downloadNewExe(const QString& exeUrl);
    void downloadNewDeno(const QString& zipUrl);
    bool extractDenoZip(const QString& zipPath, const QString& targetDir);
};