#ifndef DOWNLOADROW_H
#define DOWNLOADROW_H

#include <QFrame>
#include <QString>
#include "../../Backend/SgDownloadHistory.h" // Record + Status enum

class QLabel;
class QProgressBar;
class QPushButton;
class QNetworkAccessManager;

// One row in the Download Manager's list: thumbnail + title + status + (while downloading)
// a progress bar and speed/ETA, plus per-status action buttons (Restart / Cancel / Remove /
// Open folder). A self-contained themed QFrame, mirroring VideoCard's structure but laid out
// horizontally. The manager rebuilds these from SgDownloadHistory on structural changes and
// pushes live progress into the active row via setLiveProgress (no rebuild per tick).
class DownloadRow : public QFrame {
    Q_OBJECT
public:
    DownloadRow(const SgDownloadHistory::Record& rec, QNetworkAccessManager* nam,
                QWidget* parent = nullptr);

    QString pageUrl() const { return m_pageUrl; }

    // Live update for the currently-downloading row: fills the bar and shows speed/ETA.
    void setLiveProgress(double percent, const QString& speed, const QString& eta);

signals:
    void restartRequested(const QString& pageUrl);
    void cancelRequested(const QString& pageUrl);
    void removeRequested(const QString& pageUrl);
    void openFolderRequested(const QString& filePath);

private:
    void loadThumbnail(QNetworkAccessManager* nam);
    void applyStatus(int status); // status text/colour + which widgets/buttons show

    QString m_pageUrl;
    QString m_thumbUrl;
    QString m_filePath;
    int     m_status = SgDownloadHistory::Queued;

    QLabel*       m_thumb  = nullptr;
    QLabel*       m_title  = nullptr;
    QLabel*       m_statusLabel = nullptr;
    QLabel*       m_meta   = nullptr; // speed / ETA (downloading) or site (otherwise)
    QProgressBar* m_bar    = nullptr;
    QPushButton*  m_restartBtn = nullptr;
    QPushButton*  m_cancelBtn  = nullptr;
    QPushButton*  m_removeBtn  = nullptr;
    QPushButton*  m_openBtn    = nullptr;
};

#endif // DOWNLOADROW_H
