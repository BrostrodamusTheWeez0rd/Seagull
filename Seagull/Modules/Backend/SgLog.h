#pragma once
#include <QObject>
#include <QString>
#include <QMutex>
#include <QtGlobal>

class QFile;

// App-wide verbose logging sink. Off by default; toggled on by typing SEALOG into
// the Queue URL bar (and persisted to config, so a startup bug is captured from the
// very first line on the next launch). When enabled it mirrors every yt-dlp command
// + output line, the other backend workers' log lines, and all Qt debug/warning/
// critical messages into one text file the user can send in for diagnosis.
//
// Thread-safe: the yt-dlp comment worker and the tool updater run on their own
// threads, and Qt's installed message handler can fire from anywhere, so every write
// is mutex-guarded. Each line is flushed immediately, so a crash still leaves a
// complete log up to the failure.
class SgLog : public QObject {
    Q_OBJECT
public:
    static SgLog& instance();

    bool    isEnabled() const { return m_enabled; }
    QString filePath() const; // <appdir>/Config/seagull-log.txt

    // Turn logging on/off. Opens (append) or closes the file, writes a session
    // banner/footer, persists the state to config.ini, and installs the Qt message
    // handler on first enable. Idempotent.
    void setEnabled(bool on);

    // Read Logging/Verbose from config and enable if it was left on. Call once at
    // startup, before the workers spin up, so early activity is captured.
    void initFromConfig();

    // Write one line, prefixed with a millisecond timestamp and a category tag.
    // A cheap no-op when logging is off. Safe to call from any thread.
    void log(const QString& category, const QString& message);

private:
    explicit SgLog(QObject* parent = nullptr);
    ~SgLog();
    Q_DISABLE_COPY(SgLog)

    void openFile();
    void closeFile();
    void writeRaw(const QString& line); // locks internally

    static void messageHandler(QtMsgType type, const QMessageLogContext& ctx, const QString& msg);

    mutable QMutex m_mutex;
    QFile* m_file = nullptr;
    bool   m_enabled = false;

    static QtMessageHandler s_prevHandler;
    static bool             s_handlerInstalled;
};
