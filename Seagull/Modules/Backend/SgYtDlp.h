#pragma once
#include <QObject>
#include <QProcess>
#include <QString>
#include <QStringList>
#include <QByteArray>
#include <QUrl>
#include <QIODevice>

// A thin QIODevice wrapper around a QProcess stdout stream.
// QMediaPlayer::setSourceDevice() needs a QIODevice — this bridges the gap.
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

// ─────────────────────────────────────────────
class SgYtDlp : public QObject {
    Q_OBJECT
public:
    explicit SgYtDlp(QObject* parent = nullptr);
    ~SgYtDlp();

    void download(const QString& url);
    void fetchMetadataAndStreamUrl(const QString& url);
    void cancel();

signals:
    void logMessage(const QString& message);
    void progressUpdated(double percentage);
    void finished(bool success);
    void metadataReady(const QString& title, const QString& uploader, const QString& duration,
        const QString& viewCount, const QString& uploadDate, const QString& thumbUrl);

    // Emits the live QIODevice* for streaming — caller passes to QMediaPlayer::setSourceDevice()
    // mimeType will be "video/mp2t"
    void streamDeviceReady(QIODevice* device, const QString& mimeType);

    // Fallback for single combined URLs (no proxy needed)
    void streamUrlReady(const QUrl& url);

private slots:
    void handleReadyRead();
    void handleProcessFinished(int exitCode, QProcess::ExitStatus exitStatus);

private:
    QProcess* m_process;
    QProcess* m_proxyProcess = nullptr;
    FfmpegStreamDevice* m_streamDevice = nullptr;

    void startProxy(const QString& videoUrl, const QString& audioUrl);
    void stopProxy();

    enum class JobMode { Idle, Downloading, FetchingMetadata };
    JobMode currentMode = JobMode::Idle;

    QByteArray processBuffer;

    QStringList buildDownloadArgs(const QString& url);
};