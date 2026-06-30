#include "Seagull.h"
#include "Modules/UI/Theme.h"
#include "Modules/UI/SetupDialog.h"
#include "Modules/UI/Widgets/UpdateDialog.h"
#include "Modules/Backend/SgFavorites.h"
#include "Modules/Backend/SgPaths.h"
#include "Modules/Backend/SgThumbnailer.h"
#include <QApplication>
#include <QSettings>
#include <QCoreApplication>
#include <QTextBrowser>
#include <QTextCursor>
#include <QTimer>
#include <QScrollBar>
#include <QDialog>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QDialogButtonBox>
#include <QLabel>
#include <QFrame>
#include <QMovie>
#include <QWidget>
#include <QFont>
#include <QPushButton>
#include <QDesktopServices>
#include <QUrl>
#include <QMessageBox>
#include <QProgressDialog>
#include <QProcess>
#include <QDir>
#include <QStandardPaths>
#include <QFile>
#include <QDateTime>
#include <QLockFile>
#include <QEvent>
#include <QLocale>
#include <QStringList>
#include <QHash>
#include <QRandomGenerator>
#include <QSet>
#include <QPalette>
#include <iterator>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>

// Stamped in by the build (see CMakeLists). Fallback keeps a stray build compiling.
#ifndef SEAGULL_VERSION
#define SEAGULL_VERSION "dev"
#endif

