#ifndef SEAGULL_H
#define SEAGULL_H

#include <QObject>
#include <QThread>
#include <QStringList>
#include <QList>
#include <QHash>
#include <QSet>
#include <QUrl>
#include <QPair>
#include "Modules/UI/MainWindow.h"
#include "Modules/UI/VideoPlayer.h"
#include "Modules/UI/Queue.h"
#include "Modules/UI/FileExplorer.h"
#include "Modules/UI/MediaLibrary.h"
#include "Modules/UI/Search.h"
#include "Modules/UI/Settings.h"
#include "Modules/UI/EQ.h"
#include "Modules/Backend/SgYtDlp.h"
#include "Modules/Backend/SgSearch.h"
#include "Modules/Backend/SgSpellCheck.h"
#include "Modules/Backend/SgUpdater.h"
#include "Modules/Backend/SgHlsProxy.h"
#include "Modules/Backend/SgMetaCache.h"
#include "Modules/Backend/SgRecorder.h"
#include "Modules/Backend/SgMediaControls.h"
#include "Modules/Backend/SgAppUpdate.h"

class QTextBrowser;
class SgThumbnailer;
class QTimer;
class QProgressDialog;
class QWidget;
class QFrame;
class QLabel;
class QMovie;

class Seagull : public QObject {
    Q_OBJECT
public:
    explicit Seagull(QObject* parent = nullptr);
    ~Seagull();

    bool run(); // false = the user declined the Terms of Use; main() should exit

protected:
    // Watches the Comments page for its first Show (the tab being viewed) so comments
    // are fetched lazily, never while the stream is just starting up.
    bool eventFilter(QObject* obj, QEvent* event) override;

private:
    // Library = the card-grid media library; Explorer = the file-manager tab.
    enum class ActiveSource { None, Library, Explorer, Queue, Search };

    // Sequential downloader for ad-hoc downloads (e.g. Search cards). Files land in
    // the library; the Library tab shows a spinner while the queue drains.
    void pumpDownloads();

    // Brief seagull on the Library tab once a recording/clip is saved + playable.
    void flashLibraryTab();

    // A worker reported the source is blocking us (bot check / HTTP 429 throttling).
    // Shows one warning modal, then stays quiet for a cooldown: several workers can
    // trip at once and retries recur, so we debounce to a single nag.
    void onExtractionBlocked(const QString& kind, const QString& detail);
    bool   m_blockWarnActive = false; // a block warning is on screen right now
    qint64 m_lastBlockWarnMs = 0;     // when the last one was dismissed (cooldown gate)

