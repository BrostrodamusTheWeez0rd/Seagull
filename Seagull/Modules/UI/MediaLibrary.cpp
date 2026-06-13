#include "MediaLibrary.h"
#include "Widgets/FlowLayout.h"
#include "Widgets/VideoCard.h"
#include "../Backend/SgPaths.h"
#include "../Backend/SgThumbnailer.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QScrollArea>
#include <QScrollBar>
#include <QPushButton>
#include <QButtonGroup>
#include <QFrame>
#include <QLabel>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QLocale>
#include <QSettings>
#include <QCoreApplication>
#include <QCursor>
#include <QEvent>
#include <QShowEvent>
#include <QTimer>

namespace {
constexpr int kGridSpacing = 12;
constexpr int kPillTopMargin = 10;  // gap between the tab top and the floating pill
constexpr int kMaxCards = 300;      // hard cap so a huge folder can't stall the UI
}

MediaLibrary::MediaLibrary(QWidget* parent) : QWidget(parent) {
    thumbnailer = new SgThumbnailer(this);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(10, 0, 10, 10);

    // --- Card grid (same grow-to-fill flow as the Search tab) ---
    cardsArea = new QScrollArea();
    cardsArea->setWidgetResizable(true);
    cardsArea->setFrameShape(QFrame::NoFrame);
    cardsArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    // Always-on vertical so the scrollbar can't flicker on/off and oscillate the
    // grid width (which would re-flow on a loop).
    cardsArea->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
    cardsArea->viewport()->setAutoFillBackground(true);
    cardsArea->viewport()->setBackgroundRole(QPalette::Window);

    QSettings cfg(QCoreApplication::applicationDirPath() + "/config.ini", QSettings::IniFormat);
    m_targetWidth = qBound(120, cfg.value("Display/CardWidth", 360).toInt(), 480); // default Extra Large
    m_cardWidth = m_targetWidth;

    cardsHost = new QWidget();
    cardsFlow = new FlowLayout(cardsHost, 0, kGridSpacing, kGridSpacing);
    cardsArea->setWidget(cardsHost);
    root->addWidget(cardsArea, 1);

    cardsArea->viewport()->installEventFilter(this);

    // --- Floating translucent type switcher, overlaid on the grid ---
    // A plain child widget (not in the layout): the grid scrolls underneath it.
    typePill = new QFrame(this);
    typePill->setObjectName("libraryTypePill"); // themed in Theme::apply
    auto* pillLay = new QHBoxLayout(typePill);
    pillLay->setContentsMargins(8, 5, 8, 5);
    pillLay->setSpacing(4);

    typeGroup = new QButtonGroup(this);
    typeGroup->setExclusive(true);
    const struct { const char* label; MediaType type; } kinds[] = {
        { "Videos",     MediaType::Video },
        { "Audio",      MediaType::Audio },
        { "Images",     MediaType::Image },
        { "Recordings", MediaType::Recording },
        { "Playlists",  MediaType::Playlist },
    };
    for (const auto& k : kinds) {
        auto* b = new QPushButton(k.label, typePill);
        b->setObjectName("libraryTypeButton");
        b->setCheckable(true);
        b->setCursor(Qt::PointingHandCursor);
        b->setChecked(k.type == m_type);
        typeGroup->addButton(b, static_cast<int>(k.type));
        pillLay->addWidget(b);
    }
    connect(typeGroup, &QButtonGroup::idClicked, this, [this](int id) {
        const auto t = static_cast<MediaType>(id);
        if (t == m_type) return;
        m_type = t;
        rebuild();
        });

    // Keep the first row of cards clear of the pill's resting place. (FlowLayout
    // lays out within its own contents margins, so set them there.)
    const int pillH = typePill->sizeHint().height();
    cardsFlow->setContentsMargins(0, pillH + 2 * kPillTopMargin, 0, 0);

    // Centered note for an empty folder; floats like the pill does.
    emptyLabel = new QLabel(this);
    emptyLabel->setObjectName("libraryEmptyLabel");
    emptyLabel->setAlignment(Qt::AlignCenter);
    emptyLabel->hide();

    connect(thumbnailer, &SgThumbnailer::thumbnailReady, this,
        [this](const QString& filePath, const QPixmap& pm) {
            if (auto card = m_pendingThumbs.take(filePath)) card->setThumbnail(pm);
        });

    // Pill auto-hide: pinned while the grid sits at the top, gone once scrolled,
    // peeking back when the cursor enters its strip. Cards swallow mouse-moves
    // (they're children over the viewport), so a light cursor poll does the
    // hover check instead of mouse tracking.
    connect(cardsArea->verticalScrollBar(), &QScrollBar::valueChanged, this,
        [this](int) { updatePillVisibility(); });
    pillHoverTimer = new QTimer(this);
    pillHoverTimer->setInterval(150);
    connect(pillHoverTimer, &QTimer::timeout, this, &MediaLibrary::updatePillVisibility);
}

