#pragma once

#include <QWidget>
#include <QUrl>
#include <QStringList>
#include <QHash>
#include <QPointer>

#include <QList>
#include <QPair>
#include <QSet>
#include <QElapsedTimer>
#include <QFileInfoList>

class QScrollArea;
class QPushButton;
class QButtonGroup;
class QFrame;
class QLabel;
class QMenu;
class QAction;
class QDir;
class QTimer;
class FlowLayout;
class VideoCard;
class SgThumbnailer;
class SgSpellCheck;
class SpellCheckLineEdit;

// The "Library" tab: a card-grid view of the user's saved media, one content
// type at a time. The floating pill at the top switches between the SgPaths
// folders (Videos / Audio / Images / Recordings / Playlists) — the buttons
// aren't filters, they literally select which folder is shown. Cards reuse the
// Search tab's VideoCard; thumbnails come from SgThumbnailer. Playlist cards
// (.sgpl files) show name + entry count and play through the Queue tab.
class MediaLibrary : public QWidget {
    Q_OBJECT

public:
    enum class MediaType { Video, Audio, Image, Recording, Playlist };
    // How the grid is ordered. Persisted globally to config.ini (Library/SortMode)
    // as the raw int, so new modes must only ever be APPENDED.
    // ContinueWatching is a filter as well as an order: it shows only files with a
    // resume point in SgWatchHistory, most-recently-watched first. It means nothing
    // for Image/Playlist (never recorded), where it falls back to DateNewest.
    enum class SortMode { NameAsc, NameDesc, DateNewest, DateOldest, ContinueWatching };

    explicit MediaLibrary(SgSpellCheck* spell, QWidget* parent = nullptr);

    void setCardWidth(int targetWidth); // live from Settings -> Display "Card size"

    // Thumbnail generation still running?
    bool thumbnailsBusy() const;

    // Hold/release the thumbnail ffmpeg queue. Startup holds it until the
    // update modal finishes so an ffmpeg.exe swap never races a running grab.
    void setThumbnailsHeld(bool held);

    // Settings namespace for the media type that's currently playing (e.g.
    // "library.video"), so autoplay/shuffle settings are remembered per type.
    QString sessionContextKey() const;

signals:
    void playMediaRequested(const QUrl& url);
    void playPlaylistRequested(const QString& path);       // .sgpl card -> Queue loads + plays it
    void enqueueLocalRequested(const QStringList& paths);  // card Queue button -> Queue tab
    void buildBusyChanged(bool busy);                      // incremental card build running -> let the visualizer yield the GUI thread

public slots:
    void playNextFile();   // auto-advance / skip, in displayed order
    void playPrevFile();
    void playRandomFile(); // shuffle: a random other file from the play session
    void refresh();        // re-list the active folder

protected:
    void showEvent(QShowEvent* event) override;       // refresh on tab switch
    void hideEvent(QHideEvent* event) override;       // pause the pill hover poll
    void resizeEvent(QResizeEvent* event) override;   // reposition the floating pill
    void changeEvent(QEvent* event) override;         // re-tint the search icon on theme change
    bool eventFilter(QObject* obj, QEvent* event) override; // viewport resize -> refit cards

private:
    void rebuild();                 // folder listing -> kick off the incremental card build
    void buildNextBatch();          // create the next chunk of cards (keeps the UI responsive)
    VideoCard* addCardForEntry(const QFileInfo& fi); // build one card for a file/playlist entry
    void armCardForSelection(VideoCard* card, const QString& path); // wire delete-mode selection
    void clearCards();
    void filterCards();             // hide cards not matching the search query
    void applyCardWidth();
    int  fillCardWidth() const;
    QString folderForType() const;       // the SgPaths folder for the active type
    QStringList extensionsForType() const;
    void positionTypePill();
    void updatePillVisibility(); // visible at scroll-top or when its strip is hovered
    void positionSearch();       // place the sort + magnifier + trash buttons / bar at the top-right
    void toggleSearch();         // magnifier click: reveal the search bar (or collapse)

