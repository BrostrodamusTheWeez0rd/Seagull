#include "SgSearch.h"
#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonParseError>

SgSearch::SgSearch(QObject* parent) : QObject(parent) {
    m_process = new QProcess(this);
    m_process->setProcessChannelMode(QProcess::MergedChannels);
    connect(m_process, &QProcess::finished, this, &SgSearch::handleFinished);
    connect(m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart)
            emit failed("Could not find yt-dlp.exe.");
        });
}

SgSearch::~SgSearch() {
    cancel();
}

void SgSearch::search(Site site, const QString& query, int limit) {
    if (query.trimmed().isEmpty()) { emit failed("Empty search query."); return; }

    cancel(); // drop any in-flight query (a new search supersedes it; no 'failed' emitted)

    m_site = site;
    m_buffer.clear();

    const QString exePath = QCoreApplication::applicationDirPath() + "/tools/yt-dlp.exe";

    QStringList args;
    args << "-J" << "--flat-playlist" << "--quiet" << "--no-warnings";

    // Per-site search target. New sites slot in here with their own branch.
    switch (site) {
    case Site::YouTube:
        args << QString("ytsearch%1:%2").arg(limit).arg(query.trimmed());
        break;
    }

    emit logMessage("Searching: " + query.trimmed());
    m_process->start(exePath, args);
}

void SgSearch::cancel() {
    if (m_process->state() == QProcess::Running) {
        m_cancelled = true; // tells handleFinished to ignore this killed run
        m_process->kill();
        m_process->waitForFinished();
    }
    m_buffer.clear();
}

void SgSearch::handleFinished(int exitCode, QProcess::ExitStatus exitStatus) {
    if (m_cancelled) { m_cancelled = false; return; } // superseded by a newer query

    m_buffer.append(m_process->readAllStandardOutput());

    if (exitStatus == QProcess::CrashExit || exitCode != 0) {
        const QString err = QString::fromLocal8Bit(m_buffer).trimmed();
        emit failed(err.isEmpty() ? "Search failed." : ("Search failed: " + err.right(400)));
        return;
    }

    const int jsonStart = m_buffer.indexOf('{');
    if (jsonStart == -1) { emit failed("No results."); return; }

    QJsonParseError perr;
    QJsonDocument doc = QJsonDocument::fromJson(m_buffer.mid(jsonStart), &perr);
    if (perr.error != QJsonParseError::NoError || doc.isNull()) {
        emit failed("Could not parse search results.");
        return;
    }

    QList<SearchResult> results;
    switch (m_site) {
    case Site::YouTube:
        results = parseYoutube(doc.object());
        break;
    }
    emit resultsReady(results);
}

QList<SearchResult> SgSearch::parseYoutube(const QJsonObject& root) const {
    QList<SearchResult> out;
    const QJsonArray entries = root["entries"].toArray();

    for (const auto& it : entries) {
        const QJsonObject e = it.toObject();
        // Flat search entries put the watch URL in "url" (webpage_url is empty).
        const QString url = e["url"].toString();
        const QString title = e["title"].toString();
        if (url.isEmpty() || title.isEmpty()) continue;

        SearchResult r;
        r.title = title;
        r.url = url;
        r.channel = e["channel"].toString();
        if (r.channel.isEmpty()) r.channel = e["uploader"].toString();

        // YouTube's thumbnails[] urls are .jpg-named but actually serve WebP (the
        // signed sqp/rs variants), which QPixmap can't decode without the WebP
        // plugin. The canonical /vi/<id>/mqdefault.jpg is a real 16:9 JPEG that
        // always exists and loads fine, so build that from the video id instead.
        const QString id = e["id"].toString();
        if (!id.isEmpty())
            r.thumbnail = QString("https://i.ytimg.com/vi/%1/mqdefault.jpg").arg(id);
        else {
            int bestW = -1;
            for (const auto& t : e["thumbnails"].toArray()) {
                const QJsonObject to = t.toObject();
                const int w = to["width"].toInt();
                if (w > bestW) { bestW = w; r.thumbnail = to["url"].toString(); }
            }
        }

        r.duration = e.contains("duration") && !e["duration"].isNull()
            ? static_cast<qint64>(e["duration"].toDouble()) : -1;
        r.viewCount = e.contains("view_count") && !e["view_count"].isNull()
            ? static_cast<qint64>(e["view_count"].toDouble()) : -1;

        out.append(r);
    }
    return out;
}
