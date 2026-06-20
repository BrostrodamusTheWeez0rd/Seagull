#include "SgOptions.h"
#include "SgPaths.h"
#include <QCoreApplication>
#include <QSettings>
#include <QRegularExpression>

QStringList SgOptions::buildDownloadArgs(const QString& url) {
    QStringList args;

    QSettings settings(SgPaths::configFile(), QSettings::IniFormat);

    QString type = settings.value("Download/Type", "Video").toString();
    QString format = settings.value("Download/Format", "mp4").toString();
    QString quality = settings.value("Download/Quality", "Best Available").toString();

    // Route by type into the matching media folder: audio -> Audio folder, video
    // -> Video folder (each honours the unify setting; yt-dlp creates the dir if
    // needed). These fall back to the legacy Downloads folder when no per-type
    // folder is set, so existing setups keep working.
    const QString outDir = (type == "Audio") ? SgPaths::audioFolder()
                                             : SgPaths::videoFolder();
    args << "--newline"
        << "--no-playlist"
        << "-o" << outDir + "/%(title)s.%(ext)s";

    if (type == "Audio") {
        // Extract audio. "Best Available" keeps the source audio codec; a named
        // format re-encodes to it. Bitrate maps to yt-dlp's --audio-quality.
        args << "-x";
        if (format != "Best Available")
            args << "--audio-format" << format;
        if (quality.contains("kbps"))
            args << "--audio-quality" << quality.split(" ").first() + "K";
    }
    else {
        if (quality == "Best Available") {
            args << "-f" << "bestvideo+bestaudio/best";
        }
        else {
            QString height = QRegularExpression("(\\d+)").match(quality).captured(1);
            args << "-f" << QString("bestvideo[height<=%1]+bestaudio/best").arg(height);
        }

        if (format != "Best Available")
            args << "--merge-output-format" << format;
    }

    args += cookieArgs(); // browser cookies before the URL, if the user opted in
    args << url;
    return args;
}

QStringList SgOptions::cookieArgs() {
    QSettings settings(SgPaths::configFile(), QSettings::IniFormat);
    const QString choice = settings.value("Streaming/CookiesBrowser", "None").toString();
    if (choice.isEmpty() || choice == "None") return {};
    // The combo labels map straight to yt-dlp's browser keywords once lowercased
    // (Firefox -> firefox, Chrome -> chrome, Edge -> edge, Brave -> brave).
    return { "--cookies-from-browser", choice.toLower() };
}

int SgOptions::defaultStreamHeight() {
    QSettings settings(SgPaths::configFile(), QSettings::IniFormat);
    QString quality = settings.value("Streaming/Quality", "Best Available").toString();

    if (quality.isEmpty() || quality == "Best Available")
        return -1;

    QRegularExpression re("(\\d+)");
    QRegularExpressionMatch m = re.match(quality);
    return m.hasMatch() ? m.captured(1).toInt() : -1;
}
