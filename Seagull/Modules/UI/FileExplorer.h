#ifndef FILEEXPLORER_H
#define FILEEXPLORER_H

#include <QWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QTreeView>
#include <QTableView>
#include <QFileSystemModel>
#include <QSortFilterProxyModel>
#include <QSplitter>
#include <QPushButton>
#include <QLineEdit>
#include <QComboBox>
#include <QMenu>
#include <QInputDialog>
#include <QDir>
#include <QUrl>
#include <QList>
#include <QPoint>
#include <QAction>
#include <QTimer>
#include <QFileInfo>
#include <QDateTime>
#include <QLabel>
#include <QTableWidget>
#include <QProcess>

// -------------------------
// Folder-only filter
// -------------------------
class FolderOnlyFilter : public QSortFilterProxyModel {
    Q_OBJECT
public:
    explicit FolderOnlyFilter(QObject* parent = nullptr)
        : QSortFilterProxyModel(parent) {
    }

protected:
    bool filterAcceptsRow(int sourceRow, const QModelIndex& sourceParent) const override {
        auto* fsModel = qobject_cast<QFileSystemModel*>(sourceModel());
        if (!fsModel) return true;

        QModelIndex index = fsModel->index(sourceRow, 0, sourceParent);
        if (!index.isValid()) return false;

        if (!fsModel->isDir(index)) return false;

        QString filter = filterRegularExpression().pattern();
        if (filter.isEmpty()) return true;

        return fsModel->fileName(index).contains(filter, Qt::CaseInsensitive);
    }
};

// -------------------------
// Media filter
// -------------------------
class MediaFilterModel : public QSortFilterProxyModel {
    Q_OBJECT

public:
    explicit MediaFilterModel(QObject* parent = nullptr)
        : QSortFilterProxyModel(parent) {
    }

    void setCurrentRootPath(const QString& path) {
        currentRootPath = path;
        invalidateFilter();
    }

    // When true the table shows every file (still no folders), not just media.
    void setShowAllFiles(bool showAll) {
        m_showAllFiles = showAll;
        invalidateFilter();
    }

    // Sequential 1..N row numbers in the vertical header. The default proxy
    // headerData maps the vertical section back to the source model, which can't
    // be done over a tree+rootIndex (it returns 0 for every row past the first),
    // so we number by the proxy's own display order instead.
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override {
        if (orientation == Qt::Vertical) {
            // Handle every vertical role here — don't delegate to the base proxy,
            // which maps the section to the source model. That mapping succeeds for
            // row 0 but fails for the rest, so row 0 would pick up the source's
            // alignment/size while the others fall back to defaults, leaving the
            // "1" visually out of line with 2, 3, 4…
            if (role == Qt::DisplayRole)       return section + 1;
            if (role == Qt::TextAlignmentRole) return static_cast<int>(Qt::AlignCenter);
            return QVariant();
        }
        return QSortFilterProxyModel::headerData(section, orientation, role);
    }

protected:
    // Header-click sorting that respects each column's real data: numeric size,
    // chronological date, case-insensitive name. Other columns (Type) fall back to
    // the default display-string compare.
    bool lessThan(const QModelIndex& left, const QModelIndex& right) const override {
        if (auto* fs = qobject_cast<QFileSystemModel*>(sourceModel())) {
            const QFileInfo li = fs->fileInfo(left);
            const QFileInfo ri = fs->fileInfo(right);
            switch (left.column()) {
            case 0: return li.fileName().compare(ri.fileName(), Qt::CaseInsensitive) < 0;
            case 1: if (li.size() != ri.size()) return li.size() < ri.size(); break;
            case 3: return li.lastModified() < ri.lastModified();
            default: break;
            }
        }
        return QSortFilterProxyModel::lessThan(left, right);
    }