Seagull::Seagull(QObject* parent) : QObject(parent) {
    // The window everything hangs off of — a shell that hosts the video player.
    mainWindow = new MainWindow();
    videoPlayer = new VideoPlayer();
    mainWindow->setVideoPlayer(videoPlayer);

    // Backend workers. Each one is a yt-dlp wrapper with a single job, so their
    // long-running processes never step on each other.
    downloaderWorker = new SgYtDlp(this);
    resolverWorker = new SgYtDlp(this);
    prefetcherWorker = new SgYtDlp(this);
    playerWorker = new SgYtDlp(this);
    downloadWorker = new SgYtDlp(this);
    shortsPrefetcher = new SgYtDlp(this);
    searchWorker = new SgSearch(this);

    // The comments worker runs the slow, paginated comment extraction + JSON parse on
    // its OWN thread so none of it touches the GUI thread (no parent — required for
    // moveToThread; its child QProcess moves across with it). Its commentsReady crosses
    // back to the GUI queued, so register its payload type for queued delivery.
    qRegisterMetaType<QJsonArray>("QJsonArray");
    commentsThread = new QThread(this);
    commentsWorker = new SgYtDlp(nullptr);
    commentsWorker->moveToThread(commentsThread);
    connect(commentsThread, &QThread::finished, commentsWorker, &QObject::deleteLater);
    commentsThread->start();

    // Shared Windows OS spell checker for the Search query combo + File Explorer
    // search box. Inert if the OS/language is unsupported (fields stay plain).
    spellChecker = new SgSpellCheck(this);

    // Shared ad-strip proxy for Twitch live streams. Every worker that resolves a
    // playable stream URL gets it, so the ad-free routing applies no matter which
    // path (player probe, queue prefetch, …) produced the URL.
    hlsProxy = new SgHlsProxy(this);
    downloaderWorker->setHlsProxy(hlsProxy);
    resolverWorker->setHlsProxy(hlsProxy);
    prefetcherWorker->setHlsProxy(hlsProxy);
    playerWorker->setHlsProxy(hlsProxy);
    shortsPrefetcher->setHlsProxy(hlsProxy);

    // One -J cache shared by every worker: a video the queue title-resolver already
    // extracted is answered from cache when the prefetcher and player ask for it,
    // instead of three separate yt-dlp launches against YouTube.
    metaCache = new SgMetaCache(this);
    for (SgYtDlp* w : { downloaderWorker, resolverWorker, prefetcherWorker, playerWorker, downloadWorker, shortsPrefetcher })
        w->setMetaCache(metaCache);

    // Records the currently-playing live stream (parallel ffmpeg), driven by the
    // player's Record button.
    recorder = new SgRecorder(this);

    // Windows media controls (SMTC): the OS now-playing overlay + media keys.
    // Bound to the main window's HWND in run() (after the window exists).
    mediaControls = new SgMediaControls(this);

    // App update check. At startup it runs FIRST (before the tool check): if the
    // user updates Seagull itself, the tool check is skipped and handled on the
    // fresh launch. The Settings button drives the same check manually.
    appUpdate = new SgAppUpdate(this);
    // The startup Seagull-version check is driven by the UpdateDialog now; these
    // handlers only serve the manual Settings "Check for Updates" path.
    connect(appUpdate, &SgAppUpdate::updateAvailable, this,
        [this](const QString& v, const QString& notes, const QString& url) {
            if (!m_appCheckManual) return; // startup is owned by the UpdateDialog
            m_appCheckManual = false;
            showAppUpdatePrompt(v, notes, url);
        });
    connect(appUpdate, &SgAppUpdate::upToDate, this, [this]() {
        if (!m_appCheckManual) return;
        m_appCheckManual = false;
        QMessageBox::information(mainWindow, "Seagull",
            QString("You're on the latest version (%1).").arg(QString::fromLatin1(SEAGULL_VERSION)));
    });
    connect(appUpdate, &SgAppUpdate::checkFailed, this, [this](const QString& reason) {
        if (!m_appCheckManual) return;
        m_appCheckManual = false;
        QMessageBox::warning(mainWindow, "Seagull",
            "Could not check for updates.\n\n" + reason);
    });
    // Self-update download/stage progress + completion.
    connect(appUpdate, &SgAppUpdate::downloadProgress, this, [this](qint64 got, qint64 total) {
        if (!m_updateProgress) return;
        if (total > 0) { m_updateProgress->setMaximum(100);
                         m_updateProgress->setValue(int(got * 100 / total)); }
    });
    connect(appUpdate, &SgAppUpdate::downloadFailed, this, [this](const QString& reason) {
        if (m_updateProgress) { m_updateProgress->close(); m_updateProgress->deleteLater(); m_updateProgress = nullptr; }
        QMessageBox::warning(mainWindow, "Update Failed",
            "Could not download the update.\n\n" + reason +
            "\n\nYou can still update manually from the releases page.");
        // A startup self-update that failed: fall back to running normally (the
        // tool check is intentionally skipped — it'll run next launch).
        if (m_selfUpdateFromStartup) { m_selfUpdateFromStartup = false; finishStartupUpdates(); }
    });
    connect(appUpdate, &SgAppUpdate::readyToApply, this, &Seagull::onUpdateReadyToApply);

    // The tab modules.
    libraryModule = new MediaLibrary(spellChecker);
    explorerModule = new FileExplorer(spellChecker);
    queueModule = new Queue(downloaderWorker, resolverWorker, prefetcherWorker);
    searchModule = new Search(searchWorker, spellChecker);
    settingsModule = new Settings();
    eqModule = new EQ();

    mainWindow->addTab(libraryModule, "Library");
    mainWindow->addTab(explorerModule, "File Explorer");
    mainWindow->addTab(queueModule, "Queue");
    mainWindow->addTab(searchModule, "Search");
    mainWindow->addTab(settingsModule, "Settings");

    // The equalizer now lives inside Settings as an "Audio" page (reached either there
    // or via the player's floating EQ button), rather than as its own tab.
    settingsModule->addAudioPage(eqModule);

    // --- Description tab + Share button (replaced the banner's Info/Share) ---
    // The Description page appears as a dynamic tab whenever the playing video's
    // probe reports a description, and retires with it (local files, teardown).
    descriptionView = new QTextBrowser();
    descriptionView->setOpenExternalLinks(true);
    descriptionView->setReadOnly(true);

    // Comments page: a second dynamic tab that opens to the right of Description when
    // the comments fetch lands. Built from yt-dlp's `comments` array (see below).
    commentsView = new QTextBrowser();
    commentsView->setReadOnly(true);
    // We handle link clicks ourselves: the only anchors are the "View replies" toggles
    // (href="toggle:<id>"). Comment text is HTML-escaped, so it contains no real links.
    commentsView->setOpenLinks(false);
    connect(commentsView, &QTextBrowser::anchorClicked, this, [this](const QUrl& url) {
        if (url.scheme() == QLatin1String("toggle")) toggleCommentThread(url.path());
    });
    commentsView->installEventFilter(this); // catch QEvent::PaletteChange to re-theme the cards

    // The Comments tab is the view plus a bottom "loading" pill that mirrors the
    // Search tab's status pill, so the lazy comment fetch (initial load + scroll
    // "load more") surfaces the same familiar progress affordance at the bottom of
    // the screen. The pill reuses the searchStatus* object names so Theme styles it.
    m_commentsMovie = new QMovie(":/Assets/SeagullAnim.gif", QByteArray(), this);
    m_commentsMovie->jumpToFrame(0);
    const QSize cFrame = m_commentsMovie->currentPixmap().size();
    const int cSpinH = 22;
    const int cSpinW = cFrame.height() > 0 ? cFrame.width() * cSpinH / cFrame.height() : cSpinH;
    m_commentsMovie->setScaledSize(QSize(cSpinW, cSpinH));
    m_commentsSpinner = new QLabel();
    m_commentsSpinner->setMovie(m_commentsMovie);
    m_commentsSpinner->hide();

    m_commentsStatusLabel = new QLabel();
    m_commentsStatusLabel->setObjectName("searchStatus");

    m_commentsStatusPill = new QFrame();
    m_commentsStatusPill->setObjectName("searchStatusPill");
    auto* cPillInner = new QHBoxLayout(m_commentsStatusPill);
    cPillInner->setContentsMargins(16, 7, 16, 7);
    cPillInner->setSpacing(8);
    cPillInner->addWidget(m_commentsStatusLabel);
    cPillInner->addWidget(m_commentsSpinner);
    m_commentsStatusPill->hide();

    auto* cStatusFrame = new QWidget();
    auto* cStatusLay   = new QHBoxLayout(cStatusFrame);
    cStatusLay->setContentsMargins(10, 4, 10, 10);
    cStatusLay->addStretch(1);
    cStatusLay->addWidget(m_commentsStatusPill);
    cStatusLay->addStretch(1);

    commentsContainer = new QWidget();
    auto* cLay = new QVBoxLayout(commentsContainer);
    cLay->setContentsMargins(0, 0, 0, 0);
    cLay->setSpacing(0);
    cLay->addWidget(commentsView, 1);
    cLay->addWidget(cStatusFrame);
    commentsContainer->installEventFilter(this); // fetch comments lazily when the tab is viewed

    connect(videoPlayer, &VideoPlayer::videoInfoChanged, this,
        [this](const QString& title, const QString& uploader, const QString& views,
               const QString& date, const QString& description) {
            if (description.trimmed().isEmpty()) {
                mainWindow->closeDynamicTab(descriptionView);
                // The empty emit is also the per-video reset / teardown signal — retire
                // the Comments tab too and abort any in-flight fetch; the probe re-offers
                // it (commentsAvailable) when the next video resolves.
                mainWindow->closeDynamicTab(commentsContainer);
                resetCommentsState();
                return;
            }
            QStringList bits;
            if (!uploader.isEmpty())                 bits << uploader;
            if (!views.isEmpty() && views != "0")    bits << views + " views";
            if (!date.isEmpty())                     bits << date;
            descriptionView->setHtml(
                "<h3>" + title.toHtmlEscaped() + "</h3>"
                + (bits.isEmpty() ? QString()
                    : "<p>" + bits.join(QStringLiteral("   |   ")).toHtmlEscaped() + "</p><hr>")
                + "<p style=\"white-space: pre-wrap;\">" + description.toHtmlEscaped() + "</p>");
            mainWindow->openDynamicTab(descriptionView, "Description");
        });
    connect(videoPlayer, &VideoPlayer::shareAvailableChanged, mainWindow, &MainWindow::setShareAvailable);
    connect(mainWindow, &MainWindow::shareRequested, videoPlayer, &VideoPlayer::shareLink);

    // The player's floating EQ button jumps to the equalizer's home: reveal the
    // Settings tab and select its Audio page.
    connect(mainWindow, &MainWindow::eqRequested, this, [this]() {
        mainWindow->showTab(settingsModule);
        settingsModule->showAudioPage();
    });

    // --- Comments tab (lazy) --------------------------------------------------
    // The probe reports comment_count for free, so we can offer a Comments tab
    // without fetching anything. The actual (slow, paginated) comment fetch runs
    // only when the tab is first viewed (eventFilter -> loadCommentsLazy), so a
    // second heavy yt-dlp extraction never contends with the just-started stream.
    connect(playerWorker, &SgYtDlp::commentCountKnown, videoPlayer,
            &VideoPlayer::onCommentCount, Qt::QueuedConnection);

    // Drips the remaining comment batches into the view after the first one, so the
    // rich-text layout never does a single huge pass that freezes the UI.
    m_commentRenderTimer = new QTimer(this);
    m_commentRenderTimer->setInterval(30);
    connect(m_commentRenderTimer, &QTimer::timeout, this, &Seagull::renderCommentBatch);

    // "Load more" on scroll-to-bottom: once everything fetched is on screen and more
    // exist, re-request a bigger window (yt-dlp has no offset; dedupe handles overlap).
    connect(commentsView->verticalScrollBar(), &QScrollBar::valueChanged, this, [this](int value) {
        QScrollBar* sb = commentsView->verticalScrollBar();
        if (sb->maximum() <= 0 || value < sb->maximum() - 60) return; // not near the bottom
        if (m_commentsLoadingMore || m_commentsAllLoaded) return;
        if (m_commentRenderIdx < m_commentThreads.size()) return;     // still drip-rendering
        m_commentsLoadingMore = true;
        m_commentsRequested += 50;   // widen the window by one page
        setCommentsStatus("Loading more comments.", true);
        requestComments();
    });

    connect(videoPlayer, &VideoPlayer::commentsAvailable, this,
        [this](const QString& pageUrl, int commentCount) {
            resetCommentsState(); // drop any prior video's fetch + half-rendered list
            m_commentsPageUrl = pageUrl;
            m_commentCount    = commentCount;
            if (commentCount <= 0) { mainWindow->closeDynamicTab(commentsContainer); return; }
            // Placeholder shown the instant the tab is opened; the fetch starts then.
            commentsView->setHtml(
                "<h3>Comments (" + QLocale(QLocale::English).toString(commentCount) + ")</h3>"
                "<hr><p>Loading...</p>");
            mainWindow->openDynamicTab(commentsContainer, "Comments"); // appends right of Description
            // Preload the first batch now (don't wait for the tab to be viewed). The fetch
            // runs on commentsThread / its own yt-dlp process, so it doesn't stall the
            // stream, and the list is usually ready by the time the user clicks the tab.
            loadCommentsLazy();
        });

    // The fetched comments land here (queued from commentsThread). yt-dlp returns the
    // thread in display order (each comment followed by its replies), so we render in
    // array order and only APPEND comment ids we haven't shown yet — that makes the
    // "load more" re-fetch (a bigger window) cost-free to merge.
    connect(commentsWorker, &SgYtDlp::commentsReady, this,
        [this](const QJsonArray& comments, int totalCount) {
            m_commentRenderTimer->stop();
            const bool firstBatch = m_commentSeenIds.isEmpty();

            if (firstBatch) {
                if (comments.isEmpty()) {
                    commentsView->setHtml("<h3>Comments</h3><hr><p>Comments are unavailable.</p>");
                    m_commentsAllLoaded = true;
                    setCommentsStatus("", false);
                    return;
                }
                // Always show the video's TOTAL comment count (from the probe), not how
                // many we've fetched/rendered so far. m_commentCount is the canonical total.
                const int shownTotal = m_commentCount > 0 ? m_commentCount : totalCount;
                QString header = "<h3>Comments";
                if (shownTotal > 0) header += " (" + QLocale(QLocale::English).toString(shownTotal) + ")";
                header += "</h3><hr>";
                m_commentsHeaderHtml = header;
                commentsView->setHtml(header);
            }

            // Extract the theme-independent pieces of one comment (no colours baked in —
            // commentEntryHtml applies the themed colours fresh at render time).
            auto makeEntry = [](const QJsonObject& c) -> CommentEntry {
                const qint64 likes = qint64(c["like_count"].toDouble());
                const qint64 ts    = qint64(c["timestamp"].toDouble());
                QStringList meta;
                if (likes > 0) meta << QLocale(QLocale::English).toString(likes) + " likes";
                if (ts > 0)    meta << QDateTime::fromSecsSinceEpoch(ts).toString("MMM d, yyyy");

                CommentEntry e;
                e.authorHtml = c["author"].toString().toHtmlEscaped();
                e.isUploader = c["author_is_uploader"].toBool();
                e.metaText   = meta.join(" &middot; "); // digits/dates only — safe unescaped
                e.textHtml   = c["text"].toString().toHtmlEscaped();
                return e;
            };

            // Group into top-level threads with their replies (yt-dlp lists a parent before
            // its replies, so the parent is always indexed first). Dedupe by id so the
            // "load more" re-fetch (a bigger window) merges cleanly.
            const int prevThreadCount = m_commentThreads.size();
            bool existingThreadGrew = false; // a reply landed on an already-rendered thread
            int added = 0;
            for (const QJsonValue& v : comments) {
                const QJsonObject c = v.toObject();
                const QString id = c["id"].toString();
                if (id.isEmpty() || m_commentSeenIds.contains(id)) continue;
                m_commentSeenIds.insert(id);
                ++added;

                const QString parent = c["parent"].toString();
                const bool reply = !parent.isEmpty() && parent != QStringLiteral("root");
                auto it = reply ? m_commentThreadIndex.find(parent) : m_commentThreadIndex.end();
                if (reply && it != m_commentThreadIndex.end()) {
                    m_commentThreads[it.value()].replies << makeEntry(c);
                    if (it.value() < prevThreadCount) existingThreadGrew = true;
                } else {
                    // A top-level comment, or a reply whose parent fell outside the window
                    // (render it as its own thread so it isn't lost).
                    m_commentThreads.append({ id, makeEntry(c), {}, false });
                    m_commentThreadIndex.insert(id, m_commentThreads.size() - 1);
                }
            }

            // Got fewer than we asked for, or nothing new -> there's no more to load.
            if (comments.size() < m_commentsRequested || (!firstBatch && added == 0))
                m_commentsAllLoaded = true;
            m_commentsLoadingMore = false;
            setCommentsStatus("", false); // data is in — retire the loading pill

            // A load-more batch that only added replies to threads already on screen has to
            // rebuild the list (so their toggle counts/expanded replies update); otherwise
            // just drip the newly added threads onto the end.
            if (existingThreadGrew) {
                rerenderComments();
            } else {
                renderCommentBatch();
                if (m_commentRenderIdx < m_commentThreads.size()) m_commentRenderTimer->start();
            }
        });

    // When a module wants something played, it tells the window. We remember which
    // source asked, so "play next" later knows whether to walk the library, the
    // explorer's file list, or the queue.
    connect(libraryModule, &MediaLibrary::playMediaRequested, videoPlayer, [this](const QUrl& url) {
        activeSource = ActiveSource::Library;
        videoPlayer->playLocalFile(url);
        });

    connect(queueModule, &Queue::playMediaRequested, videoPlayer,
        [this](const QUrl& rawUrl, const QUrl& cdnVideoUrl, const QUrl& cdnAudioUrl, const QString& title) {
            activeSource = ActiveSource::Queue;
            videoPlayer->playVideo(rawUrl, cdnVideoUrl, cdnAudioUrl, title);
        });

    // A local queue row plays through the player's local path; activeSource stays
    // Queue so auto-advance walks the queue, not the Library grid.
    connect(queueModule, &Queue::playLocalFileRequested, videoPlayer, [this](const QUrl& url) {
        activeSource = ActiveSource::Queue;
        videoPlayer->playLocalFile(url);
        });

    // Library "Playlists" card -> the Queue loads the .sgpl and starts playing it.
    connect(libraryModule, &MediaLibrary::playPlaylistRequested, queueModule, [this](const QString& path) {
        queueModule->loadPlaylistFile(path, true);
        });

    // Local files queued from Library cards / the File Explorer context menu.
    // The queue itself enforces the local/online split (clear-first modal).
    connect(libraryModule, &MediaLibrary::enqueueLocalRequested, queueModule, &Queue::addLocalFilesToQueue);

    // While the Library builds its card grid (tab/category switch), pause the audio
    // visualizer's render timer so the two don't fight over the GUI thread (the hitch
    // only showed while local audio was playing — i.e. the visualizer was up).
    connect(libraryModule, &MediaLibrary::buildBusyChanged,
            videoPlayer, &VideoPlayer::setVisualizerSuspended);

    // A fresh playlist file landed in the playlist folder — flash the Library tab.
    connect(queueModule, &Queue::playlistSaved, this, [this](const QString&) { flashLibraryTab(); });

    // Shorts-feed scroll: wheel over the playing short = next/previous result of
    // whichever search tab is the active feed. (Search card play wiring lives in
    // wireSearchTab so every search tab — primary or duplicate — behaves the same.)
    connect(videoPlayer, &VideoPlayer::shortsScrolled, this, [this](int step) {
        if (activeSource == ActiveSource::Search && m_activeSearch)
            m_activeSearch->playAdjacentResult(step);
        });

    // Display "Card size" resizes the Library cards live. Search cards are handled
    // per-tab in wireSearchTab (there can be several Search tabs).
    connect(settingsModule, &Settings::cardWidthChanged, libraryModule, &MediaLibrary::setCardWidth);
    connect(settingsModule, &Settings::visualizerSettingsChanged, videoPlayer, &VideoPlayer::applyVisualizerSettings);
    // Display "Progress bar size" resizes the player's seek bar live.
    connect(settingsModule, &Settings::seekBarSizeChanged, videoPlayer, &VideoPlayer::setSeekBarSize);

    // EQ tab live edits: apply to the player ONLY when the playing media's kind
    // matches the edited content type. Otherwise the EQ tab has already persisted it
    // (config Eq/<type>/*) and VideoPlayer auto-applies the saved curve when that kind
    // next plays — so editing the Audio EQ never disturbs a currently-playing video.
    connect(eqModule, &EQ::eqChanged, this,
        [this](EqContentType type, const QVector<float>& gains, float preampDb) {
            const MediaKind k = videoPlayer->currentMediaKind();
            const bool matches = (type == EqContentType::Audio && k == MediaKind::Audio)
                              || (type == EqContentType::Video && k == MediaKind::Video);
            if (matches) videoPlayer->applyEqualizer(gains, preampDb);
        });

    // EQ power toggle: apply the curve (on) or bypass the equalizer (off) live, but
    // only when the playing media's kind matches the toggled type. Otherwise it's
    // already persisted (Eq/<type>/Enabled) and VideoPlayer honours it on next play.
    connect(eqModule, &EQ::eqEnabledChanged, this,
        [this](EqContentType type, bool enabled, const QVector<float>& gains, float preampDb) {
            const MediaKind k = videoPlayer->currentMediaKind();
            const bool matches = (type == EqContentType::Audio && k == MediaKind::Audio)
                              || (type == EqContentType::Video && k == MediaKind::Video);
            if (!matches) return;
            if (enabled) videoPlayer->applyEqualizer(gains, preampDb);
            else         videoPlayer->disableEqualizer();
        });

    // Normalization (peak protection) toggle: apply live only when the playing media's
    // kind matches the toggled type. Otherwise it's already persisted (Eq/<type>/
    // NormEnabled) and VideoPlayer honours it on next play of that kind.
    connect(eqModule, &EQ::normalizationChanged, this,
        [this](EqContentType type, bool enabled) {
            const MediaKind k = videoPlayer->currentMediaKind();
            const bool matches = (type == EqContentType::Audio && k == MediaKind::Audio)
                              || (type == EqContentType::Video && k == MediaKind::Video);
            if (matches) videoPlayer->setNormalizationEnabled(enabled);
        });

    // Auto-follow: keep the EQ's Video/Audio selector pointed at whatever is playing, so
    // opening the equalizer lands on the curve actually shaping the sound. Photo has no
    // audio, so it leaves the selection alone. The EQ re-arms following each time its page
    // is shown and pins on a manual pill click, so the user can freely flip while editing.
    auto followEqToKind = [this](MediaKind k) {
        if (k == MediaKind::Audio)      eqModule->followPlayingKind(EqContentType::Audio);
        else if (k == MediaKind::Video) eqModule->followPlayingKind(EqContentType::Video);
    };
    connect(settingsModule, &Settings::audioPageShown, this, [this, followEqToKind]() {
        eqModule->armFollow();
        followEqToKind(videoPlayer->currentMediaKind());
    });
    connect(videoPlayer, &VideoPlayer::mediaKindChanged, this,
        [followEqToKind](MediaKind k) { followEqToKind(k); });

    // Multiple-instance tabs. The primary Search + File Explorer go through the same
    // per-tab wiring the duplicates use; register them as duplicable so the "+" menu
    // offers "New Search tab" / "New File Explorer tab", and wire the open/close hooks.
    m_searchTabs.append(searchModule);     m_activeSearch   = searchModule;   wireSearchTab(searchModule);
    m_explorerTabs.append(explorerModule); m_activeExplorer = explorerModule; wireExplorerTab(explorerModule);
    mainWindow->registerDuplicableTab("Search", "New Search tab");
    mainWindow->registerDuplicableTab("File Explorer", "New File Explorer tab");
    connect(mainWindow, &MainWindow::newTabRequested,   this,
        [this](const QString& kind) { openDuplicateTab(kind); }); // "+" menu: switch to the new tab
    connect(mainWindow, &MainWindow::duplicateTabClosed, this, &Seagull::disposeDuplicateTab);

    // General "Check for Updates" button -> manual app-version check (shows an
    // "up to date" message too, unlike the silent startup check).
    connect(settingsModule, &Settings::checkForUpdatesRequested, this, [this]() {
        m_appCheckManual = true;
        appUpdate->checkForUpdate();
    });
    connect(qApp, &QCoreApplication::aboutToQuit, searchModule, [this]() {
        QSettings s(SgPaths::configFile(), QSettings::IniFormat);
        if (s.value("Search/ClearHistoryOnExit", false).toBool())
            searchModule->clearSearchHistory();
        });

    // Each finished ad-hoc download advances the FIFO; the Library spinner stays up
    // until the queue drains.
    connect(downloadWorker, &SgYtDlp::finished, this, [this](bool /*ok*/) {
        if (!m_downloadQueue.isEmpty()) m_downloadQueue.removeFirst();
        m_downloading = false;
        if (!m_downloadQueue.isEmpty()) pumpDownloads();
        else mainWindow->setTabBusy(libraryModule, false);
        });

    // Surface the search/download workers' logs in the same dev console as the rest.
    connect(searchWorker, &SgSearch::logMessage, downloaderWorker, &SgYtDlp::logMessage, Qt::QueuedConnection);
    connect(downloadWorker, &SgYtDlp::logMessage, downloaderWorker, &SgYtDlp::logMessage, Qt::QueuedConnection);

    // Any worker that hits a bot-check / throttling block warns the user (debounced).
    // Queued so the modal opens from the event loop, not re-entrantly inside the
    // worker's finished-handler while it's mid-emit.
    for (SgYtDlp* w : { downloaderWorker, resolverWorker, prefetcherWorker, playerWorker, downloadWorker, commentsWorker, shortsPrefetcher })
        connect(w, &SgYtDlp::extractionBlocked, this, &Seagull::onExtractionBlocked, Qt::QueuedConnection);

    // Shorts prefetch results land here. streamUrlReady = success (cache the CDN, then
    // fetch the next one in the lookahead); finished(false) = failure (drop it, continue).
    // On success only streamUrlReady fires (the metadata path emits no finished(true)),
    // so the two never double-handle. m_shortsBusyUrl gates out cancelled/stray emits.
    connect(shortsPrefetcher, &SgYtDlp::streamUrlReady, this, [this](const QUrl& v, const QUrl& a) {
        if (m_shortsBusyUrl.isEmpty()) return; // cancelled mid-flight or a stray
        m_shortsWatchdogTimer->stop();
        m_shortsReady.insert(m_shortsBusyUrl, qMakePair(v, a));
        m_shortsBusyUrl.clear();
        pumpShortsPrefetch(); // resolve the second short in the lookahead window
    });
    connect(shortsPrefetcher, &SgYtDlp::finished, this, [this](bool ok) {
        if (m_shortsPrefetchCancelling) return; // our own cancel() — not a real result
        if (ok || m_shortsBusyUrl.isEmpty()) return; // success is handled above; or a stray
        m_shortsWatchdogTimer->stop();
        m_shortsBusyUrl.clear();                // failed — abandon it (already off the want list)
        pumpShortsPrefetch();
    });
    // Debounce: each fetch launches after a small jittered delay so the speculative
    // requests don't hit YouTube on a fixed metronome (an easy bot tell).
    m_shortsDebounceTimer = new QTimer(this);
    m_shortsDebounceTimer->setSingleShot(true);
    connect(m_shortsDebounceTimer, &QTimer::timeout, this, [this]() {
        m_shortsScheduled = false;
        if (!m_shortsBusyUrl.isEmpty()) return;            // a fetch already started
        while (!m_shortsWant.isEmpty()
               && (m_shortsWant.front().isEmpty() || m_shortsReady.contains(m_shortsWant.front())))
            m_shortsWant.removeFirst();                    // skip any that became ready meanwhile
        if (m_shortsWant.isEmpty()) return;
        m_shortsBusyUrl = m_shortsWant.takeFirst();
        shortsPrefetcher->fetchMetadataAndStreamUrl(m_shortsBusyUrl);
        m_shortsWatchdogTimer->start(15000);               // recover the slot if it never reports
    });
    // Watchdog for the no-result resolve (see header): abandon a fetch that never reports.
    m_shortsWatchdogTimer = new QTimer(this);
    m_shortsWatchdogTimer->setSingleShot(true);
    connect(m_shortsWatchdogTimer, &QTimer::timeout, this, [this]() {
        if (m_shortsBusyUrl.isEmpty()) return;
        m_shortsBusyUrl.clear();        // already off the want list — don't re-storm it
        cancelShortsFetch();            // kill a wedged process; its finished() is ignored
        pumpShortsPrefetch();
    });

    // Once a queued/streamed video starts, clear the URL bar (the metadata preview
    // stays up, showing the now-playing video). Library playback leaves it alone.
    connect(videoPlayer, &VideoPlayer::playbackStarted, this, [this]() {
        if (activeSource == ActiveSource::Queue) queueModule->clearUrlForPlayback();
        // Point the autoplay/shuffle toggles at this content type and arm the
        // slideshow if a photo just loaded.
        m_currentIsPhoto = (videoPlayer->currentMediaKind() == MediaKind::Photo);
        mainWindow->setPlaybackContext(currentContextKey(), m_currentIsPhoto);
        // setPlaybackContext just pointed the autoplay toggle at this content type;
        // mirror it into the player so a finished short loops vs. advances correctly.
        videoPlayer->setAutoplayEnabled(mainWindow->autoplayEnabled());
        updateSlideshow();
        });

    // A finished video rolls into the next one — but anything waiting in the
    // queue outranks the grids: it plays next no matter where the finished item
    // came from (the queue's play signals re-point activeSource at Queue). When
    // shuffle is on, the next pick from each source is random.
    connect(videoPlayer, &VideoPlayer::mediaEnded, this, [this]() {
        if (!mainWindow->autoplayEnabled()) return;
        const bool shuffle = mainWindow->shuffleEnabled();
        if (shuffle ? queueModule->playRandomOrStart() : queueModule->playNextOrStart()) return;
        if (activeSource == ActiveSource::Library)
            shuffle ? libraryModule->playRandomFile() : libraryModule->playNextFile();
        else if (activeSource == ActiveSource::Explorer && m_activeExplorer)
            shuffle ? m_activeExplorer->playRandomFile() : m_activeExplorer->playNextFile();
        else if (activeSource == ActiveSource::Search && m_activeSearch)
            shuffle ? m_activeSearch->playRandomResult() : m_activeSearch->playAdjacentResult(1);
        });

    // The skip buttons (single-click = nudge, double-click = jump tracks) land here,
    // and so do the SMTC next/previous keys (see skipActive).
    connect(videoPlayer, &VideoPlayer::skipRequested, this, [this](int delta) { skipActive(delta); });

    // --- Windows media controls (SMTC) ---
    // Player -> OS: mirror state, metadata and artwork into the now-playing widget.
    connect(videoPlayer, &VideoPlayer::playbackStarted, mediaControls, [this]() {
        mediaControls->setEnabled(true);
        // Optimistic Playing so the session is active before metadata lands; the
        // engine's real playing/paused signal corrects it a beat later.
        mediaControls->setPlaybackStatus(SgMediaControls::Status::Playing);
        });
    connect(videoPlayer, &VideoPlayer::smtcStateChanged, mediaControls, [this](int state) {
        switch (state) {
        case 1: mediaControls->setPlaybackStatus(SgMediaControls::Status::Playing); break;
        case 2: mediaControls->setPlaybackStatus(SgMediaControls::Status::Paused);  break;
        default: mediaControls->setPlaybackStatus(SgMediaControls::Status::Stopped); break;
        }
        });
    connect(videoPlayer, &VideoPlayer::smtcMetadata, mediaControls, &SgMediaControls::setMetadata);
    connect(videoPlayer, &VideoPlayer::smtcArtwork,  mediaControls, &SgMediaControls::setThumbnail);
    connect(videoPlayer, &VideoPlayer::closed, mediaControls, [this]() { mediaControls->clear(); });

    // OS -> player: media keys / overlay buttons drive playback.
    connect(mediaControls, &SgMediaControls::playPressed,     videoPlayer, &VideoPlayer::togglePlayPause);
    connect(mediaControls, &SgMediaControls::pausePressed,    videoPlayer, &VideoPlayer::togglePlayPause);
    connect(mediaControls, &SgMediaControls::nextPressed,     this, [this]() { skipActive(1); });
    connect(mediaControls, &SgMediaControls::previousPressed, this, [this]() { skipActive(-1); });

    // Push the timeline (position/duration) to the overlay scrubber while playing.
    smtcTimelineTimer = new QTimer(this);
    smtcTimelineTimer->setInterval(1000);
    connect(smtcTimelineTimer, &QTimer::timeout, this, [this]() {
        if (videoPlayer->hasActiveMedia())
            mediaControls->setTimeline(videoPlayer->mediaPosition(), videoPlayer->mediaDuration());
        });
    smtcTimelineTimer->start();

    // Photo slideshow: advance to the next photo when the interval elapses. Each
    // new photo's playbackStarted re-arms it (see updateSlideshow); landing on the
    // last photo simply leaves the one-shot timer stopped, so the show ends there.
    slideshowTimer = new QTimer(this);
    slideshowTimer->setSingleShot(true);
    connect(slideshowTimer, &QTimer::timeout, this, [this]() { skipActive(1); });
    // Toggling the slideshow control, or editing the interval, re-evaluates it.
    connect(mainWindow, &MainWindow::autoplayChanged,      this, [this](bool on) {
        videoPlayer->setAutoplayEnabled(on); // a short ending mid-toggle loops vs. advances correctly
        updateSlideshow();
    });
    connect(mainWindow, &MainWindow::photoIntervalChanged, this, [this](int)  { updateSlideshow(); });
    // Tearing down playback ends any running slideshow.
    connect(videoPlayer, &VideoPlayer::closed, this, [this]() {
        m_currentIsPhoto = false;
        slideshowTimer->stop();
        });

    // Surface the player worker's logs (stream resolution, yt-dlp errors) in the
    // same dev console as the others, by re-emitting through the downloader.
    connect(playerWorker, &SgYtDlp::logMessage, downloaderWorker, &SgYtDlp::logMessage, Qt::QueuedConnection);

    // The player's stop/EOF poster for LOCAL files: a dedicated thumbnailer
    // (it shares the disk cache with the Library's, so anything the Library
    // already thumbed answers instantly) grabs a frame / cover art per play.
    playerThumbnailer = new SgThumbnailer(this);
    connect(videoPlayer, &VideoPlayer::localPosterRequested,
            playerThumbnailer, &SgThumbnailer::requestThumbnail);
    connect(playerThumbnailer, &SgThumbnailer::thumbnailReady,
            videoPlayer, &VideoPlayer::onLocalPosterReady);

    // Hold every thumbnail ffmpeg queue until the startup update modal is done:
    // updates run FIRST now (the modal locks the app), and an ffmpeg.exe swap
    // must never race a running grab. releaseThumbnailHolds() lifts both.
    libraryModule->setThumbnailsHeld(true);
    playerThumbnailer->setHeld(true);

    // Player asks the backend to resolve qualities and stream URLs on demand.
    connect(videoPlayer, &VideoPlayer::probeQualitiesRequested, playerWorker, &SgYtDlp::probeAvailableQualities);

    connect(videoPlayer, &VideoPlayer::streamUrlRequested, playerWorker, [this](const QString& url, const QString& formatId, bool freshResolve) {
        playerWorker->cancel(); // free the worker (e.g. drop an in-flight quality probe) so the resolve runs now
        playerWorker->fetchMetadataAndStreamUrl(url, formatId, freshResolve);
        });

    // Results come back on queued connections so they always land on the UI thread.
    connect(playerWorker, &SgYtDlp::availableQualitiesFound, videoPlayer, &VideoPlayer::handleAvailableQualities, Qt::QueuedConnection);
    connect(playerWorker, &SgYtDlp::liveStatusKnown, videoPlayer, &VideoPlayer::onLiveStatus, Qt::QueuedConnection);
    connect(playerWorker, &SgYtDlp::thumbnailResolved, videoPlayer, &VideoPlayer::onThumbnailResolved, Qt::QueuedConnection);
    connect(playerWorker, &SgYtDlp::videoInfoReady, videoPlayer, &VideoPlayer::onVideoInfo, Qt::QueuedConnection);
    connect(playerWorker, &SgYtDlp::streamUrlReady, videoPlayer, &VideoPlayer::onStreamUrlReady, Qt::QueuedConnection);

    // --- Live-stream recording (parallel ffmpeg against the same resolved URLs) ---
    connect(videoPlayer, &VideoPlayer::recordStartRequested, recorder,
        [this](const QUrl& videoUrl, const QUrl& audioUrl, const QString& referer, const QString& title) {
            // The URL VLC plays is already the (ad-free, for Twitch) stream; record it
            // verbatim. Referer is the page URL (ignored for Twitch's localhost proxy
            // URL, but helps hotlink-protected CDNs on other live sites).
            recorder->start(videoUrl, audioUrl, referer, title);
        });
    connect(videoPlayer, &VideoPlayer::recordStopRequested, recorder, &SgRecorder::stop);
    // VOD: clip the watched range [startMs,endMs] — direct ffmpeg cut of the resolved
    // stream URLs, with a yt-dlp full-download fallback driven off the page URL.
    connect(videoPlayer, &VideoPlayer::recordClipRequested, recorder,
        [this](const QString& pageUrl, const QUrl& videoUrl, const QUrl& audioUrl,
            qint64 startMs, qint64 endMs, const QString& title) {
            recorder->clipSection(pageUrl, videoUrl, audioUrl, startMs, endMs, title);
        });
    connect(videoPlayer, &VideoPlayer::recordClipCancelRequested, recorder, &SgRecorder::cancelClip);

    connect(recorder, &SgRecorder::started, videoPlayer, [this](const QString&) { videoPlayer->onRecordingStarted(); }, Qt::QueuedConnection);
    connect(recorder, &SgRecorder::finished, videoPlayer, [this](const QString& file, bool ok) {
        videoPlayer->onRecordingStopped(file, ok);
        if (ok && !file.isEmpty()) flashLibraryTab(); // the recording is on disk + playable
        }, Qt::QueuedConnection);
    connect(recorder, &SgRecorder::clipFinished, videoPlayer, [this](const QString& file, bool ok) {
        videoPlayer->onClipFinished(file, ok);
        if (ok && !file.isEmpty()) flashLibraryTab(); // the clip is on disk + playable
        }, Qt::QueuedConnection);
    connect(recorder, &SgRecorder::logMessage, downloaderWorker, &SgYtDlp::logMessage, Qt::QueuedConnection);

    // --- Tool auto-update, off the main thread ---
    // Checking and downloading yt-dlp / Deno / ffmpeg means blocking network calls,
    // SHA-256 hashing, and unzipping that can take many seconds. Running that on the
    // UI thread froze startup, so the updater lives on its own thread instead. The
    // worker has no parent (a requirement for moveToThread); its child QProcess and
    // network manager move across with it automatically.
    updaterThread = new QThread(this);
    updaterWorker = new SgUpdater(nullptr);
    updaterWorker->moveToThread(updaterThread);

    // Status lines still show up in the Queue log, hopping back to the UI thread.
    connect(updaterWorker, &SgUpdater::updateStatus, downloaderWorker, &SgYtDlp::logMessage, Qt::QueuedConnection);

    // Tear the worker down with its thread.
    connect(updaterThread, &QThread::finished, updaterWorker, &QObject::deleteLater);

    updaterThread->start();

    // The startup check/install is driven by the modal UpdateDialog in run();
    // it owns checkFinished/applyProgress/applyFinished for the whole flow.
}

