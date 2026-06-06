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

protected:
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

private:
    QString currentRootPath;
};

// -------------------------
// Library widget
// -------------------------
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
    QVBoxLayout* mainLayout = nullptr;
    QHBoxLayout* toolbarLayout = nullptr;

    QPushButton* backBtn = nullptr;
    QPushButton* fwdBtn = nullptr;
    QPushButton* upBtn = nullptr;
    QPushButton* refreshBtn = nullptr;
    QPushButton* plusFolderBtn = nullptr;

    QLineEdit* addressBar = nullptr;
    QLineEdit* searchBar = nullptr;

    QSplitter* mainSplitter = nullptr;
    QTreeView* folderTree = nullptr;
    QTableView* fileTable = nullptr;

    QFileSystemModel* fileModel = nullptr;
    FolderOnlyFilter* treeFilter = nullptr;
    MediaFilterModel* tableFilter = nullptr;

    QList<QString> history;
    int historyIndex = -1;
};

#endif