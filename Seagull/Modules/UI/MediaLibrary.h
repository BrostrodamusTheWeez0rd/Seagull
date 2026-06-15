#pragma once

#include <QWidget>
#include <QUrl>
#include <QStringList>
#include <QHash>
#include <QPointer>

#include <QList>
#include <QPair>

class QScrollArea;
class QPushButton;
class QButtonGroup;
class QFrame;
class QLabel;
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

    explicit MediaLibrary(SgSpellCheck* spell, QWidget* parent = nullptr);

    void setCardWidth(int targetWidth); // live from Settings -> Display "Card size"

    // Thumbnail generation still running?
    bool thumbnailsBusy() const;

    // Hold/release the thumbnail ffmpeg queue. Startup holds it until the
    // update modal finishes so an ffmpeg.exe swap never races a running grab.
    void setThumbnailsHeld(bool held);

signals:
    void playMediaRequested(const QUrl& url);
    void playPlaylistRequested(const QString& path);       // .sgpl card -> Queue loads + plays it
    void enqueueLocalRequested(const QStringList& paths);  // card Queue button -> Queue tab

public slots:
    void playNextFile();   // auto-advance / skip, in displayed order
    void playPrevFile();
    void refresh();        // re-list the active folder

protected:
    void showEvent(QShowEvent* event) override;       // refresh on tab switch
    void hideEvent(QHideEvent* event) override;       // pause the pill hover poll
    void resizeEvent(QResizeEvent* event) override;   // reposition the floating pill
    void changeEvent(QEvent* event) override;         // re-tint the search icon on theme change
    bool eventFilter(QObject* obj, QEvent* event) override; // viewport resize -> refit cards

private:
    void rebuild();                 // folder listing -> cards
    void clearCards();
    void filterCards();             // hide cards not matching the search query
    void applyCardWidth();
    int  fillCardWidth() const;
    QString folderForType() const;       // the SgPaths folder for the active type
    QStringList extensionsForType() const;
    void positionTypePill();
    void updatePillVisibility(); // visible at scroll-top or when its strip is hovered
    void positionSearch();       // place the magnifier button / bar at the top-right
    void toggleSearch();         // magnifier click: reveal the search bar (or collapse)
    void tintSearchIcon();       // recolour the magnifier glyph to the theme text colour

    MediaType m_type = MediaType::Video;

    QFrame*       typePill = nullptr;     // floating translucent type switcher
    QButtonGroup* typeGroup = nullptr;
    QPushButton*  searchButton = nullptr;  // floating magnifier at the top-right
    SpellCheckLineEdit* librarySearch = nullptr; // revealed on click; filters the active type
    bool          m_searchOpen = false;    // is the search bar revealed?
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

    int m_targetWidth = 240;      // Settings target; cards grow to fill the row
    int m_cardWidth = 240;
};
