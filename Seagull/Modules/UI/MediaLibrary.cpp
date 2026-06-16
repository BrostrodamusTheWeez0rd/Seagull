#include "MediaLibrary.h"
#include "Widgets/FlowLayout.h"
#include "Widgets/VideoCard.h"
#include "Widgets/SpellCheckLineEdit.h"
#include "../Backend/SgPaths.h"
#include "../Backend/SgThumbnailer.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QScrollArea>
#include <QScrollBar>
#include <QPushButton>
#include <QButtonGroup>
#include <QMenu>
#include <QActionGroup>
#include <QFrame>
#include <QLabel>
#include <QLineEdit>
#include <QIcon>
#include <QPixmap>
#include <QPainter>
#include <QPalette>
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
constexpr int kSearchGraceMs = 600; // keep the search bar up briefly after the magnifier click
}

MediaLibrary::MediaLibrary(SgSpellCheck* spell, QWidget* parent) : QWidget(parent), m_spell(spell) {
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
    m_sortMode = static_cast<SortMode>(qBound(0,
        cfg.value("Library/SortMode", static_cast<int>(SortMode::DateNewest)).toInt(),
        static_cast<int>(SortMode::DateOldest)));

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

    // Floating magnifier at the top-right. A plain child of the tab (like the
    // type pill), it follows the same auto-hide rules. Clicking it reveals the
    // search bar; the SVG glyph is tinted to the theme text colour in code.
    searchButton = new QPushButton(this);
    searchButton->setObjectName("librarySearchButton"); // themed in Theme::apply
    searchButton->setCursor(Qt::PointingHandCursor);
    searchButton->setFixedSize(34, 34);
    searchButton->setToolTip("Search this library");
    tintSearchIcon();
    connect(searchButton, &QPushButton::clicked, this, &MediaLibrary::toggleSearch);

    // Floating sort/order button, sitting to the right of the magnifier. Same size
    // and auto-hide rules; a click drops a menu of the basic orderings.
    sortButton = new QPushButton(this);
    sortButton->setObjectName("librarySearchButton"); // reuse the magnifier's themed style
    sortButton->setCursor(Qt::PointingHandCursor);
    sortButton->setFixedSize(34, 34);
    sortButton->setToolTip("Sort this library");
    tintSortIcon();
    connect(sortButton, &QPushButton::clicked, this, &MediaLibrary::showSortMenu);

    sortMenu = new QMenu(this);
    auto* sortGroup = new QActionGroup(sortMenu);
    sortGroup->setExclusive(true);
    const struct { const char* label; SortMode mode; } orderings[] = {
        { "Name (A\xE2\x80\x93" "Z)",  SortMode::NameAsc },
        { "Name (Z\xE2\x80\x93" "A)",  SortMode::NameDesc },
        { "Newest first",           SortMode::DateNewest },
        { "Oldest first",           SortMode::DateOldest },
    };
    for (const auto& o : orderings) {
        QAction* a = sortMenu->addAction(QString::fromUtf8(o.label));
        a->setCheckable(true);
        a->setChecked(o.mode == m_sortMode);
        sortGroup->addAction(a);
        const SortMode mode = o.mode;
        connect(a, &QAction::triggered, this, [this, mode] { applySortMode(mode); });
    }

    // The search bar itself: revealed on click, hidden by default. Uses the
    // shared OS spell checker (red squiggles + suggestions), like the Search and
    // File Explorer search fields.
    librarySearch = new SpellCheckLineEdit(m_spell, this);
    librarySearch->setObjectName("librarySearchBar"); // themed in Theme::apply
    librarySearch->setPlaceholderText("Search\xE2\x80\xA6");
    librarySearch->setClearButtonEnabled(true);
    librarySearch->setFixedWidth(180);
    librarySearch->hide();
    connect(librarySearch, &QLineEdit::textChanged, this, [this](const QString& t) {
        m_query = t;
        filterCards();
        });
    // Auto-collapse is poll-driven (see updatePillVisibility): the bar hides once
    // it's empty, unfocused, and not just opened.

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
    const bool show = atTop || hovered;
    typePill->setVisible(show);
    sortButton->setVisible(show); // always paired with the pill/magnifier strip

    // While the search bar is open it takes the magnifier's place. Keep it only
    // while it's actually in use: focused (typing), holding a query, or just
    // opened (grace after the click). Once it's empty, unfocused, and past the
    // grace, collapse back to the magnifier.
    if (m_searchOpen) {
        const bool inUse = librarySearch->hasFocus()
                        || !m_query.trimmed().isEmpty()
                        || (m_searchOpenedClock.isValid() && m_searchOpenedClock.elapsed() < kSearchGraceMs);
        if (inUse) {
            searchButton->hide();
            librarySearch->setVisible(true);
            return;
        }
        m_searchOpen = false;     // done with it — fall through to restore the magnifier
        librarySearch->clear();   // reset (clears any whitespace) so it reopens blank
    }
    librarySearch->hide();
    searchButton->setVisible(show);
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
    m_cards.clear();
    QLayoutItem* item;
    while ((item = cardsFlow->takeAt(0)) != nullptr) {
        if (QWidget* w = item->widget()) w->deleteLater();
        delete item;
    }
}

