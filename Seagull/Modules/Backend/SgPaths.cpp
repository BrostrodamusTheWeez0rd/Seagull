#include "SgPaths.h"

#include <QCoreApplication>
#include <QDir>
#include <QSettings>
#include <QStandardPaths>

QString SgPaths::configDir() {
    return QCoreApplication::applicationDirPath() + "/Config";
}

QString SgPaths::configFile() {
    return configDir() + "/config.ini";
}

namespace {

// Per-type folder lookup with a staged fallback: an explicitly configured
// folder wins; configs from before the per-type folders keep saving into
// their old Paths/DownloadFolder; genuinely fresh installs default to the
// user's matching Windows folder (Videos / Music / Pictures).
QString typedFolder(const char* key, QStandardPaths::StandardLocation loc,
                    const QString& sub = QString()) {
    QSettings cfg(SgPaths::configFile(), QSettings::IniFormat);
    if (cfg.contains(key))
        return cfg.value(key).toString();
    if (cfg.contains("Paths/DownloadFolder"))
        return cfg.value("Paths/DownloadFolder").toString();
    const QString base = QStandardPaths::writableLocation(loc);
    return sub.isEmpty() ? base : base + "/" + sub;
}

} // namespace

QString SgPaths::homeFolder() {
    QSettings cfg(SgPaths::configFile(), QSettings::IniFormat);
    return cfg.value("Paths/HomeFolder",
        QStandardPaths::writableLocation(QStandardPaths::HomeLocation)).toString();
}

QString SgPaths::downloadFolder() {
    // Paths/DownloadFolder doubles as the pre-per-type-folders legacy key, so
    // configs from before this setting keep downloading where they always did.
    QSettings cfg(SgPaths::configFile(), QSettings::IniFormat);
    return cfg.value("Paths/DownloadFolder",
        QStandardPaths::writableLocation(QStandardPaths::DownloadLocation)).toString();
}

bool SgPaths::smartSortDownloads() {
    QSettings cfg(SgPaths::configFile(), QSettings::IniFormat);
    return cfg.value("Paths/SmartSort", true).toBool(); // on by default
}

bool SgPaths::unifyMedia() {
    QSettings cfg(SgPaths::configFile(), QSettings::IniFormat);
    return cfg.value("Paths/UnifyMedia", false).toBool();
}

QString SgPaths::unifiedFolder() {
    return typedFolder("Paths/UnifiedFolder", QStandardPaths::MoviesLocation);
}

QString SgPaths::videoFolder(bool honourUnify) {
    if (honourUnify && unifyMedia()) return unifiedFolder();
    return typedFolder("Paths/VideoFolder", QStandardPaths::MoviesLocation);
}

QString SgPaths::audioFolder(bool honourUnify) {
    if (honourUnify && unifyMedia()) return unifiedFolder();
    return typedFolder("Paths/AudioFolder", QStandardPaths::MusicLocation);
}

QString SgPaths::photoFolder(bool honourUnify) {
    if (honourUnify && unifyMedia()) return unifiedFolder();
    return typedFolder("Paths/PhotoFolder", QStandardPaths::PicturesLocation);
}

QString SgPaths::recordingFolder(bool honourUnify) {
    if (honourUnify && unifyMedia()) return unifiedFolder();
    // No Windows standard folder for recordings — a subfolder of Videos.
    return typedFolder("Paths/RecordingFolder", QStandardPaths::MoviesLocation, "Recordings");
}

QString SgPaths::playlistFolder(bool honourUnify) {
    if (honourUnify && unifyMedia()) return unifiedFolder();
    // Deliberately NOT typedFolder(): its legacy Paths/DownloadFolder hop would
    // scatter app-created .sgpl files into the Downloads folder on older configs.
    QSettings cfg(SgPaths::configFile(), QSettings::IniFormat);
    QString folder = cfg.contains("Paths/PlaylistFolder")
        ? cfg.value("Paths/PlaylistFolder").toString()
        : QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) + "/Playlists";
    // Unlike the media folders (Windows-provided), Documents\Playlists doesn't
    // exist until someone makes it — and configs that predate this feature never
    // ran the setup step that would. Guarantee it here; no-op when present.
    QDir().mkpath(folder);
    return folder;
}