void Seagull::armShortsPrefetch(const QStringList& upcoming) {
    // `upcoming` is the next up-to-kShortsLookahead shorts in feed order. Keep only the
    // resolved entries still inside that window (drop anything scrolled past), then queue
    // the ones we don't already have.
    const QSet<QString> keep(upcoming.begin(), upcoming.end());
    for (auto it = m_shortsReady.begin(); it != m_shortsReady.end(); )
        it = keep.contains(it.key()) ? std::next(it) : m_shortsReady.erase(it);

    // An in-flight fetch for a short no longer in the window is wasted — drop it.
    if (!m_shortsBusyUrl.isEmpty() && !keep.contains(m_shortsBusyUrl)) {
        m_shortsBusyUrl.clear();
        cancelShortsFetch();
    }

    m_shortsWant.clear();
    for (const QString& u : upcoming)
        if (!u.isEmpty() && !m_shortsReady.contains(u) && u != m_shortsBusyUrl)
            m_shortsWant.append(u);
    pumpShortsPrefetch();
}

void Seagull::pumpShortsPrefetch() {
    if (!m_shortsBusyUrl.isEmpty()) return;         // one resolve at a time
    if (m_shortsScheduled) return;                  // a debounced launch is already pending
    while (!m_shortsWant.isEmpty()
           && (m_shortsWant.front().isEmpty() || m_shortsReady.contains(m_shortsWant.front())))
        m_shortsWant.removeFirst();
    if (m_shortsWant.isEmpty()) return;
    m_shortsScheduled = true;
    // Small base (~200ms) + jitter(0..600) so launches don't form a fixed cadence.
    m_shortsDebounceTimer->start(200 + int(QRandomGenerator::global()->bounded(600)));
}