void MediaLibrary::updatePillVisibility() {
    const bool atTop = (cardsArea->verticalScrollBar()->value() <= 0);
    // The pill's strip across the top of the tab, slightly taller than the pill
    // itself so recovering it isn't pixel-hunting.
    const QRect zone(0, 0, width(), typePill->height() + 2 * kPillTopMargin);
    const bool hovered = zone.contains(mapFromGlobal(QCursor::pos()));
    typePill->setVisible(atTop || hovered);
}

QString MediaLibrary::folderForType() const {
    switch (m_type) {
    case MediaType::Audio:     return SgPaths::audioFolder();
    case MediaType::Image:     return SgPaths::photoFolder();
    case MediaType::Recording: return SgPaths::recordingFolder();
    case MediaType::Playlist:  return SgPaths::playlistFolder();
    case MediaType::Video:     break;
    }
    return SgPaths::videoFolder();
}

QStringList MediaLibrary::extensionsForType() const {
    static const QStringList video = { "*.mp4", "*.mkv", "*.avi", "*.ts", "*.webm", "*.mov" };
    static const QStringList audio = { "*.mp3", "*.m4a", "*.opus", "*.wav", "*.flac" };
    static const QStringList image = { "*.jpg", "*.jpeg", "*.png", "*.gif", "*.webp", "*.bmp" };
    switch (m_type) {
    case MediaType::Audio:     return audio;
    case MediaType::Image:     return image;
    case MediaType::Recording: return video + audio; // recordings can be either
    case MediaType::Playlist:  return { "*.sgpl" };
    case MediaType::Video:     break;
    }
    return video;
}

void MediaLibrary::clearCards() {
    thumbnailer->cancelPending();
    m_pendingThumbs.clear();
    QLayoutItem* item;
    while ((item = cardsFlow->takeAt(0)) != nullptr) {
        if (QWidget* w = item->widget()) w->deleteLater();
        delete item;
    }
}

void MediaLibrary::rebuild() {
    clearCards();
    m_files.clear();
    m_currentPlayIndex = -1;

    QDir dir(folderForType());
    QFileInfoList entries = dir.entryInfoList(extensionsForType(), QDir::Files, QDir::Time);
    if (entries.size() > kMaxCards) entries = entries.mid(0, kMaxCards);

    const bool playlists = (m_type == MediaType::Playlist);
    for (const QFileInfo& fi : entries) {
        const QString path = fi.absoluteFilePath();

        SearchResult r;
        r.url = QUrl::fromLocalFile(path).toString();
        // duration/viewCount stay -1 (omitted); thumbnail "" = no network fetch.

        if (playlists) {
            // .sgpl card: display name + entry count from the (tiny) JSON;
            // playing routes through the Queue tab, never the local-file path.
            QString plName = fi.completeBaseName();
            int count = 0;
            QFile f(path);
            if (f.open(QIODevice::ReadOnly)) {
                const QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
                if (!root["name"].toString().isEmpty()) plName = root["name"].toString();
                count = root["entries"].toArray().size();
            }
            r.title = plName;
            r.channel = QString("%1 item%2").arg(count).arg(count == 1 ? "" : "s");

            auto* card = new VideoCard(r, nullptr, m_cardWidth, cardsHost, VideoCard::PlayButton);
            card->setThumbnailPlaceholder(QStringLiteral("≡"));
            connect(card, &VideoCard::playRequested, this, [this, path](const QUrl&, const QString&) {
                emit playPlaylistRequested(path);
                });
            cardsFlow->addWidget(card);
            continue; // no m_files entry (advance is the Queue's job), no thumbnail
        }

        m_files.append(path);
        r.title = fi.completeBaseName();
        r.channel = QLocale().formattedDataSize(fi.size()); // the meta line's first slot

        auto* card = new VideoCard(r, nullptr, m_cardWidth, cardsHost,
            VideoCard::PlayButton | VideoCard::QueueButton);

        // Audio shows a music note while (or in case no) cover art arrives.
        static const QStringList audioExts = { "mp3", "m4a", "opus", "wav", "flac" };
        if (audioExts.contains(fi.suffix().toLower()))
            card->setThumbnailPlaceholder(QStringLiteral("♪"));

        const int index = m_files.size() - 1;
        connect(card, &VideoCard::playRequested, this, [this, index](const QUrl& url, const QString&) {
            m_currentPlayIndex = index;
            emit playMediaRequested(url);
            });
        // Card "Queue" -> the Queue tab's local queue (purity handled there).
        connect(card, &VideoCard::queueRequested, this, [this, path](const QUrl&, const QString&) {
            emit enqueueLocalRequested({ path });
            });
        cardsFlow->addWidget(card);

        m_pendingThumbs.insert(path, card);
        thumbnailer->requestThumbnail(path);
    }

    applyCardWidth();

    // Playlist cards don't populate m_files (advance is the Queue's job), so the
    // empty check must count cards, not files.
    const bool noCards = (cardsFlow->count() == 0);
    const QString what = m_type == MediaType::Image    ? "Images"
                       : m_type == MediaType::Playlist ? "Playlists"
                                                       : "Media";
    emptyLabel->setText(QString("Nothing here yet.\n%1 saved to\n%2\nwill show up here.")
        .arg(what, QDir::toNativeSeparators(dir.absolutePath())));
    emptyLabel->setVisible(noCards);
    if (noCards) positionTypePill(); // also centers the empty note
}