    // Delete mode: arm the trash toggle, turn every card into a multi-select toggle,
    // then remove whatever's picked. Selections are keyed by absolute path so they
    // survive switching the grid's type (delete across Videos/Audio/Images at once).
    void setDeleteMode(bool on);
    void updateDeleteControls();      // enable/label the confirm button from the selection count
    void tintDeleteIcon(bool armed);  // recolour the trash glyph (red when armed, theme text otherwise)
    void deleteSelected();            // confirm, then send the selected files to the Recycle Bin
    void tintSearchIcon();       // recolour the magnifier glyph to the theme text colour
    void tintSortIcon();         // recolour the sort glyph to the theme text colour
    void showSortMenu();         // sort click: drop the ordering menu under the button
    void applySortMode(SortMode mode); // remember + persist the choice, then re-list
    bool supportsContinueWatching() const; // false for Image/Playlist (no watch history)
    QFileInfoList listContinueWatching(const QDir& dir) const; // watched-but-unfinished, newest first

    MediaType m_type = MediaType::Video;

    QFrame*       typePill = nullptr;     // floating translucent type switcher
    QButtonGroup* typeGroup = nullptr;
    QPushButton*  searchButton = nullptr;  // floating magnifier at the top-right
    QPushButton*  sortButton = nullptr;    // floating sort/order button, right of the magnifier
    QPushButton*  deleteButton = nullptr;  // floating trash toggle, left of the magnifier
    QPushButton*  confirmDeleteButton = nullptr; // red "Delete" action, shown while armed
    bool          m_deleteMode = false;    // is the trash armed (cards are selectable)?
    QSet<QString> m_selected;              // absolute paths picked for deletion (across types)
    QMenu*        sortMenu = nullptr;       // ordering options dropped under the sort button
    QAction*      continueAction = nullptr; // the Continue Watching entry; greyed out for Image/Playlist
    SortMode      m_sortMode = SortMode::DateNewest; // active grid ordering
    SpellCheckLineEdit* librarySearch = nullptr; // revealed on click; filters the active type
    bool          m_searchOpen = false;    // is the search bar revealed?
    QElapsedTimer m_searchOpenedClock;     // grace after the magnifier click before auto-collapse
    SgSpellCheck* m_spell = nullptr;       // shared OS spell checker (owned by Seagull)
    QString       m_query;                // current search text (lowercased on use)
    // Each displayed card + its lowercased title, for live filtering.
    QList<QPair<QPointer<VideoCard>, QString>> m_cards;
    QScrollArea*  cardsArea = nullptr;
    QWidget*      cardsHost = nullptr;
    FlowLayout*   cardsFlow = nullptr;
    QLabel*       emptyLabel = nullptr;   // centered "nothing here yet" note

    SgThumbnailer* thumbnailer = nullptr;
    QHash<QString, QPointer<VideoCard>> m_pendingThumbs; // file path -> its card
    QTimer* pillHoverTimer = nullptr; // cursor poll for the pill's auto-hide

    QStringList m_files;          // displayed order (newest first)
    int m_currentPlayIndex = -1;

    // Playback session: the ordered list + position captured the moment the user
    // pressed play. Auto-advance/skip walk THIS, not m_files, so switching the
    // grid's type/sort (which rebuilds m_files) can't hijack what plays next —
    // audio started from the Audio grid keeps advancing through audio even while
    // you're browsing the Video grid.
    QStringList m_sessionFiles;
    int m_sessionIndex = -1;
    MediaType m_sessionType = MediaType::Video; // type captured when the session began

    // Incremental grid build: entries are turned into cards a batch at a time on
    // an idle timer, so a big folder never freezes the UI on a tab/category switch.
    QFileInfoList m_buildQueue;
    int     m_buildPos = 0;
    QTimer* m_buildTimer = nullptr;
    bool    m_buildBusy = false;
    void    setBuildBusy(bool busy); // emit buildBusyChanged only on a real transition

    int m_targetWidth = 240;      // Settings target; cards grow to fill the row
    int m_cardWidth = 240;
};