    MainWindow* mainWindow;
    VideoPlayer* videoPlayer;  // the playback feature, hosted by the shell window
    Queue* queueModule;
    MediaLibrary* libraryModule;   // card-grid view of the saved-media folders
    FileExplorer* explorerModule;  // the file-manager tab (was "Library")
    Search* searchModule;
    Settings* settingsModule;
    EQ* eqModule;                  // 10-band equalizer (per-kind presets); embedded as the Settings "Audio" page
    QTextBrowser* descriptionView; // the dynamic "Description" tab's page
    QTextBrowser* commentsView;    // the dynamic "Comments" tab's page (opens right of Description)
    QWidget* commentsContainer = nullptr; // wraps commentsView + a bottom "loading" pill (the dynamic tab)
    QFrame*  m_commentsStatusPill  = nullptr; // bottom pill shown while a comment fetch is in flight
    QLabel*  m_commentsStatusLabel = nullptr;
    QLabel*  m_commentsSpinner     = nullptr; // animated spinner inside the pill
    QMovie*  m_commentsMovie       = nullptr;
    void setCommentsStatus(const QString& text, bool busy); // drive the bottom loading pill
    // The comment fetch + JSON parse run on commentsThread (off the GUI thread); the
    // parsed comments are rendered into the view in small batches so the layout never
    // freezes. The first batch is preloaded the moment the tab becomes available (the
    // fetch is on its own worker thread, so it doesn't stall the stream), so the list
    // is usually ready by the time the user clicks the Comments tab.
    void loadCommentsLazy();        // first fetch for this video (preload on tab availability)
    void requestComments();         // (re)issue the fetch for the current window size
    void renderCommentBatch();      // append the next chunk of (collapsed) comment threads
    void resetCommentsState();      // clear per-video comment state + abort any fetch
    // Replies are hidden behind a "View N replies" toggle (YouTube-style). yt-dlp returns
    // replies inline in the same batch (each carries a `parent` id), so we group them
    // under their parent and reveal on click — no extra fetch. A toggle re-renders the
    // whole list from m_commentThreads (cheap; it's occasional) preserving scroll.
    //
    // We store the raw (theme-independent) pieces of each comment, NOT pre-coloured HTML,
    // so every colour is resolved fresh at render time. That keeps the cards in step with
    // the active theme, and a palette change just re-renders (see eventFilter).
    struct CommentEntry {
        QString authorHtml;         // escaped author name
        bool    isUploader = false; // show the accent "(creator)" badge
        QString metaText;           // "N likes &middot; date" (entities/digits only, no colour)
        QString textHtml;           // escaped, pre-wrapped comment body
    };
    struct CommentThread {
        QString id;                 // the top-level comment's id (toggle anchor target)
        CommentEntry comment;       // the top-level comment itself
        QList<CommentEntry> replies;// hidden until the thread is expanded
        bool expanded = false;      // is this thread's reply list currently shown?
    };
    QString commentEntryHtml(const CommentEntry& e) const;   // author line + body (themed)
    QString commentThreadHtml(const CommentThread& t) const; // one bordered comment card
    void rerenderComments();        // rebuild the whole list (reply toggle / theme change)
    void toggleCommentThread(const QString& id); // flip one thread's replies open/closed
    QString m_commentsPageUrl;      // page URL to fetch comments for (current online VOD)
    int     m_commentCount = 0;     // comment_count from the probe (gates the tab)
    bool    m_commentsFetched = false; // first fetch issued for this video?
    QList<CommentThread> m_commentThreads; // top-level comments (with grouped replies), in order
    QHash<QString, int>  m_commentThreadIndex; // comment id -> index in m_commentThreads
    QString m_commentsHeaderHtml;   // "<h3>Comments (N)</h3><hr>" — reused on re-render
    int     m_commentRenderIdx = 0; // how many of m_commentThreads are on screen
    QTimer* m_commentRenderTimer = nullptr; // drips the remaining threads in after the first
    QThread* commentsThread = nullptr;      // commentsWorker lives here (off the GUI thread)
    // "Load more" pagination: yt-dlp has no offset, so each scroll-to-bottom re-requests
    // a bigger window; we de-dupe by comment id so re-fetched comments aren't repeated.
    QSet<QString> m_commentSeenIds; // comment ids already on screen
    int  m_commentsRequested = 0;   // current max_comments window we asked yt-dlp for
    bool m_commentsLoadingMore = false; // a load-more fetch is in flight
    bool m_commentsAllLoaded = false;   // reached the end (got fewer than requested)
    ActiveSource activeSource = ActiveSource::None;

    // Multiple-instance tabs (Search, File Explorer). The primary instances above
    // are the first entry in each list; the rest are runtime duplicates opened from
    // the "+" menu. m_active* is whichever instance last drove playback, so skip /
    // shorts-scroll walk the right tab's feed. Each duplicate Search owns its own
    // SgSearch worker (tracked here so it's deleted with the tab).
    QList<Search*>       m_searchTabs;
    Search*              m_activeSearch = nullptr;
    QHash<Search*, SgSearch*> m_searchWorkers; // duplicate Search -> its worker
    QList<FileExplorer*> m_explorerTabs;
    FileExplorer*        m_activeExplorer = nullptr;

    // Shorts prefetch: while a short plays, resolve the next two shorts' CDN streams on
    // shortsPrefetcher and hold them, so scrolling stays instant even when you move faster
    // than one resolve (same CDN-passthrough the Queue prefetch uses). Forward-only —
    // that's the dominant scroll direction; a back-scroll resolves on demand. The two are
    // fetched SEQUENTIALLY (never parallel yt-dlp — that's a bot tell and the worker only
    // runs one job anyway), each launch behind a small jittered debounce so the background
    // requests don't march at a fixed metronome.
    static constexpr int kShortsLookahead = 2;
    QHash<QString, QPair<QUrl, QUrl>> m_shortsReady; // page URL -> resolved {video, audio} CDN
    QStringList m_shortsWant;    // upcoming page URLs still needing a fetch (in feed order)
    QString     m_shortsBusyUrl; // URL the prefetcher is resolving now ("" = idle)
    bool        m_shortsScheduled = false;          // a debounced launch is pending
    bool        m_shortsPrefetchCancelling = false; // guard: ignore our own cancel()'s finished()
    QTimer*     m_shortsDebounceTimer = nullptr;    // jittered delay before each fetch launches
    // Watchdog: a resolve that finds no playable format emits neither streamUrlReady nor
    // finished, which would latch m_shortsBusyUrl and stall the feature. If a fetch hasn't
    // reported back in time, abandon it (the short still resolves on demand when scrolled to).
    QTimer*     m_shortsWatchdogTimer = nullptr;
    void armShortsPrefetch(const QStringList& upcoming); // set the lookahead window from the feed
    void pumpShortsPrefetch();                            // schedule the next fetch if idle
    void clearShortsPrefetch();                           // drop state + abandon any in-flight fetch
    void cancelShortsFetch();                             // guarded cancel() of the in-flight job

