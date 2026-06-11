#include "Library.h"
#include <QTimer>
#include <QMenu>
#include <QInputDialog>
#include <QDir>
#include <QRegularExpression>
#include <QFileInfo>
#include <QSettings>
#include <QCoreApplication>
#include <QHeaderView>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QPixmap>
#include <QFile>
#include <QMap>
#include <QItemSelectionModel>
#include <QClipboard>
#include <QGuiApplication>
#include <QApplication>
#include <QMimeData>
#include <QMessageBox>

Library::Library(QWidget* parent) : QWidget(parent) {
    mainLayout = new QVBoxLayout(this);
    toolbarLayout = new QHBoxLayout();

    backBtn = new QPushButton("←");
    fwdBtn = new QPushButton("→");
    upBtn = new QPushButton("↑");
    refreshBtn = new QPushButton("↻");

    // Toggle between media-only and all files (checked = media only, the default).
    filterBtn = new QPushButton("Media Only");
    filterBtn->setCheckable(true);
    filterBtn->setChecked(true);
    filterBtn->setToolTip("Toggle showing only media files or all files");

    addressBar = new QLineEdit();
    searchBar = new QLineEdit();
    searchBar->setPlaceholderText("Search...");
    searchBar->setFixedWidth(150);

    toolbarLayout->addWidget(backBtn);
    toolbarLayout->addWidget(fwdBtn);
    toolbarLayout->addWidget(upBtn);
    toolbarLayout->addWidget(refreshBtn);
    toolbarLayout->addWidget(filterBtn);
    toolbarLayout->addWidget(addressBar);
    toolbarLayout->addWidget(searchBar);

    toolbarLayout->setStretchFactor(addressBar, 1);
    mainLayout->addLayout(toolbarLayout);

    fileModel = new QFileSystemModel(this);
    fileModel->setRootPath(QDir::rootPath());

    fileModel->setFilter(QDir::AllDirs |
        QDir::Files |
        QDir::NoDotAndDotDot |
        QDir::Drives);

    treeFilter = new FolderOnlyFilter();
    treeFilter->setSourceModel(fileModel);

    tableFilter = new MediaFilterModel();
    tableFilter->setSourceModel(fileModel);

    treeFilter->setRecursiveFilteringEnabled(true);
    tableFilter->setRecursiveFilteringEnabled(false);

    mainSplitter = new QSplitter(Qt::Horizontal);

    folderTree = new QTreeView();
    folderTree->setModel(treeFilter);
    folderTree->setColumnHidden(1, true);
    folderTree->setColumnHidden(2, true);
    folderTree->setColumnHidden(3, true);
    folderTree->setContextMenuPolicy(Qt::CustomContextMenu);

    fileTable = new QTableView();
    fileTable->setModel(tableFilter);
    fileTable->setContextMenuPolicy(Qt::CustomContextMenu);
    fileTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    fileTable->setSelectionMode(QAbstractItemView::ExtendedSelection); // copy/delete many at once
    // Writable model is what lets Rename work (QFileSystemModel::setData renames the
    // file on disk); NoEditTriggers keeps double-click as Play — editing only starts
    // programmatically from the Rename action.
    fileModel->setReadOnly(false);
    fileTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    folderTree->setEditTriggers(QAbstractItemView::NoEditTriggers);
    // Click a column header to sort by it (Name/Size/Type/Date), via lessThan.
    fileTable->setSortingEnabled(true);
    fileTable->sortByColumn(0, Qt::AscendingOrder);

    // --- File details panel (cover/thumbnail + metadata), right of the file table ---
    detailsPanel = new QWidget();
    auto* detailsLayout = new QVBoxLayout(detailsPanel);
    detailsLayout->setContentsMargins(8, 8, 8, 8);

    coverLabel = new QLabel();
    coverLabel->setAlignment(Qt::AlignCenter);
    coverLabel->setMinimumHeight(160);
    detailsLayout->addWidget(coverLabel);

    detailsTable = new QTableWidget(0, 2);
    detailsTable->horizontalHeader()->setVisible(false);
    detailsTable->verticalHeader()->setVisible(false);
    detailsTable->setShowGrid(false);
    detailsTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    detailsTable->setSelectionMode(QAbstractItemView::NoSelection);
    detailsTable->setFocusPolicy(Qt::NoFocus);
    detailsTable->setWordWrap(true);
    detailsTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    detailsTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    detailsLayout->addWidget(detailsTable);

    mainSplitter->addWidget(folderTree);
    mainSplitter->addWidget(fileTable);
    mainSplitter->addWidget(detailsPanel);
    mainSplitter->setSizes({ 250, 500, 250 });

    mainLayout->addWidget(mainSplitter);

    // ffprobe (metadata) + ffmpeg (cover) run asynchronously per selection.
    probeProc = new QProcess(this);
    coverProc = new QProcess(this);

    historyIndex = -1;

    connect(backBtn, &QPushButton::clicked, this, &Library::goBack);
    connect(fwdBtn, &QPushButton::clicked, this, &Library::goForward);
    connect(upBtn, &QPushButton::clicked, this, &Library::goUp);
    connect(refreshBtn, &QPushButton::clicked, this, &Library::refreshLibrary);
    connect(filterBtn, &QPushButton::toggled, this, [this](bool mediaOnly) {
        tableFilter->setShowAllFiles(!mediaOnly);
        filterBtn->setText(mediaOnly ? "Media Only" : "All Files");
        });

    connect(searchBar, &QLineEdit::textChanged, this, &Library::updateSearch);
    connect(folderTree, &QTreeView::clicked, this, &Library::onTreeClicked);
    connect(folderTree, &QTreeView::doubleClicked, this, &Library::onTreeDoubleClicked);
    connect(folderTree, &QTreeView::customContextMenuRequested, this, &Library::showFolderContextMenu);
    connect(fileTable, &QTableView::doubleClicked, this, &Library::onFileDoubleClicked);
    connect(fileTable, &QTableView::customContextMenuRequested, this, &Library::showContextMenu);
    connect(fileTable->selectionModel(), &QItemSelectionModel::currentRowChanged,
        this, &Library::onFileSelectionChanged);

    // File-operation actions: live on the table (shortcuts active while it has focus)
    // and reused by the context menu so the key hints show there.
    auto makeFileAction = [this](const QString& text, const QKeySequence& seq, auto&& fn) {
        auto* a = new QAction(text, this);
        a->setShortcut(seq);
        a->setShortcutContext(Qt::WidgetWithChildrenShortcut);
        connect(a, &QAction::triggered, this, std::forward<decltype(fn)>(fn));
        fileTable->addAction(a);
        return a;
    };
    actCut    = makeFileAction("Cut",    QKeySequence::Cut,    [this]() { cutCopySelection(true); });
    actCopy   = makeFileAction("Copy",   QKeySequence::Copy,   [this]() { cutCopySelection(false); });
    actPaste  = makeFileAction("Paste",  QKeySequence::Paste,  [this]() { pasteClipboard(); });
    actDelete = makeFileAction("Delete", QKeySequence::Delete, [this]() { deleteSelection(); });
    actRename = makeFileAction("Rename", QKeySequence(Qt::Key_F2), [this]() { renameSelected(); });

    // ffmpeg cover/thumbnail finished -> load it (ignoring results for a stale selection).
    connect(coverProc, &QProcess::finished, this, [this](int, QProcess::ExitStatus) {
        if (coverProc->property("path").toString() != m_detailPath) return;
        QPixmap pm(QDir::tempPath() + "/seagull_cover.jpg");
        if (!pm.isNull()) setCover(pm);
        else { coverLabel->setPixmap(QPixmap()); coverLabel->setText("No preview"); }
        });

    // ffprobe metadata finished -> append the rich rows.
    connect(probeProc, &QProcess::finished, this, [this](int, QProcess::ExitStatus) {
        if (probeProc->property("path").toString() != m_detailPath) return;
        QJsonDocument doc = QJsonDocument::fromJson(probeProc->readAllStandardOutput());
        if (!doc.isObject()) return;
        QJsonObject root = doc.object();
        QJsonObject fmt = root["format"].toObject();

        double dur = fmt["duration"].toString().toDouble();
        if (dur > 0) {
            int total = static_cast<int>(dur);
            int h = total / 3600, m = (total % 3600) / 60, s = total % 60;
            addDetailRow("Duration", h > 0
                ? QString("%1:%2:%3").arg(h).arg(m, 2, 10, QChar('0')).arg(s, 2, 10, QChar('0'))
                : QString("%1:%2").arg(m).arg(s, 2, 10, QChar('0')));
        }

        // Tag keys vary in case by container — fold them to lower case.
        QMap<QString, QString> tags;
        QJsonObject rawTags = fmt["tags"].toObject();
        for (auto it = rawTags.begin(); it != rawTags.end(); ++it)
            tags[it.key().toLower()] = it.value().toString();
        addDetailRow("Title", tags.value("title"));
        addDetailRow("Artist", tags.value("artist"));
        addDetailRow("Album", tags.value("album"));
        addDetailRow("Genre", tags.value("genre"));
        addDetailRow("Year", tags.value("date"));

        QString vcodec, acodec, res;
        for (const auto& sv : root["streams"].toArray()) {
            QJsonObject s = sv.toObject();
            QString type = s["codec_type"].toString();
            if (type == "video" && vcodec.isEmpty()) {
                vcodec = s["codec_name"].toString();
                int w = s["width"].toInt(), hgt = s["height"].toInt();
                if (w > 0 && hgt > 0) res = QString("%1 x %2").arg(w).arg(hgt);
            }
            else if (type == "audio" && acodec.isEmpty()) {
                acodec = s["codec_name"].toString();
            }
        }
        addDetailRow("Resolution", res);
        addDetailRow("Video", vcodec.toUpper());
        addDetailRow("Audio", acodec.toUpper());

        qint64 br = fmt["bit_rate"].toString().toLongLong();
        if (br > 0) addDetailRow("Bitrate", QString::number(br / 1000) + " kbps");
        });

    QTimer::singleShot(0, this, [this]() {
        // Start at the configured Home Folder (Settings), falling back to the OS home.
        QSettings settings(QCoreApplication::applicationDirPath() + "/config.ini", QSettings::IniFormat);
        QString home = settings.value("Paths/HomeFolder", QDir::homePath()).toString();
        if (home.isEmpty() || !QDir(home).exists()) home = QDir::homePath();
        navigateTo(home);
        });
}

