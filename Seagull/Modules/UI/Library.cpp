#include "Library.h"
#include <QTimer>
#include <QMenu>
#include <QInputDialog>
#include <QDir>
#include <QRegularExpression>

Library::Library(QWidget* parent) : QWidget(parent) {
    mainLayout = new QVBoxLayout(this);
    toolbarLayout = new QHBoxLayout();

    backBtn = new QPushButton("←");
    fwdBtn = new QPushButton("→");
    upBtn = new QPushButton("↑");
    refreshBtn = new QPushButton("↻");
    plusFolderBtn = new QPushButton("+ Folder");

    addressBar = new QLineEdit();
    searchBar = new QLineEdit();
    searchBar->setPlaceholderText("Search...");
    searchBar->setFixedWidth(150);

    toolbarLayout->addWidget(backBtn);
    toolbarLayout->addWidget(fwdBtn);
    toolbarLayout->addWidget(upBtn);
    toolbarLayout->addWidget(refreshBtn);
    toolbarLayout->addWidget(plusFolderBtn);
    toolbarLayout->addWidget(addressBar);
    toolbarLayout->addWidget(searchBar);

    toolbarLayout->setStretchFactor(addressBar, 1);
    mainLayout->addLayout(toolbarLayout);

    fileModel = new QFileSystemModel(this);

    // IMPORTANT: use rootPath, not ""
    fileModel->setRootPath(QDir::rootPath());

    fileModel->setFilter(QDir::AllDirs |
        QDir::Files |
        QDir::NoDotAndDotDot |
        QDir::Drives);

    treeFilter = new FolderOnlyFilter();
    treeFilter->setSourceModel(fileModel);

    tableFilter = new MediaFilterModel();
    tableFilter->setSourceModel(fileModel);

    // FIX 1: recursive filtering breaks QFileSystemModel stability here
    treeFilter->setRecursiveFilteringEnabled(true);
    tableFilter->setRecursiveFilteringEnabled(false);

    mainSplitter = new QSplitter(Qt::Horizontal);

    folderTree = new QTreeView();
    folderTree->setModel(treeFilter);
    folderTree->setColumnHidden(1, true);
    folderTree->setColumnHidden(2, true);
    folderTree->setColumnHidden(3, true);

    fileTable = new QTableView();
    fileTable->setModel(tableFilter);
    fileTable->setContextMenuPolicy(Qt::CustomContextMenu);

    mainSplitter->addWidget(folderTree);
    mainSplitter->addWidget(fileTable);
    mainSplitter->setSizes({ 250, 750 });
    mainLayout->addWidget(mainSplitter);

    historyIndex = -1;

    connect(backBtn, &QPushButton::clicked, this, &Library::goBack);
    connect(fwdBtn, &QPushButton::clicked, this, &Library::goForward);
    connect(upBtn, &QPushButton::clicked, this, &Library::goUp);
    connect(refreshBtn, &QPushButton::clicked, this, &Library::refreshLibrary);
    connect(plusFolderBtn, &QPushButton::clicked, this, &Library::createNewFolder);

    connect(searchBar, &QLineEdit::textChanged, this, &Library::updateSearch);
    connect(folderTree, &QTreeView::clicked, this, &Library::onTreeClicked);
    // Double clicking tree will also navigate
    connect(folderTree, &QTreeView::doubleClicked, this, &Library::onTreeDoubleClicked);
    connect(fileTable, &QTableView::doubleClicked, this, &Library::onFileDoubleClicked);
    connect(fileTable, &QTableView::customContextMenuRequested, this, &Library::showContextMenu);

    // FIX 2: delay initial navigation properly (prevents empty proxy state)
    QTimer::singleShot(0, this, [this]() {
        navigateTo(QDir::homePath());
        });
}

void Library::navigateTo(const QString& path, bool recordHistory) {
    QModelIndex srcIdx = fileModel->index(path);
    if (!srcIdx.isValid()) return;

    folderTree->setCurrentIndex(treeFilter->mapFromSource(srcIdx));
    setTableRootSafe(srcIdx);
    addressBar->setText(path);

    if (recordHistory) {
        while (history.size() > historyIndex + 1) {
            history.removeLast();
        }
        history.append(path);
        historyIndex++;
    }
}

void Library::setTableRootSafe(const QModelIndex& sourceIndex) {
    // FIX 3: Tell the proxy model exactly which directory we want to enter,
    // so it temporarily "allows" this directory to bypass the 'No Folders' rule!
    tableFilter->setCurrentRootPath(fileModel->filePath(sourceIndex));

    QModelIndex proxyIdx = tableFilter->mapFromSource(sourceIndex);
    fileTable->setRootIndex(proxyIdx);
}

void Library::onTreeClicked(const QModelIndex& index) {
    QModelIndex srcIdx = treeFilter->mapToSource(index);
    QString path = fileModel->filePath(srcIdx);
    navigateTo(path);
}

void Library::onTreeDoubleClicked(const QModelIndex& index) {
    QModelIndex srcIdx = treeFilter->mapToSource(index);
    QString path = fileModel->filePath(srcIdx);
    navigateTo(path);
}

void Library::onFileDoubleClicked(const QModelIndex& index) {
    QModelIndex srcIdx = tableFilter->mapToSource(index);
    QString path = fileModel->filePath(srcIdx);
    QFileInfo info(path);

    // Only emit play signal if it's a file, not a folder
    if (info.isFile()) {
        emit playMediaRequested(QUrl::fromLocalFile(path));
    }
}

void Library::goBack() {
    if (historyIndex > 0) {
        historyIndex--;
        navigateTo(history[historyIndex], false);
    }
}

void Library::goForward() {
    if (historyIndex < history.size() - 1) {
        historyIndex++;
        navigateTo(history[historyIndex], false);
    }
}

void Library::goUp() {
    QDir dir(addressBar->text());
    if (dir.cdUp()) {
        navigateTo(dir.absolutePath());
    }
}

void Library::refreshLibrary() {
    QModelIndex srcIdx = fileModel->index(addressBar->text());
    fileModel->fetchMore(srcIdx);
}

void Library::createNewFolder() {
    QString currentPath = addressBar->text();
    if (currentPath.isEmpty()) return;

    bool ok;
    QString folderName = QInputDialog::getText(this, "New Folder", "Folder Name:", QLineEdit::Normal, "", &ok);

    if (ok && !folderName.isEmpty()) {
        QDir dir(currentPath);
        dir.mkdir(folderName);
    }
}

void Library::updateSearch(const QString& text) {
    tableFilter->setFilterRegularExpression(QRegularExpression(text, QRegularExpression::CaseInsensitiveOption));
}

void Library::showContextMenu(const QPoint& pos) {
    QModelIndex index = fileTable->indexAt(pos);
    if (!index.isValid()) return;

    QMenu menu(this);
    QAction* playAction = menu.addAction("Play");
    QAction* selected = menu.exec(fileTable->viewport()->mapToGlobal(pos));

    if (selected == playAction) {
        onFileDoubleClicked(index);
    }
}

void Library::updateAddressBar(const QModelIndex& index) {
    QModelIndex srcIdx = treeFilter->mapToSource(index);
    addressBar->setText(fileModel->filePath(srcIdx));
}