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

    // Smart sort on (default): route by type into the matching media folder (audio
    // -> Audio, video -> Video; each honours the unify setting, yt-dlp creates the
    // dir if needed). Off: everything lands in the single Downloads folder.
    const QString outDir = SgPaths::smartSortDownloads()
        ? ((type == "Audio") ? SgPaths::audioFolder() : SgPaths::videoFolder())
        : SgPaths::downloadFolder();
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

        // Embed the thumbnail as album cover art and write title/artist/etc. tags.
        // yt-dlp uses ffmpeg for MP3/Opus/FLAC art and AtomicParsley (shipped in
        // tools/) for M4A/MP4. Convert to jpg first: source thumbnails are usually
        // webp (YouTube), which won't embed cleanly into ID3/MP4 cover atoms.
        // SgYtDlp::download runs yt-dlp with tools/ as its working dir so the
        // bare-named AtomicParsley.exe is found.
        args << "--embed-thumbnail"
             << "--embed-metadata"
             << "--convert-thumbnails" << "jpg";
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
    // Firefox is the only supported source: yt-dlp reads its cookie store reliably, while
    // Chromium browsers encrypt theirs in a way it can't read on current Windows builds.
    // Anything else (None, or a stale Chrome/Edge/Brave value) means no cookies.
    if (choice.compare("Firefox", Qt::CaseInsensitive) != 0) return {};
    return { "--cookies-from-browser", "firefox" };
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
