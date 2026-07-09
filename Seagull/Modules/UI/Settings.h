#pragma once

#include <QWidget>
#include <QSettings>
#include <QListWidget>
#include <QStackedWidget>
#include <QComboBox>
#include <QLineEdit>
#include <QPushButton>
#include <QButtonGroup>
#include <QSpinBox>
#include <QSlider>
#include <QCheckBox>
#include <QList>

class QFormLayout;
class QVBoxLayout;
class QListWidgetItem;
class QTimer;
class QLabel;
class SgFavorites;

class Settings : public QWidget {
    Q_OBJECT

public:
    explicit Settings(QWidget* parent = nullptr);
    ~Settings();

    // Host the equalizer as an "Audio" page in the sidebar (the EQ widget is owned by
    // the orchestrator, which wires its live-edit signals). Call once at startup.
    void addAudioPage(QWidget* eq);
    void showAudioPage(); // select the Audio sidebar row (used by the player's EQ button)

signals:
    void cardWidthChanged(int width); // Display "Card size" -> Search card width (px)
    void seekBarSizeChanged(int width); // Display "Progress bar size" -> player seek bar width (px)
    void clearHistoryRequested();     // General "Clear History Now" -> Search wipes its history
    void visualizerSettingsChanged(); // Display "Visualizer" -> player re-reads visualizer config
    void checkForUpdatesRequested();  // General "Check Now" -> orchestrator runs the app check
    void audioPageShown();            // Audio (EQ) page became visible -> EQ re-arms auto-follow

protected:
    void showEvent(QShowEvent* event) override; // refresh the home-feed picker on open

private slots:
    void saveSettings();
    void loadSettings();
    void resetDefaults();
    void onCardSizeChanged();            // card-size combo -> show/hide slider + save
    void onAppearanceChanged();          // Light/Dark toggle -> refilter themes + save

private:
    void setupUI();
    void populateThemeCombo(bool dark, const QString& select = QString()); // fill themes of one appearance
    void updateDownloadFormatOptions();  // repopulate format list from the Download Type toggle
    void updateDownloadQualityOptions(); // repopulate quality list (resolutions vs bitrates)
    void onDownloadTypeChanged();        // type toggle -> refresh formats + qualities + save
    QString currentDownloadType() const; // "Video" or "Audio"
    void updateRecordingFormatOptions(); // repopulate record formats from the Recording Type toggle
    void onRecordingTypeChanged();       // recording type toggle -> refresh formats + save
    QString currentRecordingType() const;// "Video" or "Audio"
    int  currentCardWidth() const;       // px from the combo preset, or the slider if Custom
    void browseInto(QLineEdit* edit, const QString& title); // folder picker -> edit
    void applyUnifyState(); // unify toggle -> enable the one row / grey the typed rows
    void onCookiesBrowserChanged(const QString& text); // warn on enable, then save
    void deleteCookieData(); // clear yt-dlp's cached login/session data
    void applySmartSortState(); // smart sort toggle -> show/hide the Downloads Folder row
    void buildHomeSection(QFormLayout* form);  // homepage header + per-site drag-ordered pickers + amount spins
    void rebuildHomePickers();  // repopulate every site's picker from its favourites, in saved priority order
    void saveHomePickerFor(QListWidget* list); // persist one site's order (row order = priority)
    void onDefenderExclusionClicked(); // toggle the Defender exclusion (elevated), then refresh
    void refreshDefenderButton();      // query Defender state -> set Add/Remove Exclusion label
    void onDesktopShortcutClicked();   // toggle the desktop .lnk, then refresh the button
    void onStartMenuShortcutClicked(); // toggle the Start-menu .lnk, then refresh the button
    void refreshShortcutButtons();     // label each shortcut button Add/Remove from its .lnk state

    bool m_loading = false; // suppresses auto-save while loadSettings populates controls

    // What we last pushed to the rest of the app, so a save that didn't touch these
    // doesn't needlessly re-apply the global theme (a full re-polish of every widget)
    // or re-flow the search grid.
    QString m_appliedTheme;
    int     m_appliedCardWidth = -1;
    int     m_appliedSeekBarWidth = -1;

    // Side tab layout components
    QListWidget* sidebar;
    QStackedWidget* stackedWidget;
    int m_audioRow = -1; // sidebar row of the Audio page (set in setupUI)
    QVBoxLayout* m_audioPageLayout = nullptr; // Audio page layout; addAudioPage slots the EQ in at the top

    // General Tab elements
    QCheckBox* autoUpdateCheck;  // install tool updates silently vs ask first
    QCheckBox* rememberPositionCheck; // remember playback position (resume where you left off)
    QPushButton* checkUpdatesBtn; // General "Check Now" -> manual app update check
    QPushButton* defenderExclusionBtn; // add/remove app folder in Defender exclusions (faster cold start)
    bool defenderExcluded = false;     // last known state, drives the button's Add/Remove label
    QPushButton* desktopShortcutBtn;   // add/remove the desktop .lnk
    QPushButton* startMenuShortcutBtn; // add/remove the Start-menu .lnk