void Library::navigateTo(const QString& path, bool recordHistory) {
    if (path.isEmpty())
        return;

    QModelIndex srcIdx = fileModel->index(path);
    if (!srcIdx.isValid())
        return;

    QModelIndex treeProxy = treeFilter->mapFromSource(srcIdx);
    folderTree->setCurrentIndex(treeProxy);

    setTableRootSafe(srcIdx);
    addressBar->setText(path);

    if (recordHistory) {
        while (history.size() > historyIndex + 1)
            history.removeLast();

        history.append(path);
        historyIndex++;
    }
}

void Library::setTableRootSafe(const QModelIndex& sourceIndex) {
    if (!sourceIndex.isValid())
        return;

    QString path = fileModel->filePath(sourceIndex);
    if (path.isEmpty())
        return;

    tableFilter->setCurrentRootPath(path);

    QModelIndex proxyIdx = tableFilter->mapFromSource(sourceIndex);
    if (proxyIdx.isValid())
        fileTable->setRootIndex(proxyIdx);
}

void Library::onTreeClicked(const QModelIndex& index) {
    QModelIndex srcIdx = treeFilter->mapToSource(index);
    if (!srcIdx.isValid())
        return;

    navigateTo(fileModel->filePath(srcIdx));
}

void Library::onTreeDoubleClicked(const QModelIndex& index) {
    QModelIndex srcIdx = treeFilter->mapToSource(index);
    if (!srcIdx.isValid())
        return;

    navigateTo(fileModel->filePath(srcIdx));
}

