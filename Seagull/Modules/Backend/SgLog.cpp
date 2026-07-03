#include "SgLog.h"
#include "SgPaths.h"
#include <QFile>
#include <QDir>
#include <QSettings>
#include <QDateTime>
#include <QSysInfo>
#include <QMutexLocker>

// Stamped in by the build (see CMakeLists, target-wide). Fallback keeps a stray
// build compiling.
#ifndef SEAGULL_VERSION
#define SEAGULL_VERSION "dev"
#endif

QtMessageHandler SgLog::s_prevHandler = nullptr;
bool             SgLog::s_handlerInstalled = false;

SgLog& SgLog::instance() {
    static SgLog inst;
    return inst;
}

SgLog::SgLog(QObject* parent) : QObject(parent) {}

SgLog::~SgLog() {
    closeFile();
    QMutexLocker lock(&m_mutex);
    delete m_file;
    m_file = nullptr;
}

QString SgLog::filePath() const {
    return SgPaths::configDir() + "/seagull-log.txt";
}

void SgLog::openFile() {
    QMutexLocker lock(&m_mutex);
    if (m_file && m_file->isOpen()) return;
    QDir().mkpath(SgPaths::configDir());
    if (!m_file) m_file = new QFile(filePath());
    // Keep the file sendable: once it grows past a few MB, roll it to .old so a
    // long-running session (or logging left on for weeks) never bloats unbounded.
    if (m_file->exists() && m_file->size() > 5 * 1024 * 1024) {
        QFile::remove(filePath() + ".old");
        QFile::rename(filePath(), filePath() + ".old");
    }
    m_file->open(QIODevice::Append | QIODevice::Text);
}

void SgLog::closeFile() {
    QMutexLocker lock(&m_mutex);
    if (m_file && m_file->isOpen()) {
        m_file->flush();
        m_file->close();
    }
}

void SgLog::writeRaw(const QString& line) {
    QMutexLocker lock(&m_mutex);
    if (!m_file || !m_file->isOpen()) return;
    m_file->write(line.toUtf8());
    m_file->write("\n");
    m_file->flush(); // per-line flush so a crash still leaves a complete log
}

void SgLog::log(const QString& category, const QString& message) {
    if (!m_enabled) return; // fast path when logging is off
    writeRaw(QStringLiteral("[%1] [%2] %3").arg(
        QDateTime::currentDateTime().toString("HH:mm:ss.zzz"), category, message));
}

void SgLog::setEnabled(bool on) {
    if (on == m_enabled) return;

    if (on) {
        openFile();
        m_enabled = true;
        // Install the Qt message handler once, on first enable (chained to the
        // previous one so console/VS output still shows everything).
        if (!s_handlerInstalled) {
            s_prevHandler = qInstallMessageHandler(&SgLog::messageHandler);
            s_handlerInstalled = true;
        }
        writeRaw(QString());
        writeRaw(QStringLiteral("========================= SESSION START ========================="));
        writeRaw(QStringLiteral("Seagull %1  |  %2  |  %3")
            .arg(QString::fromLatin1(SEAGULL_VERSION),
                 QSysInfo::prettyProductName(),
                 QDateTime::currentDateTime().toString(Qt::ISODate)));
        writeRaw(QStringLiteral("================================================================="));
    } else {
        writeRaw(QStringLiteral("========================== SESSION END =========================="));
        m_enabled = false;
        closeFile();
    }

    QSettings s(SgPaths::configFile(), QSettings::IniFormat);
    s.setValue("Logging/Verbose", on);
}

void SgLog::initFromConfig() {
    QSettings s(SgPaths::configFile(), QSettings::IniFormat);
    if (s.value("Logging/Verbose", false).toBool())
        setEnabled(true);
}

void SgLog::messageHandler(QtMsgType type, const QMessageLogContext& ctx, const QString& msg) {
    const char* lvl = "debug";
    switch (type) {
    case QtDebugMsg:    lvl = "debug";    break;
    case QtInfoMsg:     lvl = "info";     break;
    case QtWarningMsg:  lvl = "warning";  break;
    case QtCriticalMsg: lvl = "critical"; break;
    case QtFatalMsg:    lvl = "fatal";    break;
    }
    SgLog::instance().log(QStringLiteral("qt/") + QLatin1String(lvl), msg);
    if (s_prevHandler) s_prevHandler(type, ctx, msg); // keep default stderr/VS output
}