void Seagull::clearShortsPrefetch() {
    m_shortsDebounceTimer->stop();
    m_shortsWatchdogTimer->stop();
    m_shortsScheduled = false;
    m_shortsWant.clear();
    m_shortsReady.clear();
    if (!m_shortsBusyUrl.isEmpty()) {
        m_shortsBusyUrl.clear();
        cancelShortsFetch();
    }
}

void Seagull::cancelShortsFetch() {
    // cancel() is synchronous and fires finished(false); the guard tells that handler the
    // emission is ours, not a real failure. Caller has already cleared m_shortsBusyUrl.
    m_shortsPrefetchCancelling = true;
    shortsPrefetcher->cancel();
    m_shortsPrefetchCancelling = false;
}

void Seagull::wireSearchTab(Search* s) {
    // A search card plays through the same path as the queue; a short additionally
    // loops at the end and the wheel walks the feed. Capturing `s` lets the active
    // feed (skip / shorts-scroll) follow whichever search tab started playback.
    connect(s, &Search::playMediaRequested, videoPlayer,
        [this, s](const QUrl& rawUrl, const QUrl& cdnVideoUrl, const QUrl& cdnAudioUrl, const QString& title) {
            activeSource   = ActiveSource::Search;
            m_activeSearch = s;
            const bool wasShorts = videoPlayer->shortsMode();
            const bool isShort   = rawUrl.toString().contains("/shorts/", Qt::CaseInsensitive);

            // If this short was prefetched (scroll-ahead), hand the player the resolved
            // CDN so it starts instantly instead of re-running yt-dlp.
            QUrl v = cdnVideoUrl, a = cdnAudioUrl;
            if (isShort && v.isEmpty()) {
                auto it = m_shortsReady.constFind(rawUrl.toString());
                if (it != m_shortsReady.constEnd() && !it->first.isEmpty()) {
                    v = it->first;
                    a = it->second;
                }
            }

            videoPlayer->playVideo(rawUrl, v, a, title);
            videoPlayer->setShortsMode(isShort); // playVideo cleared it — re-arm
            if (isShort && !wasShorts) mainWindow->collapseTabs();

            // Warm the next two shorts so scrolling stays instant; leaving shorts drops
            // the prefetch state and frees the worker.
            if (isShort) armShortsPrefetch(s->peekForwardUrls(kShortsLookahead));
            else         clearShortsPrefetch();
        });
    // Card "Queue" adds to the Queue tab; "Download" goes to the dedicated download
    // worker's FIFO (files land in the library).
    connect(s, &Search::enqueueRequested, queueModule,
        [this](const QUrl& url, const QString& title) { queueModule->addUrlToQueue(url.toString(), title); });
    connect(s, &Search::downloadRequested, this,
        [this](const QUrl& url, const QString& /*title*/) { m_downloadQueue.append(url.toString()); pumpDownloads(); });
    // Live "Card size" + the Search settings' "Clear History Now" reach every tab.
    connect(settingsModule, &Settings::cardWidthChanged,      s, &Search::setCardWidth);
    connect(settingsModule, &Settings::clearHistoryRequested, s, &Search::clearSearchHistory);
}

