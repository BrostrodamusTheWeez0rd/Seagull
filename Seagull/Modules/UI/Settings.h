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
class QListWidgetItem;
class QTimer;
class SgFavorites;

class Settings : public QWidget {
    Q_OBJECT

public:
    explicit Settings(QWidget* parent = nullptr);
    ~Settings();

signals:
    void cardWidthChanged(int width); // Display "Card size" -> Search card width (px)
    void seekBarSizeChanged(int width); // Display "Progress bar size" -> player seek bar width (px)
    void clearHistoryRequested();     // General "Clear History Now" -> Search wipes its history
    void visualizerSettingsChanged(); // Display "Visualizer" -> player re-reads visualizer config
    void checkForUpdatesRequested();  // General "Check Now" -> orchestrator runs the app check

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
    void buildHomeSection(QFormLayout* form);  // homepage header + per-site ranked pickers + amount spins
    void rebuildHomePickers();  // repopulate every site's ranked picker from its favourites + saved order
    void saveHomePickerFor(QListWidget* list); // persist one site's ranked URL order
    void toggleHomeRank(QListWidget* list, QListWidgetItem* item);  // click: add next rank / remove + compact
    void promoteHomeRank(QListWidget* list, QListWidgetItem* item); // double-click: swap up one rank
    void onDefenderExclusionClicked(); // toggle the Defender exclusion (elevated), then refresh
    void refreshDefenderButton();      // query Defender state -> set Add/Remove Exclusion label

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

    // General Tab elements
    QCheckBox* autoUpdateCheck;  // install tool updates silently vs ask first
    QPushButton* checkUpdatesBtn; // General "Check Now" -> manual app update check
    QPushButton* defenderExclusionBtn; // add/remove app folder in Defender exclusions (faster cold start)
    bool defenderExcluded = false;     // last known state, drives the button's Add/Remove label

    // Display Tab elements
    QButtonGroup* appearanceGroup; // Light | Dark — filters the theme list
    QPushButton*  lightModeBtn;
    QPushButton*  darkModeBtn;
    QComboBox* themeCombo;
    QComboBox* cardSizeCombo;   // Small / Medium / Large / Extra Large / Custom
    QSlider*   cardSizeSlider;  // shown only for Custom; spans Small..Extra Large
    QComboBox* seekBarSizeCombo; // Small / Medium / Large -> player seek bar width

    // Visualizer: a picker + a tight form of global visualizer settings below it.
    QComboBox*      visualizerCombo;      // which visualizer: Seagull Sky / Seagull Waves
    QComboBox*      behaviorCombo;        // global gull behaviour (applies to every visualizer)
    QSpinBox*       maxGullsSpin;         // global perf cap on the flock size
    QCheckBox*      killGullsCheck;       // gulls die (spin + fall) at end of song, else live on

    // Download & Streaming Tab elements
    QCheckBox*   smartSortCheck;  // route downloads into per-type folders vs one folder
    QWidget*     dlFolderRow;     // the Downloads Folder row, hidden while smart sort is on
    QFormLayout* dlForm = nullptr; // the Download tab form (show/hide the folder row on it)
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

    // Homepage section: a per-site collapsible ranked picker (click an item to add the
    // next 1..5 number, double-click to move it up) plus the two amount spins.
    struct HomePicker { SgFavorites* store; QString cfgKey; QListWidget* list; };
    QList<HomePicker> m_homePickers;
    QSpinBox* homeChannelAmountSpin = nullptr; // how many ranked channels feed the home page
    QSpinBox* homeBatchSpin         = nullptr; // how many videos per channel
    QTimer*   m_homeClickTimer      = nullptr; // disambiguates single-click (rank) from double-click (promote)
    QListWidget* m_pendingList      = nullptr; // the list whose click is pending
    QString   m_pendingUrl;                    // url of the item whose single-click is pending
    qint64    m_lastPickDblMs       = 0;       // when the last double-click landed (swallow its trailing click)
    QPushButton* clearHistoryBtn;        // wipe the search history right now
    QCheckBox* clearHistoryOnCloseCheck; // wipe it automatically on every exit

    QPushButton* resetBtn;

    // The INI file handler
    QSettings* iniSettings;
};