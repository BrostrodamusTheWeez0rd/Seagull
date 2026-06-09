#include "SgOptions.h"
#include <QCoreApplication>
#include <QSettings>
#include <QRegularExpression>

QStringList SgOptions::buildDownloadArgs(const QString& url) {
    QStringList args;

    QSettings settings(QCoreApplication::applicationDirPath() + "/config.ini",
        QSettings::IniFormat);

    QString type = settings.value("Download/Type", "Video").toString();
    QString format = settings.value("Download/Format", "Best Available").toString();
    QString quality = settings.value("Download/Quality", "Best Available").toString();
    QString dlFolder = settings.value("Paths/DownloadFolder",
        QCoreApplication::applicationDirPath() + "/Downloads").toString();

    args << "--newline"
        << "--no-playlist"
        << "-o" << dlFolder + "/%(title)s.%(ext)s";

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

    args << url;
    return args;
}

int SgOptions::defaultStreamHeight() {
    QSettings settings(QCoreApplication::applicationDirPath() + "/config.ini", QSettings::IniFormat);
    QString quality = settings.value("Streaming/Quality", "Best Available").toString();

    if (quality.isEmpty() || quality == "Best Available")
        return -1;

    QRegularExpression re("(\\d+)");
    QRegularExpressionMatch m = re.match(quality);
    return m.hasMatch() ? m.captured(1).toInt() : -1;
}