void Seagull::wireExplorerTab(FileExplorer* e) {
    connect(e, &FileExplorer::playMediaRequested, videoPlayer, [this, e](const QUrl& url) {
        activeSource     = ActiveSource::Explorer;
        m_activeExplorer = e; // this tab's file list is what skip walks
        videoPlayer->playLocalFile(url);
        });
    connect(e, &FileExplorer::enqueueRequested, queueModule, &Queue::addLocalFilesToQueue);
}

void Seagull::openDuplicateTab(const QString& kind, bool switchTo) {
    // "+" menu -> a fresh extra instance. Each Search duplicate gets its OWN SgSearch
    // worker (true concurrent searching — which is exactly why Search warns about the
    // bot risk of two tabs on one site). File Explorer has no worker.
    if (kind == QStringLiteral("Search")) {
        auto* worker = new SgSearch(this);
        connect(worker, &SgSearch::logMessage, downloaderWorker, &SgYtDlp::logMessage, Qt::QueuedConnection);
        auto* s = new Search(worker, spellChecker);
        m_searchWorkers.insert(s, worker);
        m_searchTabs.append(s);
        wireSearchTab(s);
        mainWindow->addDuplicateTab(s, "Search", switchTo);
    }
    else if (kind == QStringLiteral("File Explorer")) {
        auto* e = new FileExplorer(spellChecker);
        m_explorerTabs.append(e);
        wireExplorerTab(e);
        mainWindow->addDuplicateTab(e, "File Explorer", switchTo);
    }
}

void Seagull::disposeDuplicateTab(QWidget* page) {
    // A duplicate tab closed: delete the instance (and its worker). The active-feed
    // pointer falls back to the primary so skip/scroll still have a valid target.
    if (auto* s = qobject_cast<Search*>(page)) {
        m_searchTabs.removeOne(s);
        if (m_activeSearch == s) m_activeSearch = searchModule;
        if (SgSearch* w = m_searchWorkers.take(s)) w->deleteLater();
        s->deleteLater();
    }
    else if (auto* e = qobject_cast<FileExplorer*>(page)) {
        m_explorerTabs.removeOne(e);
        if (m_activeExplorer == e) m_activeExplorer = explorerModule;
        e->deleteLater();
    }
}

void Seagull::releaseThumbnailHolds() {
    libraryModule->setThumbnailsHeld(false);
    playerThumbnailer->setHeld(false);
}

void Seagull::ensureUpdater() {
    // The startup flow shuts the updater down once it's done (shutdownUpdater),
    // so the manual "Check for Updates" self-update path can arrive with no worker.
    // Recreate it on demand, mirroring the original construction. Parented to its
    // QThread's owner (this), so it tears down with the app; a self-update that
    // proceeds quits us anyway.
    if (updaterThread) return;
    updaterThread = new QThread(this);
    updaterWorker = new SgUpdater(nullptr);
    updaterWorker->moveToThread(updaterThread);
    connect(updaterWorker, &SgUpdater::updateStatus, downloaderWorker, &SgYtDlp::logMessage, Qt::QueuedConnection);
    connect(updaterThread, &QThread::finished, updaterWorker, &QObject::deleteLater);
    updaterThread->start();
}

void Seagull::shutdownUpdater() {
    // Safe to call as soon as the setup/update dialog has closed: the dialogs
    // only close after applyFinished (or with nothing started), so the worker
    // is idle. finished -> deleteLater (wired in the ctor) frees the worker on
    // its own thread as it winds down.
    if (!updaterThread) return;
    updaterThread->quit();
    updaterThread->wait();
    updaterThread->deleteLater();
    updaterThread = nullptr;
    updaterWorker = nullptr; // deleted via the finished->deleteLater connect
}

