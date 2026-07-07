#ifndef DOWNLOADROW_H
#define DOWNLOADROW_H

#include <QFrame>
#include <QString>
#include "../../Backend/SgDownloadHistory.h" // Record + Status enum

class QLabel;
class QProgressBar;
class QPushButton;
class QNetworkAccessManager;
class MarqueeLabel;

// One row in the Download Manager's list: thumbnail + title, then an info line with
// status, file type, speed/ETA while downloading, and the date/time it was queued or
// finished, plus per-status action buttons (Restart / Cancel / Remove / Open folder).
// A self-contained themed QFrame, mirroring VideoCard's structure but laid out
// horizontally. The manager rebuilds these from SgDownloadHistory on structural changes and
// pushes live progress into the active row via setLiveProgress (no rebuild per tick).
class DownloadRow : public QFrame {
    Q_OBJECT
public:
    DownloadRow(const SgDownloadHistory::Record& rec, QNetworkAccessManager* nam,
                QWidget* parent = nullptr);

    // Live update for the currently-downloading row: fills the bar and shows speed/ETA.
    void setLiveProgress(double percent, const QString& speed, const QString& eta);

signals:
    // All carry the record's unique id (NOT the page URL — one URL can be several rows).
    void restartRequested(const QString& id);
    void cancelRequested(const QString& id);
    void removeRequested(const QString& id);
    void openFolderRequested(const QString& filePath);

protected:
    void enterEvent(QEnterEvent* event) override; // row hover arms the title marquee
    void leaveEvent(QEvent* event) override;

private:
    void loadThumbnail(QNetworkAccessManager* nam);
    void applyStatus(int status); // status text/colour + which widgets/buttons show

    QString m_id;       // the record's unique key
    QString m_thumbUrl;
    QString m_filePath;
    int     m_status = SgDownloadHistory::Queued;

    QLabel*       m_thumb  = nullptr;
    MarqueeLabel* m_title  = nullptr; // elides at rest, marquees on row hover
    QLabel*       m_statusLabel = nullptr;
    QLabel*       m_meta   = nullptr; // speed / ETA (downloading) or site (otherwise)
    QProgressBar* m_bar    = nullptr;
    QPushButton*  m_restartBtn = nullptr;
    QPushButton*  m_cancelBtn  = nullptr;
    QPushButton*  m_removeBtn  = nullptr;
    QPushButton*  m_openBtn    = nullptr;
};

#endif // DOWNLOADROW_H
