#ifndef LIBRARY_H
#define LIBRARY_H

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
#include <QMenu>
#include <QInputDialog>
#include <QDir>
#include <QUrl>
#include <QList>
#include <QPoint>
#include <QAction>
#include <QTimer>
#include <QFileInfo>

class FolderOnlyFilter : public QSortFilterProxyModel {
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

class MediaFilterModel : public QSortFilterProxyModel {
public:
    // FIX 1: We need to know which folder the user clicked on
    void setCurrentRootPath(const QString& path) {
        currentRootPath = path;
        invalidateFilter(); // Force the table to refresh with the new rule
    }

private:
    QString currentRootPath;

protected:
    bool filterAcceptsRow(int sourceRow, const QModelIndex& sourceParent) const override {
        auto* fsModel = qobject_cast<QFileSystemModel*>(sourceModel());
        if (!fsModel) return true;

        QModelIndex index = fsModel->index(sourceRow, 0, sourceParent);
        if (!index.isValid()) return false;

        // FIX 2: We must accept the parent directory we are currently navigating into!
        // However, we reject all SUB-directories so they don't show up in the file list.
        if (fsModel->isDir(index)) {
            QString path = fsModel->filePath(index);
            // Accept ONLY if this folder is our target root (or its parent)
            return currentRootPath.startsWith(path);
        }

        QString path = fsModel->filePath(index).toLower();

        bool isMedia =
            path.endsWith(".mp4") ||
            path.endsWith(".mkv") ||
            path.endsWith(".avi") ||
            path.endsWith(".mp3") ||
            path.endsWith(".wav") ||
            path.endsWith(".flac") ||
            path.endsWith(".jpg") ||
            path.endsWith(".png");

        if (!isMedia) return false;

        QString filter = filterRegularExpression().pattern();
        if (filter.isEmpty()) return true;

        return fsModel->fileName(index).contains(filter, Qt::CaseInsensitive);
    }
};

class Library : public QWidget {
    Q_OBJECT
public:
    explicit Library(QWidget* parent = nullptr);

signals:
    void playMediaRequested(const QUrl& url);

private slots:
    void updateAddressBar(const QModelIndex& index);
    void createNewFolder();
    void refreshLibrary();
    void onFileDoubleClicked(const QModelIndex& index);
    void onTreeClicked(const QModelIndex& index);
    void onTreeDoubleClicked(const QModelIndex& index);
    void goBack();
    void goForward();
    void goUp();
    void showContextMenu(const QPoint& pos);
    void updateSearch(const QString& text);

private:
    void navigateTo(const QString& path, bool recordHistory = true);
    void setTableRootSafe(const QModelIndex& sourceIndex);

private:
    QVBoxLayout* mainLayout;
    QHBoxLayout* toolbarLayout;

    QPushButton* backBtn;
    QPushButton* fwdBtn;
    QPushButton* upBtn;
    QPushButton* refreshBtn;
    QPushButton* plusFolderBtn;

    QLineEdit* addressBar;
    QLineEdit* searchBar;

    QSplitter* mainSplitter;
    QTreeView* folderTree;
    QTableView* fileTable;

    QFileSystemModel* fileModel;
    FolderOnlyFilter* treeFilter;
    MediaFilterModel* tableFilter;

    QList<QString> history;
    int historyIndex;
};
#endif