Seagull::~Seagull() {
    // Stop the worker threads cleanly before anything else goes away.
    if (commentsThread) {
        commentsThread->quit();
        commentsThread->wait();
    }
    if (updaterThread) {
        updaterThread->quit();
        updaterThread->wait();
    }
    delete mainWindow;
}

void Seagull::pumpDownloads() {
    if (m_downloading || m_downloadQueue.isEmpty()) return;
    m_downloading = true;
    mainWindow->setTabBusy(libraryModule, true); // spin the Library tab while downloading
    downloadWorker->download(m_downloadQueue.first());
}

bool Seagull::eventFilter(QObject* obj, QEvent* event) {
    // Comments tab became visible (the user switched to it) — fetch on demand, once.
    if (obj == commentsContainer && event->type() == QEvent::Show)
        loadCommentsLazy();
    // Theme switch: the comment cards are a static HTML document with colours baked in at
    // render time, so re-render them from the stored (theme-independent) data to pick up
    // the new palette. Guarded on a non-empty list so it's a no-op the rest of the time.
    if (obj == commentsView && event->type() == QEvent::PaletteChange && !m_commentThreads.isEmpty())
        rerenderComments();
    return QObject::eventFilter(obj, event);
}

void Seagull::loadCommentsLazy() {
    if (m_commentsFetched || m_commentCount <= 0 || m_commentsPageUrl.isEmpty()) return;
    m_commentsFetched = true;     // first fetch issued; reset when the next video resolves
    m_commentsRequested = 50;     // small initial window for a fast first paint; "load more" widens it
    setCommentsStatus("Loading comments.", true);
    requestComments();
}

void Seagull::requestComments() {
    // commentsWorker lives on commentsThread — hop the call across so the process I/O
    // and JSON parse run there, not on the GUI thread.
    const QString url = m_commentsPageUrl;
    const int n = m_commentsRequested;
    QMetaObject::invokeMethod(commentsWorker, [w = commentsWorker, url, n]() { w->fetchComments(url, n); },
                              Qt::QueuedConnection);
}

QString Seagull::commentEntryHtml(const CommentEntry& e) const {
    // Author line (name, optional creator badge, meta) above the comment body. Colours
    // come from the live palette so the text follows the active theme on every render.
    const QString accent  = commentsView->palette().color(QPalette::Highlight).name();
    const QString subtext = commentsView->palette().color(QPalette::PlaceholderText).name();

    QString head = "<b>" + e.authorHtml + "</b>";
    if (e.isUploader)
        head += " <span style=\"color:" + accent + ";\">(creator)</span>";
    if (!e.metaText.isEmpty())
        head += " <span style=\"color:" + subtext + ";\"> &middot; " + e.metaText + "</span>";
    return head + "<div style=\"white-space:pre-wrap;margin-top:4px;\">" + e.textHtml + "</div>";
}

QString Seagull::commentThreadHtml(const CommentThread& t) const {
    // Card wrapper + reply-toggle colours, resolved fresh so a re-render follows the theme.
    // (QTextBrowser ignores borders on <div>, so each comment card is a single-cell table.)
    const QString cardColor   = commentsView->palette().color(QPalette::AlternateBase).name();
    const QString borderColor = commentsView->palette().color(QPalette::Mid).name();
    const QString accent      = commentsView->palette().color(QPalette::Highlight).name();

    QString body = commentEntryHtml(t.comment);
    if (!t.replies.isEmpty()) {
        const int n = t.replies.size();
        const QString label = t.expanded
            ? QStringLiteral("Hide replies")
            : QStringLiteral("View %1 %2").arg(n).arg(n == 1 ? "reply" : "replies");
        body += "<div style=\"margin-top:6px;\"><a href=\"toggle:" + t.id
              + "\" style=\"color:" + accent + ";text-decoration:none;\">" + label + "</a></div>";
        if (t.expanded)
            for (const CommentEntry& r : t.replies)
                body += "<div style=\"margin-left:24px;margin-top:10px;\">" + commentEntryHtml(r) + "</div>";
    }

    const QString style = "margin-bottom:10px;border-width:1px;border-style:solid;border-color:"
                        + borderColor + ";background-color:" + cardColor + ";";
    return "<table width=\"100%\" cellspacing=\"0\" style=\"" + style + "\">"
           "<tr><td style=\"padding:8px;\">" + body + "</td></tr></table>";
}

void Seagull::renderCommentBatch() {
    if (m_commentRenderIdx >= m_commentThreads.size()) {
        m_commentRenderTimer->stop();
        return;
    }
    constexpr int kBatch = 15;
    const int end = qMin(m_commentRenderIdx + kBatch, int(m_commentThreads.size()));
    QString chunk;
    for (int i = m_commentRenderIdx; i < end; ++i) chunk += commentThreadHtml(m_commentThreads[i]);
    m_commentRenderIdx = end;

    QTextCursor cur(commentsView->document());
    cur.movePosition(QTextCursor::End);
    cur.insertHtml(chunk); // append-only -> small layout pass, not a full re-flow
    if (m_commentRenderIdx >= m_commentThreads.size()) m_commentRenderTimer->stop();
}

void Seagull::rerenderComments() {
    // Rebuild the whole list from m_commentThreads (used when a reply toggle flips, or a
    // load-more batch grows a thread already on screen). Preserve the scroll offset so the
    // list doesn't jump under the user — content above the change stays put.
    m_commentRenderTimer->stop();
    QScrollBar* sb = commentsView->verticalScrollBar();
    const int scrollPos = sb ? sb->value() : 0;

    QString html = m_commentsHeaderHtml;
    for (const CommentThread& t : m_commentThreads) html += commentThreadHtml(t);
    commentsView->setHtml(html);
    m_commentRenderIdx = m_commentThreads.size(); // the whole list is on screen now

    if (sb) sb->setValue(qMin(scrollPos, sb->maximum()));
}

void Seagull::toggleCommentThread(const QString& id) {
    auto it = m_commentThreadIndex.find(id);
    if (it == m_commentThreadIndex.end()) return;
    CommentThread& t = m_commentThreads[it.value()];
    if (t.replies.isEmpty()) return;
    t.expanded = !t.expanded;
    rerenderComments();
}

void Seagull::resetCommentsState() {
    if (m_commentRenderTimer) m_commentRenderTimer->stop();
    m_commentThreads.clear();
    m_commentThreadIndex.clear();
    m_commentsHeaderHtml.clear();
    m_commentRenderIdx = 0;
    m_commentsPageUrl.clear();
    m_commentCount = 0;
    m_commentsFetched = false;
    m_commentSeenIds.clear();
    m_commentsRequested = 0;
    m_commentsLoadingMore = false;
    m_commentsAllLoaded = false;
    setCommentsStatus("", false); // drop any leftover loading pill from the prior video
    // Abort any in-flight fetch on its own thread (non-blocking for the GUI).
    QMetaObject::invokeMethod(commentsWorker, [w = commentsWorker]() { w->cancel(); }, Qt::QueuedConnection);
}

void Seagull::setCommentsStatus(const QString& text, bool busy) {
    if (!m_commentsStatusPill) return; // not built yet (early teardown / construction order)
    m_commentsStatusLabel->setText(text);
    m_commentsStatusPill->setVisible(!text.isEmpty());
    if (busy && !text.isEmpty()) { m_commentsSpinner->show(); m_commentsMovie->start(); }
    else                         { m_commentsMovie->stop();   m_commentsSpinner->hide(); }
}

void Seagull::flashLibraryTab() {
    // Brief seagull on the Library tab: a recording/clip just landed and is playable.
    mainWindow->setTabBusy(libraryModule, true);
    QTimer::singleShot(4000, this, [this]() {
        // Don't clear a spinner a still-draining download queue owns.
        if (m_downloadQueue.isEmpty()) mainWindow->setTabBusy(libraryModule, false);
        });
}

void Seagull::onExtractionBlocked(const QString& kind, const QString& detail) {
    // Debounce: several workers (resolver, prefetcher, player) can trip on the same
    // block within moments, and yt-dlp retries recur. Show one modal, then stay quiet
    // for a cooldown so the user isn't buried in identical dialogs.
    constexpr qint64 kCooldownMs = 60'000; // one minute between warnings
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (m_blockWarnActive || (m_lastBlockWarnMs && now - m_lastBlockWarnMs < kCooldownMs))
        return;
    m_blockWarnActive = true;

    QString title, body;
    if (kind == "throttle") {
        title = "Connection throttled";
        body  = "The site is rate-limiting requests right now (HTTP 429), so playback "
                "or downloads may fail or stall.\n\n"
                "This usually clears up on its own. Wait a few minutes before trying "
                "again, and avoid starting lots of videos or downloads at once.";
    } else {
        title = "Verification required";
        body  = "The site is asking Seagull to confirm it's not a bot, so it won't "
                "hand over the video right now.\n\n"
                "This often passes if you wait a little and try again. If it keeps "
                "happening, it's coming from the site, not from anything wrong on "
                "your end.";
    }
    if (!detail.trimmed().isEmpty())
        body += "\n\nDetails:\n" + detail.trimmed();

    QMessageBox::warning(mainWindow, title, body);

    m_lastBlockWarnMs = QDateTime::currentMSecsSinceEpoch();
    m_blockWarnActive = false;
}

void Seagull::skipActive(int delta) {
    // Forward skips honour shuffle (random next); backward always steps in order.
    const bool shuffle = delta > 0 && mainWindow->autoplayEnabled()
                                   && mainWindow->shuffleEnabled();
    if (activeSource == ActiveSource::Library) {
        if (shuffle)        libraryModule->playRandomFile();
        else if (delta > 0) libraryModule->playNextFile();
        else                libraryModule->playPrevFile();
    }
    else if (activeSource == ActiveSource::Explorer) {
        if (m_activeExplorer) {
            if (shuffle)        m_activeExplorer->playRandomFile();
            else if (delta > 0) m_activeExplorer->playNextFile();
            else                m_activeExplorer->playPrevFile();
        }
    }
    else if (activeSource == ActiveSource::Queue) {
        if (shuffle)        queueModule->playRandomOrStart();
        else if (delta > 0) queueModule->playNextQueuedItem();
        else                queueModule->playPrevQueuedItem();
    }
    else if (activeSource == ActiveSource::Search) {
        if (shuffle && m_activeSearch)      m_activeSearch->playRandomResult();
        else if (m_activeSearch)            m_activeSearch->playAdjacentResult(delta > 0 ? 1 : -1);
    }
}

