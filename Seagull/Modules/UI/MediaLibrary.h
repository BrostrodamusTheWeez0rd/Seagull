#pragma once

#include <QWidget>
#include <QUrl>
#include <QStringList>
#include <QHash>
#include <QPointer>

class QScrollArea;
class QPushButton;
class QButtonGroup;
class QFrame;
class QLabel;
class FlowLayout;
class VideoCard;
class SgThumbnailer;

// The "Library" tab: a card-grid view of the user's saved media, one content
// type at a time. The floating pill at the top switches between the four
// SgPaths folders (Videos / Audio / Images / Recordings) — the buttons aren't
// filters, they literally select which folder is shown. Cards reuse the Search
// tab's VideoCard (Play only); thumbnails come from SgThumbnailer.
class MediaLibrary : public QWidget {
    Q_OBJECT

public:
    enum class MediaType { Video, Audio, Image, Recording };

    explicit MediaLibrary(QWidget* parent = nullptr);

    void setCardWidth(int targetWidth); // live from Settings -> Display "Card size"

signals:
    void playMediaRequested(const QUrl& url);

public slots:
    void playNextFile();   // auto-advance / skip, in displayed order
    void playPrevFile();
    void refresh();        // re-list the active folder

protected:
    void showEvent(QShowEvent* event) override;       // refresh on tab switch
    void resizeEvent(QResizeEvent* event) override;   // reposition the floating pill
    bool eventFilter(QObject* obj, QEvent* event) override; // viewport resize -> refit cards

private:
    void rebuild();                 // folder listing -> cards
    void clearCards();
    void applyCardWidth();
    int  fillCardWidth() const;
    QString folderForType() const;       // the SgPaths folder for the active type
    QStringList extensionsForType() const;
    void positionTypePill();

    MediaType m_type = MediaType::Video;

    QFrame*       typePill = nullptr;     // floating translucent type switcher
    QButtonGroup* typeGroup = nullptr;
    QScrollArea*  cardsArea = nullptr;
    QWidget*      cardsHost = nullptr;
    FlowLayout*   cardsFlow = nullptr;
    QLabel*       emptyLabel = nullptr;   // centered "nothing here yet" note

    SgThumbnailer* thumbnailer = nullptr;
    QHash<QString, QPointer<VideoCard>> m_pendingThumbs; // file path -> its card

    QStringList m_files;          // displayed order (newest first)
    int m_currentPlayIndex = -1;

    int m_targetWidth = 240;      // Settings target; cards grow to fill the row
    int m_cardWidth = 240;
};