void MediaLibrary::refresh() {
    rebuild();
}

bool MediaLibrary::thumbnailsBusy() const {
    return thumbnailer->isBusy();
}

void MediaLibrary::setThumbnailsHeld(bool held) {
    thumbnailer->setHeld(held);
}

void MediaLibrary::playNextFile() {
    if (m_files.isEmpty()) return;
    const int next = m_currentPlayIndex + 1;
    if (next >= m_files.size()) return; // end of the grid — stop
    m_currentPlayIndex = next;
    emit playMediaRequested(QUrl::fromLocalFile(m_files[next]));
}

void MediaLibrary::playPrevFile() {
    if (m_files.isEmpty() || m_currentPlayIndex <= 0) return;
    --m_currentPlayIndex;
    emit playMediaRequested(QUrl::fromLocalFile(m_files[m_currentPlayIndex]));
}

int MediaLibrary::fillCardWidth() const {
    const int vw = cardsArea->viewport()->width();
    const int target = qMax(120, m_targetWidth);
    if (vw <= 0) return target;
    const int cols = qMax(1, (vw + kGridSpacing) / (target + kGridSpacing));
    return (vw - (cols - 1) * kGridSpacing) / cols;
}

void MediaLibrary::applyCardWidth() {
    const int w = fillCardWidth();
    if (w == m_cardWidth && !cardsFlow->isEmpty()) return;
    m_cardWidth = w;
    for (int i = 0; i < cardsFlow->count(); ++i) {
        if (auto* card = qobject_cast<VideoCard*>(cardsFlow->itemAt(i)->widget()))
            card->setCardWidth(w);
    }
}

void MediaLibrary::setCardWidth(int targetWidth) {
    m_targetWidth = qBound(120, targetWidth, 480);
    applyCardWidth();
}

void MediaLibrary::positionTypePill() {
    const QSize pill = typePill->sizeHint();
    typePill->resize(pill);
    typePill->move((width() - pill.width()) / 2, kPillTopMargin);
    typePill->raise();

    if (emptyLabel->isVisible()) {
        emptyLabel->adjustSize();
        emptyLabel->move((width() - emptyLabel->width()) / 2, height() / 2 - emptyLabel->height() / 2);
        emptyLabel->raise();
    }
}

void MediaLibrary::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    rebuild(); // pick up files that landed while another tab was active
    positionTypePill();
    updatePillVisibility();
    pillHoverTimer->start();
}

void MediaLibrary::hideEvent(QHideEvent* event) {
    QWidget::hideEvent(event);
    pillHoverTimer->stop(); // no cursor poll while another tab is active
}

void MediaLibrary::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    positionTypePill();
}

bool MediaLibrary::eventFilter(QObject* obj, QEvent* event) {
    if (obj == cardsArea->viewport() && event->type() == QEvent::Resize)
        applyCardWidth();
    return QWidget::eventFilter(obj, event);
}