VideoCard* MediaLibrary::addCardForEntry(const QFileInfo& fi) {
    const QString path = fi.absoluteFilePath();

    SearchResult r;
    r.url = QUrl::fromLocalFile(path).toString();
    // duration/viewCount stay -1 (omitted); thumbnail "" = no network fetch.

    if (m_type == MediaType::Playlist) {
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
        m_cards.append({ card, r.title.toLower() });
        return card; // no m_files entry (advance is the Queue's job), no thumbnail
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
    m_cards.append({ card, r.title.toLower() });

    m_pendingThumbs.insert(path, card);
    thumbnailer->requestThumbnail(path);
    return card;
}

void MediaLibrary::setBuildBusy(bool busy) {
    if (m_buildBusy == busy) return;
    m_buildBusy = busy;
    emit buildBusyChanged(busy);
}

void MediaLibrary::rebuild() {
    if (m_buildTimer) m_buildTimer->stop(); // cancel any in-progress incremental build
    setBuildBusy(true); // about to hammer the GUI thread building cards; let the visualizer yield
    clearCards();
    m_files.clear();
    m_currentPlayIndex = -1;

    QDir dir(folderForType());
    QDir::SortFlags sortFlags = QDir::Time; // Newest first (QDir::Time is most-recent-first)
    switch (m_sortMode) {
    case SortMode::NameAsc:     sortFlags = QDir::Name | QDir::IgnoreCase; break;
    case SortMode::NameDesc:    sortFlags = QDir::Name | QDir::IgnoreCase | QDir::Reversed; break;
    case SortMode::DateNewest:  sortFlags = QDir::Time; break;
    case SortMode::DateOldest:  sortFlags = QDir::Time | QDir::Reversed; break;
    }
    m_buildQueue = dir.entryInfoList(extensionsForType(), QDir::Files, sortFlags);
    if (m_buildQueue.size() > kMaxCards) m_buildQueue = m_buildQueue.mid(0, kMaxCards);
    m_buildPos = 0;

    // One fill-width for the whole build; cards are created at that width.
    m_cardWidth = fillCardWidth();

    // Default empty-note text for a truly empty folder; filterCards() decides
    // whether to show it (or a "no matches" note when a search filters all out).
    const QString what = m_type == MediaType::Image    ? "Images"
                       : m_type == MediaType::Playlist ? "Playlists"
                                                       : "Media";
    emptyLabel->setText(QString("Nothing here yet.\n%1 saved to\n%2\nwill show up here.")
        .arg(what, QDir::toNativeSeparators(dir.absolutePath())));

    // Build incrementally so a big folder never freezes the UI on a tab/category
    // switch: first batch now, the rest on an idle timer.
    if (!m_buildTimer) {
        m_buildTimer = new QTimer(this);
        m_buildTimer->setInterval(0); // fire between event-loop passes -> UI stays live
        connect(m_buildTimer, &QTimer::timeout, this, &MediaLibrary::buildNextBatch);
    }
    buildNextBatch();
    if (m_buildPos < m_buildQueue.size()) m_buildTimer->start();
}

void MediaLibrary::buildNextBatch() {
    constexpr int kBatch = 16; // cards per pass: small enough to keep the UI responsive
    const QString q = m_query.trimmed().toLower();
    const int end = qMin(m_buildPos + kBatch, m_buildQueue.size());
    for (; m_buildPos < end; ++m_buildPos) {
        VideoCard* card = addCardForEntry(m_buildQueue[m_buildPos]);
        // Respect the active query as cards appear (avoids an end-of-build flicker).
        if (card && !q.isEmpty()) card->setVisible(m_cards.last().second.contains(q));
    }
    cardsFlow->invalidate(); // reflow the newly added cards

    if (m_buildPos >= m_buildQueue.size()) {
        if (m_buildTimer) m_buildTimer->stop();
        m_buildQueue.clear();
        applyCardWidth();
        filterCards(); // final pass: visibility + empty-note + reflow
        setBuildBusy(false); // build done -> the visualizer can animate again
    }
}

void MediaLibrary::filterCards() {
    const QString q = m_query.trimmed().toLower();
    int visible = 0;
    for (const auto& entry : m_cards) {
        VideoCard* c = entry.first;
        if (!c) continue;
        const bool match = q.isEmpty() || entry.second.contains(q);
        c->setVisible(match);
        if (match) ++visible;
    }
    cardsFlow->invalidate(); // re-flow now that some cards are hidden

    if (m_cards.isEmpty()) {
        emptyLabel->setVisible(true);            // folder message (set by rebuild)
    } else if (visible == 0) {
        emptyLabel->setText("No media matches your search.");
        emptyLabel->setVisible(true);
    } else {
        emptyLabel->setVisible(false);
    }
    if (emptyLabel->isVisible()) positionTypePill(); // also centers the note
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

    positionSearch();

    if (emptyLabel->isVisible()) {
        emptyLabel->adjustSize();
        emptyLabel->move((width() - emptyLabel->width()) / 2, height() / 2 - emptyLabel->height() / 2);
        emptyLabel->raise();
    }
}

void MediaLibrary::positionSearch() {
    // Top-right, just inside the always-on vertical scrollbar so it never overlaps.
    const int sb = cardsArea->verticalScrollBar()->isVisible()
                       ? cardsArea->verticalScrollBar()->width() : 0;
    const int rightEdge = width() - kPillTopMargin - sb;
    constexpr int gap = 6;

    // Sort sits at the far right; the magnifier (and its expanding bar) to its left.
    sortButton->move(rightEdge - sortButton->width(), kPillTopMargin);
    sortButton->raise();

    const int searchRight = rightEdge - sortButton->width() - gap;
    searchButton->move(searchRight - searchButton->width(), kPillTopMargin);
    searchButton->raise();
    librarySearch->move(searchRight - librarySearch->width(),
        kPillTopMargin + (searchButton->height() - librarySearch->height()) / 2);
    librarySearch->raise();
}

void MediaLibrary::toggleSearch() {
    m_searchOpen = !m_searchOpen;
    if (m_searchOpen) {
        m_searchOpenedClock.restart(); // grace so it doesn't collapse the instant it opens
        librarySearch->show();
        librarySearch->raise();
        librarySearch->setFocus(Qt::OtherFocusReason);
    } else {
        librarySearch->clear(); // drops the filter
        librarySearch->hide();
    }
    positionSearch();
    updatePillVisibility();
}

void MediaLibrary::tintSearchIcon() {
    if (!searchButton) return;
    // QSS can't recolour a QIcon, so flat-tint the glyph to the theme text colour
    // (same trick the player controls use). Re-run on every theme change.
    const QSize sz(18, 18);
    QPixmap pm = QIcon(QStringLiteral(":/Assets/icons/search.svg")).pixmap(sz);
    if (pm.isNull()) return;
    QPainter p(&pm);
    p.setCompositionMode(QPainter::CompositionMode_SourceIn);
    p.fillRect(pm.rect(), palette().color(QPalette::WindowText));
    p.end();
    searchButton->setIcon(QIcon(pm));
    searchButton->setIconSize(sz);
}

void MediaLibrary::tintSortIcon() {
    if (!sortButton) return;
    // Same flat-tint trick as the magnifier: recolour the glyph to the theme text.
    const QSize sz(18, 18);
    QPixmap pm = QIcon(QStringLiteral(":/Assets/icons/sort.svg")).pixmap(sz);
    if (pm.isNull()) return;
    QPainter p(&pm);
    p.setCompositionMode(QPainter::CompositionMode_SourceIn);
    p.fillRect(pm.rect(), palette().color(QPalette::WindowText));
    p.end();
    sortButton->setIcon(QIcon(pm));
    sortButton->setIconSize(sz);
}

void MediaLibrary::showSortMenu() {
    // Drop the menu flush under the button's bottom-left corner.
    sortMenu->popup(sortButton->mapToGlobal(QPoint(0, sortButton->height())));
}

void MediaLibrary::applySortMode(SortMode mode) {
    if (mode == m_sortMode) return;
    m_sortMode = mode;
    QSettings cfg(QCoreApplication::applicationDirPath() + "/config.ini", QSettings::IniFormat);
    cfg.setValue("Library/SortMode", static_cast<int>(mode));
    rebuild(); // re-list the active folder in the new order
}

void MediaLibrary::changeEvent(QEvent* event) {
    QWidget::changeEvent(event);
    if (event->type() == QEvent::PaletteChange || event->type() == QEvent::StyleChange) {
        tintSearchIcon();
        tintSortIcon();
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
    if (m_buildTimer) m_buildTimer->stop(); // pause any in-progress build; showEvent rebuilds
    setBuildBusy(false); // build paused -> don't leave the visualizer suspended
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