    bool filterAcceptsRow(int sourceRow, const QModelIndex& sourceParent) const override {
        auto* fsModel = qobject_cast<QFileSystemModel*>(sourceModel());
        if (!fsModel) return true;

        QModelIndex index = fsModel->index(sourceRow, 0, sourceParent);
        if (!index.isValid()) return false;

        if (fsModel->isDir(index)) {
            QString path = fsModel->filePath(index);
            return currentRootPath.startsWith(path);
        }

        QString path = fsModel->filePath(index).toLower();

        if (!m_showAllFiles) {
            bool isMedia =
                path.endsWith(".mp4") ||
                path.endsWith(".mkv") ||
                path.endsWith(".avi") ||
                path.endsWith(".ts") ||
                path.endsWith(".webm") ||
                path.endsWith(".mov") ||
                path.endsWith(".mp3") ||
                path.endsWith(".m4a") ||
                path.endsWith(".opus") ||
                path.endsWith(".wav") ||
                path.endsWith(".flac") ||
                path.endsWith(".jpg") ||
                path.endsWith(".png");

            if (!isMedia) return false;
        }

        QString filter = filterRegularExpression().pattern();
        if (filter.isEmpty()) return true;

        return fsModel->fileName(index).contains(filter, Qt::CaseInsensitive);
    }

private:
    QString currentRootPath;
    bool m_showAllFiles = false;
};

// -------------------------
// FileExplorer widget
// -------------------------
class FileExplorer : public QWidget {
    Q_OBJECT

public:
    explicit FileExplorer(QWidget* parent = nullptr);

signals:
    void playMediaRequested(const QUrl& url);

public slots:
    void playNextFile();
    void playPrevFile();

private slots:
    void updateAddressBar(const QModelIndex& index);
    void createNewFolder();
    void refreshView();
    void onFileDoubleClicked(const QModelIndex& index);
    void onTreeClicked(const QModelIndex& index);
    void onTreeDoubleClicked(const QModelIndex& index);
    void goBack();
    void goForward();
    void goUp();
    void showContextMenu(const QPoint& pos);
    void showFolderContextMenu(const QPoint& pos);
    void updateSearch(const QString& text);
    void onFileSelectionChanged(const QModelIndex& current, const QModelIndex& previous);

private:
    void navigateTo(const QString& path, bool recordHistory = true);
    void setTableRootSafe(const QModelIndex& sourceIndex);
    void addToAddressHistory(const QString& path);

    // Clipboard file operations (context menu + Ctrl+X/C/V, Del, F2). Copy/cut put
    // file URLs on the clipboard in Explorer's format, so cut/copy/paste interops
    // with Explorer in both directions.
    void cutCopySelection(bool cut);
    void pasteClipboard();
    void deleteSelection();   // to the Recycle Bin, after confirmation
    void renameSelected();    // inline edit; QFileSystemModel::setData does the rename
    QStringList selectedFilePaths() const;
    static bool copyPath(const QString& src, const QString& dst); // recursive for dirs
    static QString uniqueDestPath(const QDir& dir, const QString& name);

    // File-details panel (cover/thumbnail + metadata) to the right of the file table.
    void showDetailsFor(const QString& path);
    void clearDetails();
    void addDetailRow(const QString& key, const QString& value);
    void setCover(const QPixmap& pm);

private:
    QVBoxLayout* mainLayout = nullptr;
    QHBoxLayout* toolbarLayout = nullptr;

    QPushButton* backBtn = nullptr;
    QPushButton* fwdBtn = nullptr;
    QPushButton* upBtn = nullptr;
    QPushButton* refreshBtn = nullptr;
    QPushButton* filterBtn = nullptr;   // toggle: media-only vs all files
    QPushButton* goBtn = nullptr;

    QComboBox* addressBar = nullptr;
    QLineEdit* searchBar = nullptr;

    QSplitter* mainSplitter = nullptr;
    QTreeView* folderTree = nullptr;
    QTableView* fileTable = nullptr;

    // Details panel widgets + the async tools that feed them
    QWidget* detailsPanel = nullptr;
    QLabel* coverLabel = nullptr;
    QTableWidget* detailsTable = nullptr;
    QProcess* probeProc = nullptr;   // ffprobe — metadata
    QProcess* coverProc = nullptr;   // ffmpeg — cover/thumbnail
    QString m_detailPath;            // file currently shown (guards stale async results)

    // Shortcut-bearing actions, shared between the table's shortcuts and the
    // context menu (so the menu shows the key hints).
    QAction* actCut = nullptr;
    QAction* actCopy = nullptr;
    QAction* actPaste = nullptr;
    QAction* actDelete = nullptr;
    QAction* actRename = nullptr;

    QFileSystemModel* fileModel = nullptr;
    FolderOnlyFilter* treeFilter = nullptr;
    MediaFilterModel* tableFilter = nullptr;

    QList<QString> history;
    int historyIndex = -1;
    int currentPlayIndex = -1;
};

#endif