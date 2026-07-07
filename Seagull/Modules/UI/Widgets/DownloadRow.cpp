#include "DownloadRow.h"
#include "MarqueeLabel.h"
#include "../../Backend/SgThumbnailer.h" // decodeViaFfmpeg (WebP thumbnails)

#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QPixmap>
#include <QPixmapCache>
#include <QPalette>
#include <QStringList>
#include <QFileInfo>
#include <QDateTime>
#include <QLocale>

namespace {
constexpr int kThumbW = 96;
constexpr int kThumbH = 54; // 16:9
constexpr int kRowH   = 70; // fixed row height so entries stay compact + uniform

// Semantic status colours (fixed — green "done" / red "failed" read the same in every theme;
// the neutral states fall back to the themed dim/accent text via object names).
const char* kGreen = "#4caf50";
const char* kRed   = "#ff6b6b";

QString statusText(int s) {
    switch (s) {
    case SgDownloadHistory::Queued:      return QStringLiteral("Queued");
    case SgDownloadHistory::Downloading: return QStringLiteral("Downloading");
    case SgDownloadHistory::Completed:   return QStringLiteral("Completed");
    case SgDownloadHistory::Failed:      return QStringLiteral("Failed");
    case SgDownloadHistory::Canceled:    return QStringLiteral("Canceled");
    }
    return QString();
}

// The row's file-type label: the real extension once the file exists (handles
// "Best Available" resolving to whatever it resolves to), else the format the
// download was queued with, else just Video/Audio. Empty for legacy records.
QString typeText(const SgDownloadHistory::Record& r) {
    const QString ext = QFileInfo(r.filePath).suffix();
    if (!ext.isEmpty()) return ext.toUpper();
    if (!r.fmt.isEmpty() && r.fmt.compare(QStringLiteral("Best Available"), Qt::CaseInsensitive) != 0)
        return r.fmt.toUpper();
    return r.kind;
}

// Date/time for the row: when it finished for done/failed/canceled entries,
// otherwise when it was queued.
QString whenText(const SgDownloadHistory::Record& r) {
    const qint64 ms = (r.finishedAt > 0) ? r.finishedAt : r.addedAt;
    if (ms <= 0) return QString(); // legacy records may predate the timestamps
    return QLocale().toString(QDateTime::fromMSecsSinceEpoch(ms), QLocale::ShortFormat);
}
}

DownloadRow::DownloadRow(const SgDownloadHistory::Record& rec, QNetworkAccessManager* nam,
                         QWidget* parent)
    : QFrame(parent), m_id(rec.id), m_thumbUrl(rec.thumbUrl),
      m_filePath(rec.filePath), m_status(rec.status) {
    setObjectName("downloadRow");
    // Fixed, compact height so entries don't stretch to fill the list (and stay uniform).
    setFixedHeight(kRowH);
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);

    auto* row = new QHBoxLayout(this);
    row->setContentsMargins(8, 6, 10, 6);
    row->setSpacing(12);

    // Thumbnail (rounded via the themed object name; a "…" placeholder until it loads).
    m_thumb = new QLabel(QStringLiteral("…"));
    m_thumb->setObjectName("downloadRowThumb");
    m_thumb->setFixedSize(kThumbW, kThumbH);
    m_thumb->setAlignment(Qt::AlignCenter);
    m_thumb->setScaledContents(true);
    row->addWidget(m_thumb, 0, Qt::AlignVCenter);

    // Middle column (takes the width): title, then a status + speed/ETA line, then a thin
    // progress bar while downloading. A trailing stretch pins everything to the top.
    auto* mid = new QVBoxLayout();
    mid->setContentsMargins(0, 0, 0, 0);
    mid->setSpacing(3);

    m_title = new MarqueeLabel(rec.title.isEmpty() ? rec.pageUrl : rec.title);
    m_title->setObjectName("downloadRowTitle");
    m_title->setWordWrap(false);
    m_title->setTextInteractionFlags(Qt::NoTextInteraction);
    // Ignored width so a long title elides instead of widening the row.
    m_title->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    mid->addWidget(m_title);

    auto* infoRow = new QHBoxLayout();
    infoRow->setContentsMargins(0, 0, 0, 0);
    infoRow->setSpacing(10);
    m_statusLabel = new QLabel();
    m_statusLabel->setObjectName("downloadRowStatus");
    // File type (e.g. MP4 / MP3), then speed/ETA while downloading; the queued/finished
    // date-time sits pinned at the info line's right edge. All dim themed text.
    auto* typeLabel = new QLabel(typeText(rec));
    typeLabel->setObjectName("metaStats");
    typeLabel->setVisible(!typeLabel->text().isEmpty()); // legacy records have no type
    m_meta = new QLabel();
    m_meta->setObjectName("metaStats"); // themed dim text
    m_meta->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred); // clip long paths
    auto* whenLabel = new QLabel(whenText(rec));
    whenLabel->setObjectName("metaStats");
    infoRow->addWidget(m_statusLabel);
    infoRow->addWidget(typeLabel);
    infoRow->addWidget(m_meta, 1);
    infoRow->addWidget(whenLabel, 0, Qt::AlignRight);
    mid->addLayout(infoRow);

    m_bar = new QProgressBar();
    m_bar->setRange(0, 100);
    m_bar->setValue(0);
    m_bar->setTextVisible(false);
    m_bar->setFixedHeight(6);
    mid->addWidget(m_bar);

    mid->addStretch();
    row->addLayout(mid, 1);

    // Right: action buttons in a compact horizontal row, vertically centred.
    auto* actions = new QHBoxLayout();
    actions->setContentsMargins(0, 0, 0, 0);
    actions->setSpacing(6);
    auto makeBtn = [this, actions](const QString& text) {
        auto* b = new QPushButton(text);
        b->setObjectName("downloadRowButton");
        b->setCursor(Qt::PointingHandCursor);
        actions->addWidget(b);
        return b;
    };
    m_restartBtn = makeBtn(QStringLiteral("Restart"));
    m_openBtn    = makeBtn(QStringLiteral("Open folder"));
    m_cancelBtn  = makeBtn(QStringLiteral("Cancel"));
    m_removeBtn  = makeBtn(QStringLiteral("Remove"));
    row->addLayout(actions, 0);

    connect(m_restartBtn, &QPushButton::clicked, this, [this]() { emit restartRequested(m_id); });
    connect(m_cancelBtn,  &QPushButton::clicked, this, [this]() { emit cancelRequested(m_id); });
    connect(m_removeBtn,  &QPushButton::clicked, this, [this]() { emit removeRequested(m_id); });
    connect(m_openBtn,    &QPushButton::clicked, this, [this]() { emit openFolderRequested(m_filePath); });

    applyStatus(m_status);
    if (nam) loadThumbnail(nam);
}