void Library::onFileDoubleClicked(const QModelIndex& index) {
    QModelIndex srcIdx = tableFilter->mapToSource(index);
    if (!srcIdx.isValid())
        return;

    QString path = fileModel->filePath(srcIdx);
    if (path.isEmpty())
        return;

    QFileInfo info(path);

    // CRITICAL FIX: prevent directories or invalid files being emitted
    if (!info.exists() || !info.isFile())
        return;

    QUrl url = QUrl::fromLocalFile(info.absoluteFilePath());

    // extra safety: prevent garbage URLs
    if (!url.isValid() || url.isEmpty())
        return;

    // Track the row in the proxy model so next/prev can navigate from it
    currentPlayIndex = index.row();

    emit playMediaRequested(url);
}

void Library::playNextFile() {
    if (currentPlayIndex < 0) return;
    int nextRow = currentPlayIndex + 1;
    if (nextRow >= tableFilter->rowCount(fileTable->rootIndex())) return;
    QModelIndex proxyIdx = tableFilter->index(nextRow, 0, fileTable->rootIndex());
    if (proxyIdx.isValid())
        onFileDoubleClicked(proxyIdx);
}

void Library::playPrevFile() {
    if (currentPlayIndex <= 0) return;
    int prevRow = currentPlayIndex - 1;
    QModelIndex proxyIdx = tableFilter->index(prevRow, 0, fileTable->rootIndex());
    if (proxyIdx.isValid())
        onFileDoubleClicked(proxyIdx);
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
    if (dir.cdUp())
        navigateTo(dir.absolutePath());
}