    void wireSearchTab(Search* s);          // connect a Search instance's signals
    void wireExplorerTab(FileExplorer* e);  // connect a FileExplorer instance's signals
    void openDuplicateTab(const QString& kind, bool switchTo = true); // "+" -> build + wire + add a new instance
    void disposeDuplicateTab(QWidget* page);       // tab closed -> delete the instance (+ worker)

    // The orchestrator owns every backend worker and hands them out to modules.
    SgYtDlp* downloaderWorker;
    SgYtDlp* resolverWorker;
    SgYtDlp* prefetcherWorker;
    SgYtDlp* playerWorker;     // dedicated to the player's probe/stream-url traffic
    SgYtDlp* downloadWorker;   // dedicated to ad-hoc (Search card) downloads
    SgYtDlp* commentsWorker;   // dedicated to the slow, paginated comments fetch
    SgYtDlp* shortsPrefetcher; // dedicated to resolving the NEXT short ahead of the scroll
    SgSearch* searchWorker;    // backend for the Search tab (discovery)
    SgSpellCheck* spellChecker; // shared OS spell checker for the text fields

    // One shared localhost proxy that strips Twitch's stitched ad segments from the
    // live HLS manifest before VLC sees them. Handed to every resolve worker.
    SgHlsProxy* hlsProxy;

    // One shared yt-dlp -J cache across all workers, so the queue title-resolver,
    // the prefetcher, and the player don't each re-extract the same video (the
    // duplicate request burst is a bot-detection trigger).
    SgMetaCache* metaCache;

    // Records the currently-playing live stream to disk (parallel ffmpeg).
    SgRecorder* recorder;

    // Mirrors playback to the Windows media controls (SMTC) and routes the OS
    // media-key / overlay buttons back to the player. A timer pushes the timeline.
    SgMediaControls* mediaControls;
    QTimer* smtcTimelineTimer;
    void skipActive(int delta); // shared by the skip buttons + the SMTC next/prev keys

    // Settings namespace for the content type currently playing (e.g.
    // "search.youtube", "library.image"). Drives the per-type autoplay/shuffle
    // toggles and the slideshow chrome via MainWindow::setPlaybackContext.
    QString currentContextKey() const;

    // Photo slideshow: in photo mode with autoplay on, advance to the next photo
    // every N seconds (the MainWindow spin box). Re-armed each time a photo loads.
    QTimer* slideshowTimer = nullptr;
    bool    m_currentIsPhoto = false;
    void    updateSlideshow(); // (re)arm or stop the timer for the current state

    // Checks GitHub Releases for a newer Seagull build and, if found, prompts the
    // user with the release notes + a button to open the download page.
    SgAppUpdate* appUpdate;
    bool m_appCheckManual = false;        // user pressed "Check for Updates" (Settings); show "up to date" too
    bool m_autoUpdateStartup = true;      // cached AutoUpdate setting for the startup flow
    bool m_selfUpdateFromStartup = false; // a startup-initiated self-update is downloading
    bool m_selfUpdateChosen = false;      // the startup UpdateDialog requested a Seagull self-update
    // The Settings manual "Check for Updates" prompt (rich notes + open-page). The
    // startup Seagull check is owned by the UpdateDialog instead.
    bool showAppUpdatePrompt(const QString& version, const QString& notes, const QString& pageUrl);

    // Startup update flow runs entirely inside run(): the two-stage UpdateDialog
    // (Seagull then tools) and, on first run, SetupDialog as the tool stage.
    void finishStartupUpdates(); // release thumbnail holds + shut the updater thread

    // Self-update: download + stage the new build (progress dialog), then launch a
    // helper that swaps the files once we exit and relaunches us.
    QProgressDialog* m_updateProgress = nullptr;
    void ensureUpdater();          // (re)create the updater worker+thread if shut down
    void startSelfUpdate();        // bring tools current, THEN download/stage Seagull
    void beginSeagullDownload();   // second half of startSelfUpdate, after the tool pass
    void onUpdateReadyToApply(const QString& stagedAppDir);

    // Answers the player's local-file poster requests (frame grab / cover art).
    // Held with the Library's thumbnailer until the startup update modal is done.
    SgThumbnailer* playerThumbnailer;

    // Release the thumbnail ffmpeg queues once tool updates can no longer race them.
    void releaseThumbnailHolds();

    // The updater's only job is the startup flow; once that's done, stop its
    // thread instead of letting it idle for the whole session.
    void shutdownUpdater();

    QStringList m_downloadQueue; // pending ad-hoc download URLs (FIFO)
    bool        m_downloading = false;
    bool        m_setupActive = false; // first-run dialog owns the updater right now

    // The tool updater does slow, blocking work (network fetches, hashing, unzip),
    // so it gets its own thread to keep startup and the UI snappy.
    SgUpdater* updaterWorker;
    QThread* updaterThread;
};

#endif