    // Display Tab elements
    QButtonGroup* appearanceGroup; // Light | Dark — filters the theme list
    QPushButton*  lightModeBtn;
    QPushButton*  darkModeBtn;
    QComboBox* themeCombo;
    QComboBox* cardSizeCombo;   // Small / Medium / Large / Extra Large / Custom
    QSlider*   cardSizeSlider;  // shown only for Custom; spans Small..Extra Large
    QComboBox* seekBarSizeCombo; // Small / Medium / Large -> player seek bar width

    // Visualizer: a picker + a tight form of global visualizer settings below it.
    QComboBox*      visualizerCombo;      // which visualizer: Seagull Morning / Day / Dusk / Night
    QComboBox*      lighthouseCombo;      // beats per lighthouse flash; row shown only for Night
    QComboBox*      behaviorCombo;        // global gull behaviour (applies to every visualizer)
    QComboBox*      directionCombo;       // which way the flock flies, independent of behaviour
    QSpinBox*       maxGullsSpin;         // global perf cap on the flock size
    QCheckBox*      killGullsCheck;       // gulls die (spin + fall) at end of song, else live on

    // Download & Streaming Tab elements
    QCheckBox*   smartSortCheck;  // route downloads into per-type folders vs one folder
    QWidget*     dlFolderRow;     // the Downloads Folder row (on the Folders page), hidden while smart sort is on
    QButtonGroup* typeGroup;     // Video | Audio toggle
    QPushButton* typeVideoBtn;
    QPushButton* typeAudioBtn;
    QComboBox* formatCombo;
    QComboBox* dlQualityCombo;
    QComboBox* streamQualityCombo;
    QComboBox* cookiesBrowserCombo; // browser to pull YouTube cookies from (anti-bot)
    QPushButton* deleteCookiesBtn;  // wipes yt-dlp's cached cookie-derived data
    QString m_prevCookiesChoice = "None"; // last cookies value, to detect None -> browser (warn)
    QButtonGroup* recTypeGroup;  // Recording: Video | Audio toggle
    QPushButton* recTypeVideoBtn;
    QPushButton* recTypeAudioBtn;
    QComboBox* recFormatCombo;   // Recording container/codec, per the type
    QLineEdit* homeFolderEdit;
    QLineEdit* dlFolderEdit;     // downloads — dedicated, outside the unify system
    QLineEdit* videoFolderEdit;  // video downloads
    QLineEdit* audioFolderEdit;  // audio downloads / extractions
    QLineEdit* photoFolderEdit;  // saved images
    QLineEdit* recFolderEdit;      // where recordings + clips are saved
    QLineEdit* playlistFolderEdit; // where queue playlists (.sgpl) are saved

    // "Unify media folders": ticking swaps the typed folder rows for one
    // Media Folder row (the unified row is hidden until then). Applies only to
    // the media folders (Videos/Audio/Photos/Recordings/Playlists) — Home is untouched.
    QCheckBox* unifyCheck;
    QLineEdit* unifiedFolderEdit;
    QWidget*   unifiedFolderRow;        // the row container, for show/hide
    QList<QWidget*> typedFolderRows;    // the per-type row containers
    QFormLayout* foldersForm = nullptr; // the form the folder rows live in

    // Search Tab elements
    QSpinBox* searchResultsSpin;         // how many results a search fetches
    QFormLayout* searchForm       = nullptr; // the Search page form

    // Homepage section: a per-site collapsible picker you drag to reorder (top row =
    // highest priority), each with its OWN result-count spin folded into the section,
    // plus a per-site videos-per-channel spin.
    struct HomePicker {
        SgFavorites* store;
        QString      cfgKey;            // drag-order priority:    "Search/HomeChannels<Site>"
        QString      amountKey;         // per-site result limit:  "Search/HomeAmount<Site>"
        QString      videosKey;         // per-site videos/channel: "Search/HomeVideosPerChannel<Site>" (empty for Chaturbate)
        QListWidget* list;
        QSpinBox*    amountSpin = nullptr;
        QSpinBox*    videosSpin = nullptr; // null for Chaturbate (live rooms, no per-channel videos)
        QLabel*      warning    = nullptr; // YouTube throttle note (shown when amount > 10); null otherwise
        QString      shuffleKey;           // randomize toggle: "Search/HomeRandomize<Site>" (empty for Chaturbate)
        QPushButton* shuffleBtn = nullptr; // checked = mix by recency (default); unchecked = favourites order. null for Chaturbate
        int          amountDefault = 5;    // per-site default for the "Max homepage videos" spin
        QString      continueKey;          // per-site Continue Watching row: "Search/ShowContinueWatching<Site>"
        QCheckBox*   continueCheck = nullptr; // show this site's Continue Watching row on its home page
        QString      lazyKey;              // per-site lazy-load toggle: "Search/HomeLazyLoad<Site>" (empty for Chaturbate)
        QCheckBox*   lazyCheck = nullptr;  // scroll-to-load-more on this site's home feed (default off, warn on enable)
    };
    QList<HomePicker> m_homePickers;
    bool m_rebuildingPickers = false; // true while rebuildHomePickers repopulates (suppresses save)
    QPushButton* clearHistoryBtn;        // wipe the search history right now
    QCheckBox* clearHistoryOnCloseCheck; // wipe it automatically on every exit

    QPushButton* resetBtn;

    // The INI file handler
    QSettings* iniSettings;
};