void Library::refreshLibrary() {
    QModelIndex srcIdx = fileModel->index(addressBar->text());
    if (srcIdx.isValid())
        fileModel->fetchMore(srcIdx);
}

void Library::createNewFolder() {
    QString currentPath = addressBar->text();
    if (currentPath.isEmpty())
        return;

    bool ok;
    QString folderName = QInputDialog::getText(
        this,
        "New Folder",
        "Folder Name:",
        QLineEdit::Normal,
        "",
        &ok
    );

    if (ok && !folderName.isEmpty()) {
        QDir dir(currentPath);
        dir.mkdir(folderName);
    }
}

void Library::updateSearch(const QString& text) {
    tableFilter->setFilterRegularExpression(
        QRegularExpression(text, QRegularExpression::CaseInsensitiveOption)
    );
}

void Library::showContextMenu(const QPoint& pos) {
    const QModelIndex index = fileTable->indexAt(pos);
    QMenu menu(this);

    const QMimeData* clip = QGuiApplication::clipboard()->mimeData();
    actPaste->setEnabled(clip && clip->hasUrls());

    if (index.isValid()) {
        // Right-click selects what's under the cursor (like Explorer) — but leave a
        // multi-selection alone so cut/copy/delete can act on all of it.
        if (!fileTable->selectionModel()->isSelected(index))
            fileTable->setCurrentIndex(index);

        const QModelIndex src = tableFilter->mapToSource(index);
        const QFileInfo info = fileModel->fileInfo(src);
        const QString path = info.absoluteFilePath();

        if (info.isDir())
            menu.addAction("Open", this, [this, path]() { navigateTo(path); });
        else
            menu.addAction("Play", this, [this, index]() { onFileDoubleClicked(index); });
        menu.addSeparator();
        menu.addAction(actCut);
        menu.addAction(actCopy);
        menu.addAction(actPaste);
        menu.addSeparator();
        menu.addAction(actRename);
        menu.addAction(actDelete);
        menu.addSeparator();
        menu.addAction("Show in Explorer", this, [path]() {
            QProcess::startDetached("explorer.exe",
                { "/select,", QDir::toNativeSeparators(path) });
            });
        menu.addAction("Copy Path", this, [path]() {
            QGuiApplication::clipboard()->setText(QDir::toNativeSeparators(path));
            });
    } else {
        // Empty area: folder-level operations.
        menu.addAction(actPaste);
        menu.addSeparator();
        menu.addAction("New Folder", this, &Library::createNewFolder);
        menu.addAction("Refresh", this, &Library::refreshLibrary);
        menu.addSeparator();
        menu.addAction("Open Folder in Explorer", this, [this]() {
            QProcess::startDetached("explorer.exe",
                { QDir::toNativeSeparators(addressBar->text()) });
            });
    }

    menu.exec(fileTable->viewport()->mapToGlobal(pos));
    actPaste->setEnabled(true); // restore for the Ctrl+V shortcut (paste no-ops itself)
}