void DownloadRow::applyStatus(int status) {
    m_status = status;
    m_statusLabel->setText(statusText(status));

    // Status colour: green completed, red failed, accent downloading, dim otherwise.
    QString colour;
    switch (status) {
    case SgDownloadHistory::Completed:   colour = kGreen; break;
    case SgDownloadHistory::Failed:      colour = kRed;   break;
    case SgDownloadHistory::Downloading: colour = palette().color(QPalette::Highlight).name(); break;
    default:                             colour = palette().color(QPalette::WindowText).name(); break;
    }
    m_statusLabel->setStyleSheet("color:" + colour + ";");

    const bool downloading = (status == SgDownloadHistory::Downloading);
    m_bar->setVisible(downloading);
    if (!downloading) m_meta->clear(); // speed/ETA only matters mid-download

    // Buttons per status.
    const bool terminal = (status == SgDownloadHistory::Completed
                        || status == SgDownloadHistory::Failed
                        || status == SgDownloadHistory::Canceled);
    m_restartBtn->setVisible(terminal);
    m_openBtn->setVisible(status == SgDownloadHistory::Completed && !m_filePath.isEmpty());
    m_cancelBtn->setVisible(status == SgDownloadHistory::Queued || downloading);
    m_removeBtn->setVisible(!downloading); // don't yank a row mid-download; Cancel first
}

void DownloadRow::setLiveProgress(double percent, const QString& speed, const QString& eta) {
    if (m_status != SgDownloadHistory::Downloading) applyStatus(SgDownloadHistory::Downloading);
    m_bar->setValue(qBound(0, int(percent + 0.5), 100));
    QStringList parts;
    if (!speed.isEmpty()) parts << speed;
    if (!eta.isEmpty())   parts << ("ETA " + eta);
    m_meta->setText(parts.join("   "));
}

// Hovering anywhere on the row starts the title marquee (long titles elide at
// rest); leaving snaps it back.
void DownloadRow::enterEvent(QEnterEvent* event) {
    if (m_title) m_title->setMarqueeOn(true);
    QFrame::enterEvent(event);
}

void DownloadRow::leaveEvent(QEvent* event) {
    if (m_title) m_title->setMarqueeOn(false);
    QFrame::leaveEvent(event);
}

void DownloadRow::loadThumbnail(QNetworkAccessManager* nam) {
    if (m_thumbUrl.isEmpty()) return; // keep the "…" placeholder

    QPixmap cached;
    if (QPixmapCache::find(m_thumbUrl, &cached)) { m_thumb->setPixmap(cached); return; }

    QNetworkRequest req((QUrl(m_thumbUrl)));
    req.setRawHeader("User-Agent", "Seagull-Player");
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    QNetworkReply* reply = nam->get(req);
    const QString key = m_thumbUrl;
    connect(reply, &QNetworkReply::finished, this, [this, reply, key]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) return;
        const QByteArray data = reply->readAll();
        QPixmap pm;
        if (pm.loadFromData(data)) { QPixmapCache::insert(key, pm); m_thumb->setPixmap(pm); return; }
        // WebP (no qwebp plugin in this Qt build) — round-trip through ffmpeg to PNG.
        SgThumbnailer::decodeViaFfmpeg(data, this, [this, key](const QPixmap& decoded) {
            if (decoded.isNull()) return;
            QPixmapCache::insert(key, decoded);
            m_thumb->setPixmap(decoded);
        });
    });
}
