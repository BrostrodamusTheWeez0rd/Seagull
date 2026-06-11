#include "SgPaths.h"

#include <QCoreApplication>
#include <QSettings>

namespace {

// Per-type folder lookup with a two-stage fallback: configs from before the
// per-type folders keep saving into their old Paths/DownloadFolder; genuinely
// fresh installs get tidy Downloads/<sub> folders.
QString typedFolder(const char* key, const QString& sub) {
    QSettings cfg(QCoreApplication::applicationDirPath() + "/config.ini", QSettings::IniFormat);
    if (cfg.contains(key))
        return cfg.value(key).toString();
    if (cfg.contains("Paths/DownloadFolder"))
        return cfg.value("Paths/DownloadFolder").toString();
    return QCoreApplication::applicationDirPath() + "/Downloads/" + sub;
}

} // namespace

QString SgPaths::homeFolder() {
    QSettings cfg(QCoreApplication::applicationDirPath() + "/config.ini", QSettings::IniFormat);
    return cfg.value("Paths/HomeFolder", QCoreApplication::applicationDirPath()).toString();
}

QString SgPaths::videoFolder()     { return typedFolder("Paths/VideoFolder", "Videos"); }
QString SgPaths::audioFolder()     { return typedFolder("Paths/AudioFolder", "Audio"); }
QString SgPaths::photoFolder()     { return typedFolder("Paths/PhotoFolder", "Photos"); }
QString SgPaths::recordingFolder() { return typedFolder("Paths/RecordingFolder", "Recordings"); }