QString Seagull::currentContextKey() const {
    switch (activeSource) {
    case ActiveSource::Library:  return libraryModule->sessionContextKey();
    case ActiveSource::Explorer: return QStringLiteral("explorer");
    case ActiveSource::Queue:    return QStringLiteral("queue");
    case ActiveSource::Search:   return m_activeSearch ? m_activeSearch->playbackContextKey()
                                                       : QStringLiteral("search.youtube");
    case ActiveSource::None:     break;
    }
    return QStringLiteral("queue"); // direct URL paste etc. -> queue-style context
}

void Seagull::updateSlideshow() {
    // Run the slideshow only for a photo with autoplay (slideshow) enabled.
    if (m_currentIsPhoto && mainWindow->autoplayEnabled()) {
        slideshowTimer->start(mainWindow->photoIntervalSeconds() * 1000);
    } else {
        slideshowTimer->stop();
    }
}

bool Seagull::run() {
    // The player's VLC output HWND is bound AFTER mainWindow->show() below — the
    // proven timing. What lets the startup modals run while the window is still
    // hidden is that the player no longer queues a deferred winId()/VLC hookup at
    // construction (it was a QTimer::singleShot(0)). That stray deferred call was
    // the landmine: it could fire inside a modal's nested event loop and realize
    // the native windows under an active modal block, leaving the app input-dead.
    // With it gone, nothing touches winId() while a pre-window modal is up.

    // Eagerly construct the favorites singletons so they load their JSON before any
    // VideoCard is built (safe to call before this, but this guarantees the load
    // happens on the main thread before modules are shown). Two contained stores:
    // YouTube channels and PornHub models.
    SgFavorites::instance();
    SgFavorites::phInstance();

    // First-run Terms of Use: must be accepted before the app is usable. Shown
    // modally BEFORE the window (safe now that the player's deferred winId hookup
    // is gone). Declining quits the app. Closing or Escape counts as declining.
    {
        QSettings cfg(SgPaths::configFile(), QSettings::IniFormat);
        if (!cfg.value("Setup/TermsAccepted", false).toBool()) {
            QDialog dlg(nullptr);
            dlg.setWindowTitle("Seagull - Terms of Use");
            dlg.resize(560, 480);
            auto* lay = new QVBoxLayout(&dlg);
            auto* view = new QTextBrowser(&dlg);
            view->setOpenExternalLinks(true);
            QFile f(":/docs/DISCLAIMER.md");
            view->setMarkdown(f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()) : QString());
            lay->addWidget(view);
            auto* buttons = new QDialogButtonBox(&dlg);
            buttons->addButton("I Agree", QDialogButtonBox::AcceptRole);
            buttons->addButton("Decline", QDialogButtonBox::RejectRole);
            connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
            connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
            lay->addWidget(buttons);
            if (dlg.exec() != QDialog::Accepted) return false; // declined -> main() exits
            cfg.setValue("Setup/TermsAccepted", true);
            cfg.sync();
        }
    }

    QSettings cfg(SgPaths::configFile(), QSettings::IniFormat);
    m_autoUpdateStartup = cfg.value("General/AutoUpdate", true).toBool();
    const bool firstRunTools = SetupDialog::isNeeded();

    // A self-update just relaunched us: it already brought Seagull and the tools
    // current, so the startup check below would be pure redundancy. Honour the flag
    // once, then clear it so the next ordinary launch checks as usual.
    const bool justSelfUpdated = cfg.value("Updates/JustSelfUpdated", false).toBool();
    if (justSelfUpdated) {
        cfg.remove("Updates/JustSelfUpdated");
        cfg.sync();
    }

    // Throttle the automatic startup check to once an hour. The Settings "Check for
    // Updates" button is the manual override and ignores this entirely. (LastChecked
    // is also written by the tool check itself; writing it here means even declining
    // the prompt starts the clock, so a quick relaunch doesn't re-nag.)
    constexpr qint64 kStartupCheckIntervalSecs = 3600;
    const qint64 nowSecs   = QDateTime::currentSecsSinceEpoch();
    const qint64 lastCheck = cfg.value("Updates/LastChecked", 0).toLongLong();
    const bool   checkCooldown = (nowSecs - lastCheck) < kStartupCheckIntervalSecs;

    const bool runStartupCheck = m_autoUpdateStartup && !justSelfUpdated && !checkCooldown;

    // Two-stage updater modal, BEFORE the window (the window stays hidden until it
    // closes), but ONLY when the startup check is due. AutoUpdate means "ask on
    // startup, then run the check on Yes" — it never installs without a prompt. Off
    // (or within the hourly cooldown, or right after a self-update) skips the startup
    // check entirely; the Settings "Check for Updates" button is the only path then.
    // Stage 1 checks Seagull; stage 2 checks the tools — except on first run, where
    // SetupDialog below is the tool stage (runToolStage=false), which also sidesteps
    // re-probing freshly-extracted tool exes (they'd misread their versions and
    // silently re-download). The thumbnail ffmpeg queues stay held until
    // finishStartupUpdates() so a tool swap can't race a running grab.
    m_selfUpdateChosen = false;
    if (runStartupCheck) {
        cfg.setValue("Updates/LastChecked", nowSecs);
        cfg.sync();
        UpdateDialog dlg(appUpdate, updaterWorker, /*autoInstall=*/true,
                         /*skipAsk=*/false, /*runToolStage=*/!firstRunTools, nullptr);
        connect(&dlg, &UpdateDialog::selfUpdateRequested, this,
                [this]() { m_selfUpdateChosen = true; });
        dlg.exec();
    }

    if (m_selfUpdateChosen) {
        // The user accepted a Seagull update at stage 1. Show the window, then run
        // the self-update (its own progress dialog); it relaunches on success, or
        // falls back to running normally on cancel/failure (finishStartupUpdates).
        // The tool/Setup stage waits for the fresh launch.
        m_selfUpdateFromStartup = true;
        mainWindow->show();
        videoPlayer->rebindOutputWindow(); // bind VLC now the render HWND exists
        mediaControls->attachToWindow(reinterpret_cast<void*>(mainWindow->winId()));
        startSelfUpdate();
        return true;
    }

    // First run (or tools missing): folder confirmation + dependency download.
    // This is stage 2 on first run, shown before the window like the rest.
    if (firstRunTools) {
        m_setupActive = true;
        SetupDialog setup(updaterWorker, nullptr);
        setup.exec();
        m_setupActive = false;

        // The Library already scanned with the pre-setup default folders;
        // rescan now that the user confirmed (possibly different) paths.
        libraryModule->refresh();
    }

    // Now reveal the window, bind VLC's output to the render frame's HWND, and bind
    // the Windows media controls to the window HWND (SMTC is per-HWND for desktop
    // apps).
    mainWindow->show();
    videoPlayer->rebindOutputWindow(); // bind VLC now the render HWND exists
    mediaControls->attachToWindow(reinterpret_cast<void*>(mainWindow->winId()));

    // One-time Defender-exclusion offer for users updating from a build that predates
    // the first-run checkbox. Asked once, ever (the flag stops it riding along on every
    // future update). New users already chose it during setup (which sets the same flag),
    // so this only fires when setup wasn't shown this launch. Settings > General is the
    // anytime path. Declining still sets the flag — we don't nag; they can opt in later.
    if (!firstRunTools && !cfg.value("Setup/DefenderExclusionOffered", false).toBool()) {
        QMessageBox box(mainWindow);
        box.setWindowTitle("Speed Up Seagull's Startup");
        box.setIcon(QMessageBox::Question);
        box.setText("Add Seagull to Windows Defender's exclusion list?");
        box.setInformativeText(
            "Windows Defender rescans Seagull's files when you launch it after a restart, "
            "which can make the first start slow. Excluding the app folder lets Seagull "
            "start quickly every time. Windows will ask for permission.\n\n"
            "You can change this anytime from Settings, General.");
        box.setStandardButtons(QMessageBox::Yes | QMessageBox::No);
        box.setDefaultButton(QMessageBox::Yes);
        if (box.exec() == QMessageBox::Yes) {
            // The add verifies what actually persisted; if Tamper Protection blocked it,
            // tell the user how to finish by hand rather than leaving them thinking it worked.
            const SgMediaControls::DefenderResult result = SgMediaControls::addDefenderExclusion();
            if (result == SgMediaControls::DefenderResult::Success)
                cfg.setValue("Setup/DefenderExcluded", true); // Settings reads this back (non-elevated can't)
            const QString message = SgMediaControls::defenderResultMessage(result);
            if (!message.isEmpty()) {
                QMessageBox warn(mainWindow);
                warn.setIcon(QMessageBox::Warning);
                warn.setWindowTitle("Defender Exclusion");
                warn.setText(message);
                QPushButton* openBtn = warn.addButton("Open Windows Security", QMessageBox::ActionRole);
                warn.addButton(QMessageBox::Close);
                warn.exec();
                if (warn.clickedButton() == openBtn) SgMediaControls::openDefenderSettings();
            }
        }
        cfg.setValue("Setup/DefenderExclusionOffered", true);
        cfg.sync();
    }

    for (const QString& kind : cfg.value("Tabs/ExtraTabs").toStringList())
        openDuplicateTab(kind, false /*switchTo*/);

    finishStartupUpdates(); // release thumbnail holds + shut the updater thread

    // Now that the update flow has settled (tool swaps done, yt-dlp.exe stable),
    // start filling the YouTube home feed in the background so it is ready before
    // the user opens the Search tab. Only the primary tab warms; duplicates fill
    // on first show as before.
    searchModule->warmHomeFeed();
    return true;
}