void Library::showFolderContextMenu(const QPoint& pos) {
    const QModelIndex index = folderTree->indexAt(pos);
    QMenu menu(this);

    if (index.isValid()) {
        const QModelIndex srcIdx = treeFilter->mapToSource(index);
        const QString path = fileModel->filePath(srcIdx);

        menu.addAction("Open", this, [this, path]() { navigateTo(path); });
        menu.addSeparator();
        menu.addAction("New Folder", this, [this, path]() {
            bool ok;
            QString name = QInputDialog::getText(this, "New Folder", "Folder Name:",
                QLineEdit::Normal, "", &ok);
            if (ok && !name.isEmpty())
                QDir(path).mkdir(name);
        });
        menu.addSeparator();
        menu.addAction("Show in Explorer", this, [path]() {
            QProcess::startDetached("explorer.exe",
                { "/select,", QDir::toNativeSeparators(path) });
        });
        menu.addAction("Copy Path", this, [path]() {
            QGuiApplication::clipboard()->setText(QDir::toNativeSeparators(path));
        });
    } else {
        menu.addAction("New Folder", this, &Library::createNewFolder);
        menu.addAction("Refresh", this, &Library::refreshLibrary);
    }

    menu.exec(folderTree->viewport()->mapToGlobal(pos));
}

void Library::updateAddressBar(const QModelIndex& index) {
    QModelIndex srcIdx = treeFilter->mapToSource(index);
    if (!srcIdx.isValid())
        return;

    addressBar->setText(fileModel->filePath(srcIdx));
}

// ---------------------------------------------------------------------------
// File operations (context menu + shortcuts)
// ---------------------------------------------------------------------------

QStringList Library::selectedFilePaths() const {
    QStringList paths;
    const auto rows = fileTable->selectionModel()->selectedRows(0);
    for (const QModelIndex& proxyIdx : rows) {
        const QModelIndex src = tableFilter->mapToSource(proxyIdx);
        if (src.isValid()) paths << fileModel->filePath(src);
    }
    return paths;
}

void Library::cutCopySelection(bool cut) {
    const QStringList paths = selectedFilePaths();
    if (paths.isEmpty()) return;

    QList<QUrl> urls;
    for (const QString& p : paths) urls << QUrl::fromLocalFile(p);
    auto* mime = new QMimeData(); // clipboard takes ownership
    mime->setUrls(urls);
    // Explorer interop: the "Preferred DropEffect" DWORD marks the urls as a cut
    // (DROPEFFECT_MOVE = 2) or a copy (COPY|LINK = 5), in both directions — our cut
    // pastes as a move in Explorer, and Explorer's cut pastes as a move here.
    QByteArray effect(4, '\0');
    effect[0] = cut ? char(2) : char(5);
    mime->setData("Preferred DropEffect", effect);
    QGuiApplication::clipboard()->setMimeData(mime);
}

