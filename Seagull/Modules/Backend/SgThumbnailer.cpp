#include "SgThumbnailer.h"

#include <QProcess>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QImageReader>

namespace {
constexpr int kThumbWidth = 480; // matches the card's reference render width

QString thumbsDir() {
    return QCoreApplication::applicationDirPath() + "/cache/thumbs";
}

QString ffmpegPath() {
    return QCoreApplication::applicationDirPath() + "/tools/ffmpeg.exe";
}

// Load capped at the card's reference width — QImageReader scales while
// decoding, so a 20MP photo never fully decompresses.
QPixmap loadScaled(const QString& path) {
    QImageReader reader(path);
    reader.setAutoTransform(true);
    QSize sz = reader.size();
    if (sz.isValid() && sz.width() > kThumbWidth)
        reader.setScaledSize(QSize(kThumbWidth, sz.height() * kThumbWidth / sz.width()));
    return QPixmap::fromImage(reader.read());
}
}

SgThumbnailer::SgThumbnailer(QObject* parent) : QObject(parent) {
    m_proc = new QProcess(this);
    connect(m_proc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
        this, &SgThumbnailer::onProcessFinished);
}

bool SgThumbnailer::isImageFile(const QString& path) {
    static const QStringList exts = { "jpg", "jpeg", "png", "gif", "webp", "bmp" };
    return exts.contains(QFileInfo(path).suffix().toLower());
}

bool SgThumbnailer::isAudioFile(const QString& path) {
    static const QStringList exts = { "mp3", "m4a", "opus", "wav", "flac" };
    return exts.contains(QFileInfo(path).suffix().toLower());
}

QString SgThumbnailer::cachePathFor(const QString& filePath) const {
    // Keyed on path + size + mtime so an edited/replaced file regenerates.
    const QFileInfo fi(filePath);
    const QByteArray key = (filePath + "|" + QString::number(fi.size()) + "|"
        + fi.lastModified().toString(Qt::ISODate)).toUtf8();
    const QString hash = QCryptographicHash::hash(key, QCryptographicHash::Sha1).toHex();
    return thumbsDir() + "/" + hash + ".jpg";
}

void SgThumbnailer::requestThumbnail(const QString& filePath) {
    if (isImageFile(filePath)) {
        const QPixmap pm = loadScaled(filePath);
        if (!pm.isNull()) emit thumbnailReady(filePath, pm);
        return;
    }

    const QString cached = cachePathFor(filePath);
    if (QFileInfo::exists(cached)) {
        const QPixmap pm(cached);
        if (!pm.isNull()) { emit thumbnailReady(filePath, pm); return; }
    }

    if (m_current == filePath || m_queue.contains(filePath)) return;
    m_queue.append(filePath);
    pump();
}

void SgThumbnailer::cancelPending() {
    m_queue.clear();
}

bool SgThumbnailer::isBusy() const {
    return !m_current.isEmpty() || !m_queue.isEmpty();
}

void SgThumbnailer::pump() {
    if (m_proc->state() != QProcess::NotRunning || m_queue.isEmpty()) return;

    QDir().mkpath(thumbsDir());
    m_current = m_queue.takeFirst();
    m_currentOut = cachePathFor(m_current);
    m_retried = false;

    QStringList args{ "-hide_banner", "-loglevel", "error", "-nostdin", "-y" };
    if (isAudioFile(m_current)) {
        // The embedded cover art is an attached-picture *video* stream, so
        // dropping audio and taking one video frame extracts exactly it.
        args << "-i" << m_current << "-an" << "-frames:v" << "1";
    } else {
        // Skip the first seconds of video — frame 0 is usually a black fade-in.
        args << "-ss" << "3" << "-i" << m_current << "-frames:v" << "1";
    }
    args << "-vf" << QString("scale=%1:-2").arg(kThumbWidth) << "-q:v" << "4" << m_currentOut;

    m_proc->start(ffmpegPath(), args);
}

void SgThumbnailer::onProcessFinished() {
    const QFileInfo out(m_currentOut);
    if (out.exists() && out.size() > 0) {
        const QPixmap pm(m_currentOut);
        if (!pm.isNull()) emit thumbnailReady(m_current, pm);
    } else if (!m_retried && !isAudioFile(m_current)) {
        // Shorter than 3s (clips, fresh recordings): grab the first frame instead.
        m_retried = true;
        QStringList args{ "-hide_banner", "-loglevel", "error", "-nostdin", "-y",
            "-i", m_current, "-frames:v", "1",
            "-vf", QString("scale=%1:-2").arg(kThumbWidth), "-q:v", "4", m_currentOut };
        m_proc->start(ffmpegPath(), args);
        return; // same m_current; finish handler runs again
    }

    m_current.clear();
    m_currentOut.clear();
    pump();
}