void Seagull::finishStartupUpdates() {
    releaseThumbnailHolds();
    shutdownUpdater(); // the startup flow was the updater's whole job
}

bool Seagull::showAppUpdatePrompt(const QString& version, const QString& notes, const QString& pageUrl) {
    QDialog dlg(mainWindow);
    dlg.setWindowTitle("Update Available");
    dlg.resize(520, 460);
    auto* lay = new QVBoxLayout(&dlg);

    auto* heading = new QLabel(
        QString("Seagull %1 is available. You have %2.")
            .arg(version, QString::fromLatin1(SEAGULL_VERSION)), &dlg);
    QFont hf = heading->font();
    hf.setBold(true);
    hf.setPointSize(hf.pointSize() + 1);
    heading->setFont(hf);
    heading->setWordWrap(true);
    lay->addWidget(heading);

    auto* notesView = new QTextBrowser(&dlg);
    notesView->setOpenExternalLinks(true);
    if (notes.trimmed().isEmpty()) notesView->setPlainText("No release notes were provided.");
    else                           notesView->setMarkdown(notes);
    lay->addWidget(notesView, 1);

    auto* buttons = new QDialogButtonBox(&dlg);
    auto* update = buttons->addButton("Update Now", QDialogButtonBox::AcceptRole);
    auto* page   = buttons->addButton("View on GitHub", QDialogButtonBox::ActionRole);
    buttons->addButton("Later", QDialogButtonBox::RejectRole);
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    connect(page, &QPushButton::clicked, &dlg, [pageUrl]() { QDesktopServices::openUrl(QUrl(pageUrl)); });
    update->setDefault(true);
    lay->addWidget(buttons);

    if (dlg.exec() == QDialog::Accepted) {
        startSelfUpdate(); // download + stage + swap, in-app
        return true;
    }
    return false; // user chose Later / View on GitHub (and closed)
}

void Seagull::startSelfUpdate() {
    m_updateProgress = new QProgressDialog("Downloading update...", "Cancel", 0, 0, mainWindow);
    m_updateProgress->setWindowTitle("Updating Seagull");
    m_updateProgress->setWindowModality(Qt::ApplicationModal);
    m_updateProgress->setMinimumDuration(0);
    m_updateProgress->setAutoClose(false);
    m_updateProgress->setAutoReset(false);
    // Cancel just abandons the in-flight download; the install hasn't been touched.
    connect(m_updateProgress, &QProgressDialog::canceled, this, [this]() {
        if (m_updateProgress) { m_updateProgress->deleteLater(); m_updateProgress = nullptr; }
        // If this was the startup self-update, fall back to running normally
        // (release the thumbnail holds + shut the updater; tools wait for next launch).
        if (m_selfUpdateFromStartup) { m_selfUpdateFromStartup = false; finishStartupUpdates(); }
    });
    m_updateProgress->show();

    // Bring the bundled tools current BEFORE we swap Seagull and relaunch. Tools
    // live in tools/, which the swap helper preserves (robocopy /XD Tools), so
    // doing it now is equivalent to doing it after — minus the redundant check on
    // the fresh launch, which onUpdateReadyToApply then marks to be skipped. Force
    // the check (ignore the cooldown): the user explicitly asked to update.
    m_updateProgress->setLabelText("Updating tools...");
    ensureUpdater(); // the manual path may arrive after the startup updater was shut down
    connect(updaterWorker, &SgUpdater::checkFinished, this,
        [this](const QStringList& pending) {
            if (!m_updateProgress) return; // canceled during the tool check
            if (pending.isEmpty()) { beginSeagullDownload(); return; }
            // Something to install: run it, then move on to the Seagull download.
            connect(updaterWorker, &SgUpdater::applyFinished, this,
                [this](bool /*allOk*/) { beginSeagullDownload(); },
                Qt::ConnectionType(Qt::QueuedConnection | Qt::SingleShotConnection));
            QMetaObject::invokeMethod(updaterWorker, &SgUpdater::applyUpdates, Qt::QueuedConnection);
        },
        Qt::ConnectionType(Qt::QueuedConnection | Qt::SingleShotConnection));
    QMetaObject::invokeMethod(updaterWorker, [u = updaterWorker]() { u->checkForUpdates(true); },
        Qt::QueuedConnection);
}

void Seagull::beginSeagullDownload() {
    if (!m_updateProgress) return; // user canceled during the tool pass
    m_updateProgress->setLabelText("Downloading update...");
    m_updateProgress->setValue(0);
    appUpdate->downloadAndApply();
}

void Seagull::onUpdateReadyToApply(const QString& stagedAppDir) {
    if (!m_updateProgress) return; // user canceled during download — don't apply

    m_updateProgress->setLabelText("Installing update...");
    m_updateProgress->setCancelButton(nullptr); // past the point of no return now

    const QString installDir = QDir::toNativeSeparators(QCoreApplication::applicationDirPath());
    const QString exePath    = QDir::toNativeSeparators(QCoreApplication::applicationFilePath());
    const QString staged     = QDir::toNativeSeparators(stagedAppDir);
    const qint64  pid        = QCoreApplication::applicationPid();

    // The swap helper: waits for us to exit, copies the staged build over the
    // install (preserving the Config folder and the Tools folder),
    // then relaunches and cleans up the staging area. Lives in temp root so the
    // staged-folder cleanup can't delete it mid-run.
    const QString helperPath = QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation))
                                   .filePath(QStringLiteral("seagull_apply.ps1"));
    const QString script = QStringLiteral(
        "param([int]$ProcId,[string]$Staged,[string]$Install,[string]$Exe)\n"
        "try { Wait-Process -Id $ProcId -Timeout 30 -ErrorAction SilentlyContinue } catch {}\n"
        "Start-Sleep -Milliseconds 500\n"
        "robocopy $Staged $Install /E /XD Tools Config | Out-Null\n"
        "Start-Process -FilePath $Exe\n"
        "Remove-Item -LiteralPath (Split-Path $Staged -Parent) -Recurse -Force -ErrorAction SilentlyContinue\n");

    QFile f(helperPath);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (m_updateProgress) { m_updateProgress->deleteLater(); m_updateProgress = nullptr; }
        QMessageBox::warning(mainWindow, "Update Failed", "Could not prepare the update helper.");
        return;
    }
    f.write(script.toUtf8());
    f.close();

    const QStringList args = {
        "-NoProfile", "-ExecutionPolicy", "Bypass", "-WindowStyle", "Hidden",
        "-File", QDir::toNativeSeparators(helperPath),
        "-ProcId", QString::number(pid),
        "-Staged", staged, "-Install", installDir, "-Exe", exePath
    };
    if (!QProcess::startDetached("powershell", args)) {
        if (m_updateProgress) { m_updateProgress->deleteLater(); m_updateProgress = nullptr; }
        QMessageBox::warning(mainWindow, "Update Failed", "Could not launch the update helper.");
        return;
    }

    // Mark this as a fresh self-update so the relaunched instance skips its startup
    // update check — we just brought both Seagull and the tools current, so checking
    // again immediately is pure redundancy. The swap helper preserves Config (/XD
    // Config), so this flag survives into the new launch, which clears it (one-shot).
    {
        QSettings cfg(SgPaths::configFile(), QSettings::IniFormat);
        cfg.setValue("Updates/JustSelfUpdated", true);
        cfg.setValue("Updates/LastChecked", QDateTime::currentSecsSinceEpoch());
        cfg.sync();
    }

    // Hand off: quit so the helper can replace our files and relaunch us.
    qApp->quit();
}

int main(int argc, char* argv[]) {
    // Stamp the process with an AppUserModelID before anything else, so Windows can
    // attribute our SMTC session (otherwise the now-playing card shows no metadata).
    SgMediaControls::registerAppIdentity();

    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("Seagull"));
    QApplication::setApplicationVersion(QString::fromLatin1(SEAGULL_VERSION));

    // Migrate flat config files into Config/ if upgrading from a pre-0.14 install.
    // This must happen before any QSettings is opened so the settings are found at the new path.
    {
        const QString appDir    = QCoreApplication::applicationDirPath();
        const QString configDir = SgPaths::configDir();
        if (!QDir(configDir).exists() && QFile::exists(appDir + "/config.ini")) {
            QDir().mkpath(configDir);
            for (const QString& name : { "config.ini", "search_history.txt",
                                          "search_history_ph.txt", "search_history_cb.txt" }) {
                const QString src = appDir + "/" + name;
                if (QFile::exists(src))
                    QFile::rename(src, configDir + "/" + name);
            }
        }
    }
    // Ensure the Config directory exists even on a clean install (migration above handles
    // upgrades; mkpath is a no-op when the folder is already there).
    QDir().mkpath(SgPaths::configDir());

    // Apply the saved theme before any widgets are built so the whole UI is themed.
    QSettings settings(SgPaths::configFile(), QSettings::IniFormat);
    Theme::apply(settings.value("Display/Theme", "Seagull").toString());

    // Startup guard (not a lifetime single-instance lock). A second instance is fine
    // once we're up, but two *cold* starts launched back-to-back race each other,
    // both hammering the disk/AV loading the same DLLs + VLC plugins and both trying
    // to build a window. Hold a lock across construction + run() so concurrent
    // launches can't overlap; a launch that arrives while another is still starting
    // just exits. The lock is released the instant startup finishes, so a deliberately
    // launched second instance afterward runs normally. A crashed startup leaves a
    // stale lock, which QLockFile clears automatically (dead PID), so this never wedges.
    QLockFile startupLock(QDir(QDir::tempPath()).filePath(QStringLiteral("seagull-startup.lock")));
    if (!startupLock.tryLock(0)) return 0; // another instance is mid-startup

    Seagull orchestrator;
    const bool started = orchestrator.run();
    startupLock.unlock(); // startup done — later launches may run as separate instances
    if (!started) return 0; // user declined the Terms of Use
    return app.exec();
}