void Library::pasteClipboard() {
    const QMimeData* mime = QGuiApplication::clipboard()->mimeData();
    if (!mime || !mime->hasUrls()) return;
    const QDir destDir(addressBar->text());
    if (!destDir.exists()) return;

    const QByteArray fx = mime->data("Preferred DropEffect");
    const bool move = fx.size() >= 1 && (fx.at(0) & 2);

    QApplication::setOverrideCursor(Qt::WaitCursor);
    QStringList failed;
    for (const QUrl& u : mime->urls()) {
        if (!u.isLocalFile()) continue;
        const QString src = u.toLocalFile();
        const QFileInfo si(src);
        if (!si.exists()) continue;
        // Moving into the file's own folder is a no-op; a copy there duplicates.
        if (move && QString::compare(si.absolutePath(), destDir.absolutePath(), Qt::CaseInsensitive) == 0)
            continue;
        const QString dst = uniqueDestPath(destDir, si.fileName());
        bool ok;
        if (move) {
            ok = si.isDir() ? QDir().rename(src, dst) : QFile::rename(src, dst);
            if (!ok) { // rename fails across drives — fall back to copy + delete
                ok = copyPath(src, dst);
                if (ok) ok = si.isDir() ? QDir(src).removeRecursively() : QFile::remove(src);
            }
        } else {
            ok = copyPath(src, dst);
        }
        if (!ok) failed << si.fileName();
    }
    QApplication::restoreOverrideCursor();

    if (move) QGuiApplication::clipboard()->clear(); // a cut pastes once, like Explorer
    if (!failed.isEmpty())
        QMessageBox::warning(this, "Paste", "Could not paste:\n" + failed.join('\n'));
}

void Library::deleteSelection() {
    const QStringList paths = selectedFilePaths();
    if (paths.isEmpty()) return;

    const QString what = paths.size() == 1
        ? "\"" + QFileInfo(paths.first()).fileName() + "\""
        : QString("%1 items").arg(paths.size());
    if (QMessageBox::question(this, "Delete",
        "Move " + what + " to the Recycle Bin?") != QMessageBox::Yes)
        return;

    QStringList failed;
    for (const QString& p : paths)
        if (!QFile::moveToTrash(p)) failed << QFileInfo(p).fileName();
    if (!failed.isEmpty())
        QMessageBox::warning(this, "Delete", "Could not delete:\n" + failed.join('\n'));
}

void Library::renameSelected() {
    const QModelIndex cur = fileTable->currentIndex();
    if (!cur.isValid()) return;
    // In-place edit of the Name column; on commit QFileSystemModel renames on disk.
    fileTable->edit(cur.siblingAtColumn(0));
}

bool Library::copyPath(const QString& src, const QString& dst) {
    const QFileInfo si(src);
    if (si.isDir()) {
        if (!QDir().mkpath(dst)) return false;
        const auto entries = QDir(src).entryInfoList(
            QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden);
        for (const QFileInfo& e : entries)
            if (!copyPath(e.absoluteFilePath(), dst + "/" + e.fileName())) return false;
        return true;
    }
    return QFile::copy(src, dst);
}

QString Library::uniqueDestPath(const QDir& dir, const QString& name) {
    QString candidate = dir.filePath(name);
    if (!QFileInfo::exists(candidate)) return candidate;
    const QFileInfo fi(name);
    const QString base = fi.completeBaseName();
    const QString ext = fi.suffix().isEmpty() ? QString() : "." + fi.suffix();
    for (int n = 2;; ++n) { // "name (2).ext", like Explorer
        candidate = dir.filePath(QString("%1 (%2)%3").arg(base).arg(n).arg(ext));
        if (!QFileInfo::exists(candidate)) return candidate;
    }
}

// ---------------------------------------------------------------------------
// File details panel
// ---------------------------------------------------------------------------

void Library::onFileSelectionChanged(const QModelIndex& current, const QModelIndex&) {
    if (!current.isValid()) { clearDetails(); m_detailPath.clear(); return; }
    QModelIndex src = tableFilter->mapToSource(current);
    if (!src.isValid()) { clearDetails(); m_detailPath.clear(); return; }

    QString path = fileModel->filePath(src);
    QFileInfo info(path);
    if (!info.isFile()) { clearDetails(); m_detailPath.clear(); return; }

    showDetailsFor(path);
}

void Library::showDetailsFor(const QString& path) {
    m_detailPath = path;
    clearDetails();

    QFileInfo info(path);
    addDetailRow("Name", info.fileName());
    addDetailRow("Type", info.suffix().toUpper());
    addDetailRow("Size", QString::number(info.size() / 1048576.0, 'f', 2) + " MB");

    const QString ext = info.suffix().toLower();
    static const QStringList imageExts = { "jpg", "jpeg", "png", "bmp", "gif", "webp" };
    static const QStringList audioExts = { "mp3", "flac", "m4a", "wav", "aac", "opus", "ogg" };

    // --- Cover / thumbnail ---
    coverLabel->setPixmap(QPixmap());
    coverLabel->setText("Loading…");
    if (imageExts.contains(ext)) {
        QPixmap pm(path);
        if (!pm.isNull()) setCover(pm);
        else coverLabel->setText("No preview");
    }
    else {
        const QString ffmpeg = QCoreApplication::applicationDirPath() + "/tools/ffmpeg.exe";
        const QString coverOut = QDir::tempPath() + "/seagull_cover.jpg";
        QFile::remove(coverOut);

        const bool isAudio = audioExts.contains(ext);
        QStringList args;
        args << "-y";
        if (!isAudio) args << "-ss" << "3";              // a few seconds in for video
        args << "-i" << path << "-frames:v" << "1";
        if (isAudio) args << "-an";                      // grab the embedded cover art
        args << "-vf" << "scale='min(360,iw)':-1" << coverOut;

        if (coverProc->state() != QProcess::NotRunning) { coverProc->kill(); coverProc->waitForFinished(50); }
        coverProc->setProperty("path", path);
        coverProc->start(ffmpeg, args);
    }

    // --- Metadata via ffprobe ---
    const QString ffprobe = QCoreApplication::applicationDirPath() + "/tools/ffprobe.exe";
    QStringList pargs;
    pargs << "-v" << "quiet" << "-print_format" << "json"
        << "-show_format" << "-show_streams" << path;

    if (probeProc->state() != QProcess::NotRunning) { probeProc->kill(); probeProc->waitForFinished(50); }
    probeProc->setProperty("path", path);
    probeProc->start(ffprobe, pargs);
}

void Library::clearDetails() {
    if (detailsTable) detailsTable->setRowCount(0);
    if (coverLabel) { coverLabel->setPixmap(QPixmap()); coverLabel->setText(""); }
}

void Library::addDetailRow(const QString& key, const QString& value) {
    if (value.trimmed().isEmpty()) return;
    int r = detailsTable->rowCount();
    detailsTable->insertRow(r);
    auto* k = new QTableWidgetItem(key);
    k->setFlags(Qt::ItemIsEnabled);
    auto* v = new QTableWidgetItem(value);
    v->setFlags(Qt::ItemIsEnabled);
    detailsTable->setItem(r, 0, k);
    detailsTable->setItem(r, 1, v);
}

void Library::setCover(const QPixmap& pm) {
    coverLabel->setText("");
    int w = detailsPanel->width() - 24;
    if (w < 80) w = 200;
    coverLabel->setPixmap(pm.scaled(w, 240, Qt::KeepAspectRatio, Qt::SmoothTransformation));
}