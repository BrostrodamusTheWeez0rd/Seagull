#include "Settings.h"
#include "Theme.h"
#include "Widgets/PlayerControls.h" // widthForSize: Progress bar size -> px
#include "../Backend/SgPaths.h"
#include "../Backend/SgFavorites.h" // home-feed channel picker source
#include "../Backend/SgMediaControls.h" // addDefenderExclusion (startup speed-up)
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QFormLayout>
#include <QFileDialog>
#include <QCoreApplication>
#include <QStandardPaths>
#include <QGroupBox>
#include <QPushButton>
#include <QTabWidget>
#include <QTextBrowser>
#include <QLabel>
#include <QFont>
#include <QFile>
#include <QRegularExpression>
#include <QMessageBox>
#include <QCheckBox>
#include <QProcess>
#include <QSettings>
#include <QTimer>
#include <QApplication>
#include <QPainter>
#include <QStyledItemDelegate>
#include <QListWidgetItem>
#include <QMap>
#include <QDateTime>

// The build stamps this in (see CMakeLists). Fallback keeps a stray build compiling.
#ifndef SEAGULL_VERSION
#define SEAGULL_VERSION "dev"
#endif

namespace {
// The home-feed picker stores each row's channel URL; the row order IS the priority
// (top = highest), set by dragging.
constexpr int kUrlRole = Qt::UserRole;

// Load a bundled resource doc as text ("" if missing).
QString readDoc(const QString& resourcePath) {
    QFile f(resourcePath);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return QString();
    return QString::fromUtf8(f.readAll());
}

// Card-size presets: the named sizes are points on one continuous slider, so the
// Custom slider can land on any width in between.
struct CardPreset { const char* name; int px; };
const CardPreset kCardPresets[] = {
    { "Small", 180 }, { "Medium", 240 }, { "Large", 300 }, { "Extra Large", 360 }
};
constexpr int kCardMinPx = 180; // == Small
constexpr int kCardMaxPx = 360; // == Extra Large

int presetWidth(const QString& name) {
    for (const auto& p : kCardPresets) if (name == p.name) return p.px;
    return 240; // Medium fallback
}
QString presetName(int px) {
    for (const auto& p : kCardPresets) if (px == p.px) return QString::fromLatin1(p.name);
    return QString(); // not a preset -> Custom
}
}

Settings::Settings(QWidget* parent) : QWidget(parent) {
    iniSettings = new QSettings(SgPaths::configFile(), QSettings::IniFormat, this);

    setupUI();
    loadSettings();
}

Settings::~Settings() {
    // iniSettings is parented to 'this', so it cleans up automatically
}

void Settings::addAudioPage(QWidget* eq) {
    if (!eq || !m_audioPageLayout) return;
    // The Audio page container is built in setupUI; the EQ is its only content.
    m_audioPageLayout->addWidget(eq);
}

void Settings::showAudioPage() {
    if (m_audioRow < 0 || !sidebar) return;
    if (sidebar->currentRow() == m_audioRow)
        emit audioPageShown();                 // already here: setCurrentRow won't re-fire, so arm directly
    else
        sidebar->setCurrentRow(m_audioRow);    // row change -> currentRowChanged -> audioPageShown
}

void Settings::setupUI() {
    auto* outerLayout = new QVBoxLayout(this);
    auto* mainLayout = new QHBoxLayout();
    outerLayout->addLayout(mainLayout);

    // --- 1. Left Sidebar ---
    sidebar = new QListWidget(this);
    sidebar->setMaximumWidth(200);
    sidebar->addItem("General");
    sidebar->addItem("Appearance");
    sidebar->addItem("Audio");
    sidebar->addItem("Downloads & Recording");
    sidebar->addItem("Folders");
    sidebar->addItem("Search & Home");
    sidebar->addItem("Info");
    // Simple styling for a flat, modern look
    sidebar->setStyleSheet(
        "QListWidget { border: none; background-color: transparent; outline: none; }"
        "QListWidget::item { padding: 10px; border-radius: 5px; }"
        "QListWidget::item:selected { background-color: palette(highlight); color: palette(highlighted-text); }"
    );

    // --- 2. Right Content Area ---
    stackedWidget = new QStackedWidget(this);

    // === General Tab ===
    auto* generalWidget = new QWidget();
    auto* generalLayout = new QFormLayout(generalWidget);
    generalLayout->setContentsMargins(20, 20, 20, 20);

    autoUpdateCheck = new QCheckBox("Keep tools up to date automatically");
    autoUpdateCheck->setToolTip("Install yt-dlp / ffmpeg / Deno / AtomicParsley updates in the background "
        "at startup. When off, Seagull asks before updating.");
    generalLayout->addRow("Auto Update:", autoUpdateCheck);

    rememberPositionCheck = new QCheckBox("Resume videos and audio where I left off");
    rememberPositionCheck->setToolTip("Remember how far you watched or listened, and pick back up from "
        "there next time. When off, nothing is remembered and Continue Watching rows stay empty.");
    generalLayout->addRow("Playback:", rememberPositionCheck);
    connect(rememberPositionCheck, &QCheckBox::toggled, this, &Settings::saveSettings);

    // Browser cookies: the most effective fix for the "confirm you're not a bot" wall.
    // When set, yt-dlp reuses that browser's logged-in session so the requests look like
    // a normal viewer. Off by default; turning it on pops a warning about not using your
    // main account (see onCookiesBrowserChanged). Firefox only — Chromium browsers encrypt
    // their cookie store in a way yt-dlp can't read on current Windows builds.
    cookiesBrowserCombo = new QComboBox();
    cookiesBrowserCombo->addItems({ "None", "Firefox" });
    cookiesBrowserCombo->setToolTip(
        "Reuse Firefox's login to cut down on \"confirm you're not a bot\" errors. "
        "yt-dlp can read Firefox's cookies reliably; Chromium browsers (Chrome, Edge, "
        "Brave) encrypt theirs in a way it can't currently read on Windows. Use a spare "
        "account, never your main one.");
    generalLayout->addRow("Cookies from browser:", cookiesBrowserCombo);

    deleteCookiesBtn = new QPushButton("Delete Cached Cookie Data");
    deleteCookiesBtn->setMaximumWidth(220);
    deleteCookiesBtn->setToolTip("Clears yt-dlp's cache, including any login/session tokens it "
        "derived from your browser cookies. Use this after turning cookies off to leave nothing behind.");
    generalLayout->addRow("", deleteCookiesBtn);

    checkUpdatesBtn = new QPushButton("Check for Updates");
    checkUpdatesBtn->setToolTip("Check now for a newer version of Seagull.");
    generalLayout->addRow("", checkUpdatesBtn);
    connect(checkUpdatesBtn, &QPushButton::clicked, this, &Settings::checkForUpdatesRequested);

    defenderExclusionBtn = new QPushButton("Add Exclusion");
    defenderExclusionBtn->setToolTip("Add Seagull to Windows Defender's exclusion list so it "
        "stops rescanning the app's files on every launch. This is the main cause of a slow "
        "first start after a restart. Windows will ask for permission. If the exclusion is "
        "already set, this button removes it instead.");
    generalLayout->addRow("Defender:", defenderExclusionBtn);
    connect(defenderExclusionBtn, &QPushButton::clicked, this, &Settings::onDefenderExclusionClicked);
    // The label's Add/Remove state is resolved lazily in showEvent (the query is slow
    // to start, so we keep it off the cold-start path).

    // Shortcuts: the same offer the first-run setup makes, available any time here so
    // a user who declined (or wants one back) isn't stuck. One button per location;
    // each toggles Add/Remove based on whether its .lnk currently exists.
    desktopShortcutBtn = new QPushButton("Add Desktop Shortcut");
    desktopShortcutBtn->setToolTip("Create (or remove) a Seagull shortcut on your desktop.");
    startMenuShortcutBtn = new QPushButton("Add Start Menu Shortcut");
    startMenuShortcutBtn->setToolTip("Create (or remove) a Seagull shortcut in your Start menu. "
        "This also gives Windows Seagull's name and icon on the media controls card.");
    auto* shortcutRow = new QHBoxLayout();
    shortcutRow->setContentsMargins(0, 0, 0, 0);
    shortcutRow->addWidget(desktopShortcutBtn);
    shortcutRow->addWidget(startMenuShortcutBtn);
    shortcutRow->addStretch();
    generalLayout->addRow("Shortcuts:", shortcutRow);
    connect(desktopShortcutBtn, &QPushButton::clicked, this, &Settings::onDesktopShortcutClicked);
    connect(startMenuShortcutBtn, &QPushButton::clicked, this, &Settings::onStartMenuShortcutClicked);

    stackedWidget->addWidget(generalWidget);

    // === Appearance Tab ===
    auto* displayWidget = new QWidget();
    auto* displayLayout = new QFormLayout(displayWidget);
    displayLayout->setContentsMargins(20, 20, 20, 20);

    // Appearance toggle (Light | Dark): filters the Theme list below to just the
    // themes of that kind, so the menu isn't a mixed bag.
    lightModeBtn = new QPushButton("Light");
    darkModeBtn  = new QPushButton("Dark");
    QString appearanceStyle =
        "QPushButton { padding: 6px 18px; }"
        "QPushButton:checked { background-color: palette(highlight); color: palette(highlighted-text); }";
    for (auto* b : { lightModeBtn, darkModeBtn }) { b->setCheckable(true); b->setStyleSheet(appearanceStyle); }
    appearanceGroup = new QButtonGroup(this);
    appearanceGroup->setExclusive(true);
    appearanceGroup->addButton(lightModeBtn);
    appearanceGroup->addButton(darkModeBtn);
    auto* appearanceRow = new QHBoxLayout();
    appearanceRow->setContentsMargins(0, 0, 0, 0);
    appearanceRow->addWidget(lightModeBtn);
    appearanceRow->addWidget(darkModeBtn);
    appearanceRow->addStretch();
    displayLayout->addRow("Appearance:", appearanceRow);

    themeCombo = new QComboBox();
    themeCombo->setToolTip("Colour theme. The Appearance toggle above filters this to "
        "light or dark themes.");
    displayLayout->addRow("Theme:", themeCombo);

    cardSizeCombo = new QComboBox();
    cardSizeCombo->addItems({ "Small", "Medium", "Large", "Extra Large", "Custom" });
    cardSizeCombo->setToolTip("Target card size — cards grow to fill the row from this. "
        "Smaller = more per row. Custom reveals a slider for in-between sizes.");
    displayLayout->addRow("Card size:", cardSizeCombo);

    // Custom slider: spans Small..Extra Large with a tick at each named size. Only
    // shown when "Custom" is selected.
    cardSizeSlider = new QSlider(Qt::Horizontal);
    cardSizeSlider->setRange(kCardMinPx, kCardMaxPx);
    cardSizeSlider->setSingleStep(10);
    cardSizeSlider->setPageStep(60);
    cardSizeSlider->setTickInterval(60); // ticks land on Small/Medium/Large/Extra Large
    cardSizeSlider->setTickPosition(QSlider::TicksBelow);
    cardSizeSlider->hide();
    displayLayout->addRow("", cardSizeSlider);

    // Player seek bar width: a static size (no dynamic growth). Larger hands the
    // seeker more pixels for finer scrubbing; the player re-centres it over the video.
    seekBarSizeCombo = new QComboBox();
    seekBarSizeCombo->addItems({ "Small", "Medium", "Large" });
    seekBarSizeCombo->setToolTip("Width of the player's seek bar. Larger gives finer "
        "scrubbing control.");
    displayLayout->addRow("Progress bar size:", seekBarSizeCombo);

    // Visualizer picker + its settings. Tied to audio playback, but it's a visual
    // customization, so it lives here on Appearance rather than on the Audio (EQ) page.
    visualizerCombo = new QComboBox();
    visualizerCombo->addItems({ "Seagull Morning", "Seagull Waves", "Seagull Night" });
    visualizerCombo->setToolTip("Which visualizer the player's visualizer button shows for audio.");
    displayLayout->addRow("Visualizer:", visualizerCombo);

    // All visualizer settings in one tight form folded under the picker. Behaviour, the
    // cap, and end-of-song are all global — they apply to whichever visualizer is selected.
    auto* vizBlock = new QWidget();
    auto* vizForm = new QFormLayout(vizBlock);
    vizForm->setContentsMargins(0, 0, 0, 0);

    behaviorCombo = new QComboBox();
    behaviorCombo->addItems({ "Drift", "Reverse", "Swooping", "Flocking" });
    behaviorCombo->setToolTip("How the seagulls move: Drift (left to right), "
        "Reverse (right to left), Swooping, or Flocking.");
    vizForm->addRow("Seagull behavior:", behaviorCombo);

    // Lighthouse flash cadence — only meaningful where the lamp is lit, so the
    // row appears dynamically when the night visualizer is selected.
    lighthouseCombo = new QComboBox();
    lighthouseCombo->addItem("Every beat", 1);
    lighthouseCombo->addItem("Every 2 beats", 2);
    lighthouseCombo->addItem("Every 4 beats", 4);
    lighthouseCombo->addItem("Every 8 beats", 8);
    lighthouseCombo->setToolTip("How often the lighthouse beam faces you and flashes. "
        "It spins at the song's tempo either way.");
    vizForm->addRow("Lighthouse flash:", lighthouseCombo);
    auto updateLighthouseRow = [this, vizForm]() {
        const bool lit = visualizerCombo->currentText().contains("Night");
        lighthouseCombo->setVisible(lit);
        if (QWidget* label = vizForm->labelForField(lighthouseCombo)) label->setVisible(lit);
    };
    connect(visualizerCombo, &QComboBox::currentTextChanged, this, updateLighthouseRow);
    updateLighthouseRow();

    maxGullsSpin = new QSpinBox();
    maxGullsSpin->setRange(2, 24);
    maxGullsSpin->setToolTip("Maximum number of seagulls on screen. Lower it if the visualizer "
        "is heavy on your machine.");
    vizForm->addRow("Max seagulls:", maxGullsSpin);

    killGullsCheck = new QCheckBox("Seagulls fall from the sky at the end of a song");
    killGullsCheck->setToolTip("When on, the flock spins and falls when a song ends. "
        "When off, the seagulls keep flying.");
    vizForm->addRow("End of song:", killGullsCheck);

    displayLayout->addRow("", vizBlock);

    stackedWidget->addWidget(displayWidget);

    // === Audio Tab ===
    // Just the equalizer, added at runtime via addAudioPage. The container keeps the
    // sidebar/stacked index alignment so addAudioPage doesn't have to juggle indices.
    auto* audioPage = new QWidget();
    m_audioPageLayout = new QVBoxLayout(audioPage);
    m_audioPageLayout->setContentsMargins(20, 20, 20, 20);

    m_audioRow = 2; // sidebar row of the Audio page (the EQ is inserted here)
    stackedWidget->addWidget(audioPage);

    // === Downloads & Recording Tab ===
    auto* dlWidget = new QWidget();
    auto* dlLayout = new QFormLayout(dlWidget);
    dlLayout->setContentsMargins(20, 20, 20, 20);

    // Download Type toggle (Video | Audio) — drives which formats are offered.
    typeVideoBtn = new QPushButton("Video");
    typeAudioBtn = new QPushButton("Audio");
    QString segStyle =
        "QPushButton { padding: 6px 18px; }"
        "QPushButton:checked { background-color: palette(highlight); color: palette(highlighted-text); }";
    for (auto* b : { typeVideoBtn, typeAudioBtn }) { b->setCheckable(true); b->setStyleSheet(segStyle); }
    typeVideoBtn->setChecked(true);
    typeGroup = new QButtonGroup(this);
    typeGroup->setExclusive(true);
    typeGroup->addButton(typeVideoBtn);
    typeGroup->addButton(typeAudioBtn);
    auto* typeRow = new QHBoxLayout();
    typeRow->setContentsMargins(0, 0, 0, 0);
    typeRow->addWidget(typeVideoBtn);
    typeRow->addWidget(typeAudioBtn);
    typeRow->addStretch();

    formatCombo = new QComboBox();
    formatCombo->setMaxVisibleItems(8);
    // Format + quality lists are populated from the Download Type below.
    updateDownloadFormatOptions();

    dlQualityCombo = new QComboBox();
    dlQualityCombo->setMaxVisibleItems(8);
    updateDownloadQualityOptions();

    streamQualityCombo = new QComboBox();
    streamQualityCombo->addItems({
        "Best Available",
        "2160p (4K)", "1440p (2K)", "1080p", "720p", "480p", "360p"
        });
    streamQualityCombo->setMaxVisibleItems(8);

    // Recording Type toggle (Video | Audio) — what the player's Record button captures.
    recTypeVideoBtn = new QPushButton("Video");
    recTypeAudioBtn = new QPushButton("Audio");
    for (auto* b : { recTypeVideoBtn, recTypeAudioBtn }) { b->setCheckable(true); b->setStyleSheet(segStyle); }
    recTypeVideoBtn->setChecked(true);
    recTypeGroup = new QButtonGroup(this);
    recTypeGroup->setExclusive(true);
    recTypeGroup->addButton(recTypeVideoBtn);
    recTypeGroup->addButton(recTypeAudioBtn);
    auto* recTypeRow = new QHBoxLayout();
    recTypeRow->setContentsMargins(0, 0, 0, 0);
    recTypeRow->addWidget(recTypeVideoBtn);
    recTypeRow->addWidget(recTypeAudioBtn);
    recTypeRow->addStretch();

    recFormatCombo = new QComboBox();
    recFormatCombo->setMaxVisibleItems(8);
    recFormatCombo->setToolTip("Container for recordings and clips. MKV survives an "
        "interrupted recording best; MP4 is the most widely compatible.");
    updateRecordingFormatOptions();

    // Folder rows: read-only path edit + Browse button, one per media type. Each
    // row is a widget so the unify toggle can enable/disable it as one unit.
    // (Read-only so users can't type invalid paths manually.)
    auto makeFolderRow = [this](QLineEdit*& edit, const QString& title, const QString& tip) {
        auto* row = new QWidget();
        auto* lay = new QHBoxLayout(row);
        lay->setContentsMargins(0, 0, 0, 0);
        edit = new QLineEdit();
        edit->setReadOnly(true);
        edit->setToolTip(tip);
        auto* btn = new QPushButton("Browse...");
        lay->addWidget(edit);
        lay->addWidget(btn);
        QLineEdit* target = edit; // capture the pointer value, not the local ref
        connect(btn, &QPushButton::clicked, this, [this, target, title]() {
            browseInto(target, title);
            });
        return row;
        };

    // One folder for everything: the typed folders grey out and every save —
    // downloads, recordings, clips — lands in the single Media Folder. The
    // Library's type buttons then act as pure file-type filters over it.
    unifyCheck = new QCheckBox("Use one folder for all media");
    unifyCheck->setToolTip("Save every download and recording into a single folder. "
        "The Library's type buttons filter it by file type.");
    unifiedFolderRow = makeFolderRow(unifiedFolderEdit, "Select Media Folder",
        "The single folder all media is saved to.");

    dlFolderRow = makeFolderRow(dlFolderEdit, "Select Downloads Folder",
        "Where downloaded files are saved. Independent of the media folders.");
    auto* homeRow = makeFolderRow(homeFolderEdit, "Select Home Folder",
        "Where the File Explorer opens on startup.");
    auto* videoFolderRow = makeFolderRow(videoFolderEdit, "Select Videos Folder",
        "Where video downloads are saved.");
    auto* audioFolderRow = makeFolderRow(audioFolderEdit, "Select Audio Folder",
        "Where audio downloads and extractions are saved.");
    auto* photoFolderRow = makeFolderRow(photoFolderEdit, "Select Photos Folder",
        "Where saved images are stored.");
    auto* recFolderRow = makeFolderRow(recFolderEdit, "Select Recordings Folder",
        "Where the Record button saves recordings and clips.");
    auto* playlistFolderRow = makeFolderRow(playlistFolderEdit, "Select Playlists Folder",
        "Where queue playlists (.sgpl) are saved.");
    typedFolderRows = { videoFolderRow, audioFolderRow, photoFolderRow, recFolderRow, playlistFolderRow };

    // Smart sort: on (default) routes each download into its media-type folder; off
    // reveals the single Downloads Folder row below it.
    smartSortCheck = new QCheckBox("Smart sort downloading");
    smartSortCheck->setToolTip("On: each download is saved into its media type's folder "
        "(videos to Videos, audio to Audio). Off: everything is saved to the single "
        "Downloads Folder you choose below.");

    dlLayout->addRow("Download Type:", typeRow);
    dlLayout->addRow("Download Format:", formatCombo);
    dlLayout->addRow("Download Quality:", dlQualityCombo);
    dlLayout->addRow("Stream Quality:", streamQualityCombo);
    dlLayout->addRow("Recording Type:", recTypeRow);
    dlLayout->addRow("Recording Format:", recFormatCombo);

    stackedWidget->addWidget(dlWidget);

    // === Folders Tab ===
    // Every save location in one place: the unify toggle swaps the typed rows for the
    // single Media Folder row, and Smart Sort (a folder-routing rule) reveals the single
    // Downloads Folder row when off.
    auto* dirsWidget = new QWidget();
    auto* dirsLayout = new QFormLayout(dirsWidget);
    dirsLayout->setContentsMargins(20, 20, 20, 20);
    foldersForm = dirsLayout; // applyUnifyState / applySmartSortState show/hide rows on it

    dirsLayout->addRow("Unify Folders:", unifyCheck);
    dirsLayout->addRow("Media Folder:", unifiedFolderRow);
    dirsLayout->addRow("Videos Folder:", videoFolderRow);
    dirsLayout->addRow("Audio Folder:", audioFolderRow);
    dirsLayout->addRow("Photos Folder:", photoFolderRow);
    dirsLayout->addRow("Recordings Folder:", recFolderRow);
    dirsLayout->addRow("Playlists Folder:", playlistFolderRow);
    dirsLayout->addRow("Home Directory:", homeRow);
    dirsLayout->addRow("Smart Sort:", smartSortCheck);
    dirsLayout->addRow("Downloads Folder:", dlFolderRow);

    stackedWidget->addWidget(dirsWidget);

    // === Search & Home Tab ===
    auto* searchWidget = new QWidget();
    auto* searchLayout = new QFormLayout(searchWidget);
    searchLayout->setContentsMargins(20, 20, 20, 20);
    searchForm = searchLayout;

    // Favourites changed elsewhere (a star toggled in Search) -> keep the home-page pickers
    // current (the home section is appended to this Search & Home page below).
    connect(SgFavorites::instance(),   &SgFavorites::changed, this,
            [this](const QString&, bool) { rebuildHomePickers(); });
    connect(SgFavorites::phInstance(), &SgFavorites::changed, this,
            [this](const QString&, bool) { rebuildHomePickers(); });
    connect(SgFavorites::cbInstance(), &SgFavorites::changed, this,
            [this](const QString&, bool) { rebuildHomePickers(); });
    connect(SgFavorites::scInstance(), &SgFavorites::changed, this,
            [this](const QString&, bool) { rebuildHomePickers(); });
    connect(SgFavorites::twInstance(), &SgFavorites::changed, this,
            [this](const QString&, bool) { rebuildHomePickers(); });

    searchResultsSpin = new QSpinBox();
    searchResultsSpin->setRange(5, 100);
    searchResultsSpin->setSingleStep(5);
    searchResultsSpin->setValue(20);
    searchLayout->addRow("Results per batch:", searchResultsSpin);
    searchResultsSpin->setToolTip("How many results load at a time; more reveal as you scroll.");

    clearHistoryBtn = new QPushButton("Clear History Now");
    clearHistoryBtn->setMaximumWidth(180);
    clearHistoryBtn->setToolTip("Erase the saved search history immediately.");
    searchLayout->addRow("Search History:", clearHistoryBtn);

    clearHistoryOnCloseCheck = new QCheckBox("Clear search history when Seagull closes");
    clearHistoryOnCloseCheck->setToolTip("Wipes the saved history file automatically on every exit.");
    searchLayout->addRow("", clearHistoryOnCloseCheck);

    // Per-site home-page favourites pickers + result/video limits, appended below the
    // search settings (buildHomeSection emits its own "Home page" header; the site
    // sections start collapsed).
    buildHomeSection(searchLayout);

    stackedWidget->addWidget(searchWidget);

    // === Info Tab: bundled docs in a tabbed reader ===
    auto* infoWidget = new QWidget();
    auto* infoLayout = new QVBoxLayout(infoWidget);
    infoLayout->setContentsMargins(20, 20, 20, 20);

    // App version up top so bug reports can cite the exact build.
    auto* versionLabel = new QLabel(QStringLiteral("Seagull %1").arg(QString::fromLatin1(SEAGULL_VERSION)));
    versionLabel->setObjectName("infoVersionLabel");
    QFont versionFont = versionLabel->font();
    versionFont.setBold(true);
    versionFont.setPointSize(versionFont.pointSize() + 2);
    versionLabel->setFont(versionFont);
    infoLayout->addWidget(versionLabel);

    auto* docTabs = new QTabWidget();

    // Read Me — drop the Screenshots section (its images live on disk/GitHub, not
    // in resources, so they'd render as broken icons in-app).
    QString readme = readDoc(":/docs/README.md");
    readme.remove(QRegularExpression("\\n## Screenshots[\\s\\S]*?(?=\\n## )"));
    auto* readmeView = new QTextBrowser();
    readmeView->setOpenExternalLinks(true);
    readmeView->setMarkdown(readme);
    docTabs->addTab(readmeView, "Read Me");

    auto* faqView = new QTextBrowser();
    faqView->setOpenExternalLinks(true);
    faqView->setMarkdown(readDoc(":/docs/FAQ.md"));
    docTabs->addTab(faqView, "FAQ");

    auto* disclaimerView = new QTextBrowser();
    disclaimerView->setOpenExternalLinks(true);
    disclaimerView->setMarkdown(readDoc(":/docs/DISCLAIMER.md"));
    docTabs->addTab(disclaimerView, "Disclaimer");

    auto* licenseView = new QTextBrowser();
    licenseView->setOpenExternalLinks(true);
    licenseView->setPlainText(readDoc(":/docs/LICENSE.txt")); // plain text, not markdown
    docTabs->addTab(licenseView, "License");

    auto* noticesView = new QTextBrowser();
    noticesView->setOpenExternalLinks(true);
    noticesView->setMarkdown(readDoc(":/docs/THIRD_PARTY_NOTICES.md"));
    docTabs->addTab(noticesView, "Notices");

    infoLayout->addWidget(docTabs);
    stackedWidget->addWidget(infoWidget);

    // --- Assemble and Connect ---
    mainLayout->addWidget(sidebar);
    mainLayout->addWidget(stackedWidget);

    // Change pages when a side tab is clicked
    connect(sidebar, &QListWidget::currentRowChanged, stackedWidget, &QStackedWidget::setCurrentIndex);
    // When the Audio (EQ) page becomes visible — via the player's EQ button or a direct
    // sidebar click — tell the EQ to re-arm its auto-follow of the playing media kind.
    connect(sidebar, &QListWidget::currentRowChanged, this, [this](int row) {
        if (row == m_audioRow) emit audioPageShown();
    });

    // --- Bottom button bar: Reset to Default only (settings auto-apply) ---
    auto* buttonBar = new QHBoxLayout();
    buttonBar->setContentsMargins(0, 10, 10, 10);
    resetBtn = new QPushButton("Reset to Default");
    buttonBar->addStretch();
    buttonBar->addWidget(resetBtn);
    outerLayout->addLayout(buttonBar);

    connect(resetBtn, &QPushButton::clicked, this, &Settings::resetDefaults);

    // Auto-apply: every control change writes config and applies immediately.
    connect(appearanceGroup, &QButtonGroup::buttonClicked, this, [this](QAbstractButton*) {
        onAppearanceChanged(); // refilter the theme list for the chosen appearance, then save
        });
    connect(themeCombo, &QComboBox::currentTextChanged, this, &Settings::saveSettings);
    connect(cardSizeCombo, &QComboBox::currentTextChanged, this, &Settings::onCardSizeChanged);
    connect(cardSizeSlider, &QSlider::valueChanged, this, &Settings::saveSettings);
    connect(seekBarSizeCombo, &QComboBox::currentTextChanged, this, &Settings::saveSettings);
    connect(visualizerCombo, &QComboBox::currentTextChanged, this, &Settings::saveSettings);
    connect(lighthouseCombo, &QComboBox::currentIndexChanged, this, &Settings::saveSettings);
    connect(behaviorCombo, &QComboBox::currentTextChanged, this, &Settings::saveSettings);
    connect(maxGullsSpin, &QSpinBox::valueChanged, this, &Settings::saveSettings);
    connect(killGullsCheck, &QCheckBox::toggled, this, &Settings::saveSettings);
    connect(typeGroup, &QButtonGroup::buttonClicked, this, [this](QAbstractButton*) {
        onDownloadTypeChanged(); // refresh format + quality lists, then save
        });
    connect(formatCombo, &QComboBox::currentTextChanged, this, &Settings::saveSettings);
    connect(dlQualityCombo, &QComboBox::currentTextChanged, this, &Settings::saveSettings);
    connect(searchResultsSpin, &QSpinBox::valueChanged, this, &Settings::saveSettings);
    connect(streamQualityCombo, &QComboBox::currentTextChanged, this, &Settings::saveSettings);
    connect(cookiesBrowserCombo, &QComboBox::currentTextChanged, this, &Settings::onCookiesBrowserChanged);
    connect(deleteCookiesBtn, &QPushButton::clicked, this, &Settings::deleteCookieData);
    connect(recTypeGroup, &QButtonGroup::buttonClicked, this, [this](QAbstractButton*) {
        onRecordingTypeChanged(); // refresh the recording format list, then save
        });
    connect(recFormatCombo, &QComboBox::currentTextChanged, this, &Settings::saveSettings);
    for (auto* edit : { homeFolderEdit, dlFolderEdit, videoFolderEdit, audioFolderEdit,
                        photoFolderEdit, recFolderEdit, unifiedFolderEdit })
        connect(edit, &QLineEdit::textChanged, this, &Settings::saveSettings);
    connect(unifyCheck, &QCheckBox::toggled, this, [this](bool) {
        applyUnifyState();
        saveSettings();
        });
    connect(smartSortCheck, &QCheckBox::toggled, this, [this](bool) {
        applySmartSortState(); // show/hide the Downloads Folder row
        saveSettings();
        });
    connect(autoUpdateCheck, &QCheckBox::toggled, this, &Settings::saveSettings);
    connect(clearHistoryOnCloseCheck, &QCheckBox::toggled, this, &Settings::saveSettings);
    connect(clearHistoryBtn, &QPushButton::clicked, this, [this]() { emit clearHistoryRequested(); });

    // Set default tab
    sidebar->setCurrentRow(0);
}

QString Settings::currentDownloadType() const {
    return typeAudioBtn->isChecked() ? "Audio" : "Video";
}

void Settings::updateDownloadFormatOptions() {
    const QString prev = formatCombo->currentText();
    formatCombo->blockSignals(true);
    formatCombo->clear();
    // No "Best Available" here: a concrete container, mp4/m4a-first, so the saved
    // file is always something predictable. (Quality keeps its Best option.)
    if (currentDownloadType() == "Audio")
        formatCombo->addItems({ "m4a", "mp3", "flac", "wav", "opus", "aac", "vorbis" });
    else
        formatCombo->addItems({ "mp4", "mkv", "webm", "avi", "flv", "mov" });
    int idx = formatCombo->findText(prev);
    formatCombo->setCurrentIndex(idx >= 0 ? idx : 0); // stale values (e.g. old "Best Available") fall back to index 0
    formatCombo->blockSignals(false);
}

void Settings::updateDownloadQualityOptions() {
    const bool audio = (currentDownloadType() == "Audio");
    const QString prev = dlQualityCombo->currentText();

    dlQualityCombo->blockSignals(true);
    dlQualityCombo->clear();
    if (audio) {
        dlQualityCombo->addItems({
            "Best Available", "320 kbps", "256 kbps", "192 kbps", "128 kbps", "96 kbps"
            });
    }
    else {
        dlQualityCombo->addItems({
            "Best Available", "4320p (8K)", "2160p (4K)", "1440p (2K)", "1080p", "720p", "480p", "360p"
            });
    }
    // Keep the previous choice if it's still valid, otherwise fall back to Best.
    int idx = dlQualityCombo->findText(prev);
    dlQualityCombo->setCurrentIndex(idx >= 0 ? idx : 0);
    dlQualityCombo->blockSignals(false);
}

void Settings::onDownloadTypeChanged() {
    updateDownloadFormatOptions();
    updateDownloadQualityOptions();
    saveSettings();
}

QString Settings::currentRecordingType() const {
    return recTypeAudioBtn->isChecked() ? "Audio" : "Video";
}

void Settings::updateRecordingFormatOptions() {
    const QString prev = recFormatCombo->currentText();
    recFormatCombo->blockSignals(true);
    recFormatCombo->clear();
    if (currentRecordingType() == "Audio")
        recFormatCombo->addItems({ "M4A", "MP3", "Opus", "FLAC", "WAV" });
    else
        recFormatCombo->addItems({ "MP4", "MKV", "TS" }); // MP4 first = the default
    int idx = recFormatCombo->findText(prev);
    recFormatCombo->setCurrentIndex(idx >= 0 ? idx : 0);
    recFormatCombo->blockSignals(false);
}

void Settings::onRecordingTypeChanged() {
    updateRecordingFormatOptions();
    saveSettings();
}

int Settings::currentCardWidth() const {
    if (cardSizeCombo->currentText() == "Custom") return cardSizeSlider->value();
    return presetWidth(cardSizeCombo->currentText());
}

void Settings::onCardSizeChanged() {
    const bool custom = (cardSizeCombo->currentText() == "Custom");
    cardSizeSlider->setVisible(custom);
    if (!custom) {
        // Snap the (hidden) slider to the chosen preset so switching to Custom later
        // starts from the right place.
        cardSizeSlider->blockSignals(true);
        cardSizeSlider->setValue(presetWidth(cardSizeCombo->currentText()));
        cardSizeSlider->blockSignals(false);
    }
    saveSettings();
}

void Settings::populateThemeCombo(bool dark, const QString& select) {
    // Fill the Theme menu with just the themes of the chosen appearance, optionally
    // selecting one. Signals are blocked so refilling never auto-saves mid-build.
    themeCombo->blockSignals(true);
    themeCombo->clear();
    themeCombo->addItems(Theme::names(dark));
    if (!select.isEmpty()) {
        const int idx = themeCombo->findText(select);
        if (idx >= 0) themeCombo->setCurrentIndex(idx);
    }
    themeCombo->blockSignals(false);
}

void Settings::onAppearanceChanged() {
    const bool dark = darkModeBtn->isChecked();
    // Keep the current theme if it already matches the new appearance, otherwise the
    // first of that appearance becomes selected.
    const QString cur = themeCombo->currentText();
    const QString keep = (Theme::isKnown(cur) && Theme::isDark(cur) == dark) ? cur : QString();
    populateThemeCombo(dark, keep);
    saveSettings(); // themeCombo now holds the right theme -> apply + persist
}

void Settings::browseInto(QLineEdit* edit, const QString& title) {
    QString dir = QFileDialog::getExistingDirectory(this, title, edit->text());
    if (!dir.isEmpty())
        edit->setText(dir); // textChanged -> saveSettings
}

void Settings::applyUnifyState() {
    // Swap which folder rows exist: unified mode shows just the one Media Folder
    // row; per-type mode shows the four typed rows. (setRowVisible hides the
    // row's label along with the field.)
    const bool unified = unifyCheck->isChecked();
    foldersForm->setRowVisible(unifiedFolderRow, unified);
    for (QWidget* row : typedFolderRows)
        foldersForm->setRowVisible(row, !unified);
}

void Settings::applySmartSortState() {
    // The single Downloads Folder only matters when smart sort is OFF — otherwise each
    // download is routed by its media type, so hide the row to avoid implying otherwise.
    // (The Downloads Folder row lives on the Folders page alongside the other paths.)
    if (foldersForm) foldersForm->setRowVisible(dlFolderRow, !smartSortCheck->isChecked());
}

void Settings::buildHomeSection(QFormLayout* form) {
    // Header + one-line intro (full-width rows, no label column).
    auto* header = new QLabel("Home page");
    QFont hf = header->font(); hf.setBold(true); hf.setPointSize(hf.pointSize() + 1);
    header->setFont(hf);
    header->setContentsMargins(0, 2, 0, 0);
    form->addRow(header);

    auto* intro = new QLabel("Open a site below to set how many favourites appear on its home page, then "
        "drag them into the order you want. The ones at the top show first.");
    intro->setObjectName("metaStats"); // themed dim text
    intro->setWordWrap(true);
    form->addRow(intro);

    // hasVideos: YouTube/PornHub/SoundCloud pull N recent videos (tracks) per channel;
    // the live sites' favourites are live rooms/channels, so no per-channel spin there.
    // hasAmount: most sites cap the home page at a max count; Chaturbate shows ALL
    // favourited models, so it gets no max-amount spin.
    // hasContinue: live streams never enter watch history, so Twitch gets no Continue
    // Watching toggle (there would never be anything to show).
    // amountLabel/videosLabel: per-site wording (videos vs tracks vs channels).
    struct SiteDef {
        SgFavorites* store;
        const char*  label;
        const char*  suffix;
        bool isYouTube;
        bool hasVideos;
        bool hasAmount;
        bool hasContinue;
        int  defaultAmount;
        const char* amountLabel;
        const char* videosLabel;
    };
    // Section order mirrors the Search tab's site dropdown: YouTube first, then the
    // other general sites, adult sites last. Config keys are per-suffix, so reordering
    // rows never touches stored settings.
    const SiteDef sites[] = {
        { SgFavorites::instance(),   "YouTube",    "YouTube",    true,  true,  true,  true,  15, "Max homepage videos:",   "Videos per channel:" },
        { SgFavorites::scInstance(), "SoundCloud", "SoundCloud", false, true,  true,  true,  15, "Max homepage tracks:",   "Tracks per artist:"  },
        { SgFavorites::twInstance(), "Twitch",     "Twitch",     false, false, true,  false, 20, "Max homepage channels:", ""                    },
        { SgFavorites::phInstance(), "PornHub",    "PornHub",    false, true,  true,  true,  20, "Max homepage videos:",   "Videos per channel:" },
        { SgFavorites::cbInstance(), "Chaturbate", "Chaturbate", false, false, false, false, 5,  "",                       ""                    },
    };
    for (const SiteDef& s : sites) {
        const QString label  = QString::fromLatin1(s.label);
        const QString suffix = QString::fromLatin1(s.suffix);
        auto* toggle = new QPushButton(QStringLiteral("▸  ") + label); // ▸ collapsed
        toggle->setObjectName("homeSiteToggle");
        toggle->setCheckable(true);
        toggle->setCursor(Qt::PointingHandCursor);
        toggle->setStyleSheet("text-align:left; padding:5px 6px;");
        form->addRow(toggle);

        auto* list = new QListWidget();
        list->setFixedHeight(140);
        list->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        // Drag to reorder: the row order is the home-page priority (top = first).
        list->setSelectionMode(QAbstractItemView::SingleSelection);
        list->setDragDropMode(QAbstractItemView::InternalMove);
        list->setDefaultDropAction(Qt::MoveAction);
        list->setDragDropOverwriteMode(false);
        list->hide(); // collapsed until the site label is clicked
        form->addRow(list);

        // Per-site result limit, folded into this site's section. Chaturbate has no limit
        // (it shows all favourited models), so it gets a short note instead of a spin.
        QSpinBox* amountSpin = nullptr;
        QLabel*   amountLabel = nullptr;
        if (s.hasAmount) {
            amountSpin = new QSpinBox();
            amountSpin->setRange(1, 20);
            amountSpin->setValue(s.defaultAmount);
            amountSpin->setToolTip(s.hasVideos
                ? "The most items shown on this site's home page (from the top of your list)."
                : "The most channels shown on this site's home page (from the top of your list).");
            amountSpin->hide();
            amountLabel = new QLabel(QString::fromLatin1(s.amountLabel));
            amountLabel->hide();
            form->addRow(amountLabel, amountSpin);
        } else {
            amountLabel = new QLabel("All favourited models show on the home page.");
            amountLabel->setObjectName("metaStats"); // themed dim text
            amountLabel->setWordWrap(true);
            amountLabel->hide();
            form->addRow(amountLabel);
        }

        // YouTube only: a throttling/bot-detection warning when the count goes past 10
        // (each channel is another rapid request). Shown inline, no modal nag.
        QLabel* warning = nullptr;
        if (s.isYouTube) {
            warning = new QLabel("Pulling more than 10 channels makes many rapid requests and "
                                 "can trigger throttling or bot checks.");
            warning->setObjectName("metaStats"); // themed dim text
            warning->setWordWrap(true);
            warning->hide();
            form->addRow(warning);
            connect(amountSpin, &QSpinBox::valueChanged, this, [warning](int v) {
                warning->setVisible(v > 10);
            });
        }

        // Per-site videos-per-channel (video-listing sites only — the live sites have
        // no per-channel listing to pull from).
        QSpinBox* videosSpin = nullptr;
        QLabel*   videosLabel = nullptr;
        if (s.hasVideos) {
            videosSpin = new QSpinBox();
            videosSpin->setRange(1, 20);
            videosSpin->setValue(5);
            videosSpin->setToolTip("How many recent items to pull from each of this site's home-page favourites.");
            videosSpin->hide();
            videosLabel = new QLabel(QString::fromLatin1(s.videosLabel));
            videosLabel->hide();
            form->addRow(videosLabel, videosSpin);
            connect(videosSpin, &QSpinBox::valueChanged, this, &Settings::saveSettings);
        }

        // Randomize-order toggle (only the video feeds mix by recency — Chaturbate's live
        // rooms always show in your list order, so it gets no toggle). Checked = mix by
        // recency (current behaviour); unchecked = show grouped in your favourites order.
        QPushButton* shuffleBtn = nullptr;
        if (s.hasVideos) {
            shuffleBtn = new QPushButton();
            shuffleBtn->setObjectName("homeShuffleToggle");
            shuffleBtn->setCheckable(true);
            shuffleBtn->setCursor(Qt::PointingHandCursor);
            shuffleBtn->setToolTip("On: mix recent videos from your favourites together. "
                                   "Off: show them grouped in the order set above.");
            shuffleBtn->hide();
            form->addRow(shuffleBtn);
            connect(shuffleBtn, &QPushButton::toggled, this, [shuffleBtn](bool on) {
                shuffleBtn->setText(on ? QStringLiteral("Randomize order: On")
                                       : QStringLiteral("Randomize order: Off"));
            });
            connect(shuffleBtn, &QPushButton::toggled, this, &Settings::saveSettings);
        }

        // Per-site "Continue Watching" row toggle. Off hides the row on that site's
        // home; playback position is still remembered silently (governed by the global
        // Playback toggle). Twitch skips it: live streams never enter watch history.
        QCheckBox* continueCheck = nullptr;
        if (s.hasContinue) {
            continueCheck = new QCheckBox("Show Continue Watching");
            continueCheck->setToolTip("Offer a Continue Watching option on this site's home page, "
                                      "listing videos you've partly watched.");
            continueCheck->hide();
            form->addRow(continueCheck);
            connect(continueCheck, &QCheckBox::toggled, this, &Settings::saveSettings);
        }

        // Lazy-load toggle (video-listing sites only — Chaturbate's live rooms are a fixed
        // list). Off by default; enabling it makes the home feed keep pulling more from your
        // favourites as you scroll, which is extra requests, so we warn on the way on.
        QCheckBox* lazyCheck = nullptr;
        if (s.hasVideos) {
            lazyCheck = new QCheckBox("Load more as I scroll");
            lazyCheck->setToolTip("Keep loading more videos from your favourites as you reach the "
                                  "bottom of this site's home page. Off by default: it makes repeated "
                                  "requests, which can trigger rate-limiting or bot checks.");
            lazyCheck->hide();
            form->addRow(lazyCheck);
            connect(lazyCheck, &QCheckBox::toggled, this, [this, lazyCheck](bool on) {
                if (m_loading) return; // programmatic load: no prompt, no save
                if (on) {
                    QMessageBox box(QMessageBox::Warning, "Load more as you scroll",
                        "This keeps requesting more videos from your favourites every time you "
                        "reach the bottom of the home page. On sites that watch for automated "
                        "traffic, those repeated requests can lead to rate-limiting or "
                        "\"confirm you're not a bot\" checks. Turn it on only if you want a "
                        "longer home feed and accept that risk.",
                        QMessageBox::Ok | QMessageBox::Cancel, this);
                    box.button(QMessageBox::Ok)->setText("Enable anyway");
                    if (box.exec() != QMessageBox::Ok) {
                        lazyCheck->blockSignals(true);
                        lazyCheck->setChecked(false);
                        lazyCheck->blockSignals(false);
                        return; // declined: leave it off, nothing to save
                    }
                }
                saveSettings();
            });
        }

        m_homePickers.append({ s.store, "Search/HomeChannels" + suffix, "Search/HomeAmount" + suffix,
                               s.hasVideos ? ("Search/HomeVideosPerChannel" + suffix) : QString(),
                               list, amountSpin, videosSpin, warning,
                               s.hasVideos ? ("Search/HomeRandomize" + suffix) : QString(), shuffleBtn,
                               s.defaultAmount,
                               s.hasContinue ? ("Search/ShowContinueWatching" + suffix) : QString(), continueCheck,
                               s.hasVideos ? ("Search/HomeLazyLoad" + suffix) : QString(), lazyCheck });

        if (amountSpin) connect(amountSpin, &QSpinBox::valueChanged, this, &Settings::saveSettings);
        connect(toggle, &QPushButton::toggled, this,
                [toggle, list, amountSpin, amountLabel, videosSpin, videosLabel, warning, shuffleBtn, continueCheck, lazyCheck, label](bool on) {
            list->setVisible(on);
            if (amountSpin)  amountSpin->setVisible(on);
            if (amountLabel) amountLabel->setVisible(on);
            if (videosSpin)  videosSpin->setVisible(on);
            if (videosLabel) videosLabel->setVisible(on);
            if (warning) warning->setVisible(on && amountSpin && amountSpin->value() > 10);
            if (shuffleBtn) shuffleBtn->setVisible(on);
            if (continueCheck) continueCheck->setVisible(on);
            if (lazyCheck) lazyCheck->setVisible(on);
            toggle->setText((on ? QStringLiteral("▾  ") : QStringLiteral("▸  ")) + label); // ▾/▸
        });
        // Persist the new priority whenever a drag reorders the rows. QListWidget's
        // internal move is a remove+insert (rowsInserted); cover rowsMoved too in case
        // the model does a true move. Guarded so the programmatic rebuild doesn't save.
        auto save = [this, list]() { if (!m_rebuildingPickers) saveHomePickerFor(list); };
        connect(list->model(), &QAbstractItemModel::rowsInserted, this, save);
        connect(list->model(), &QAbstractItemModel::rowsMoved,    this, save);
    }
}

void Settings::rebuildHomePickers() {
    m_rebuildingPickers = true; // suppress the rowsInserted save while we repopulate
    for (const HomePicker& p : m_homePickers) {
        const QStringList saved = iniSettings->value(p.cfgKey).toStringList(); // priority order
        const auto favs = p.store->favorites();

        // Display order = saved order first (still-favourited), then any favourites not
        // yet placed appended at the bottom (lowest priority — e.g. a freshly starred one).
        QStringList order;
        for (const QString& u : saved) {
            bool stillFav = false;
            for (const auto& f : favs) if (f.url == u) { stillFav = true; break; }
            if (stillFav && !order.contains(u)) order << u;
        }
        for (const auto& f : favs)
            if (!order.contains(f.url)) order << f.url;

        p.list->clear();
        for (const QString& u : order) {
            QString name = u;
            for (const auto& f : favs) if (f.url == u) { name = f.name.isEmpty() ? f.url : f.name; break; }
            auto* item = new QListWidgetItem(name, p.list);
            item->setData(kUrlRole, u);
        }

        // Keep the stored order complete (newly starred favourites get appended) so the
        // home feed includes them without the user having to open this page and drag.
        if (order != saved) {
            iniSettings->setValue(p.cfgKey, order);
            iniSettings->sync();
        }
    }
    m_rebuildingPickers = false;
}

void Settings::saveHomePickerFor(QListWidget* list) {
    for (const HomePicker& p : m_homePickers) {
        if (p.list != list) continue;
        QStringList order; // row order top->bottom IS the priority
        for (int i = 0; i < list->count(); ++i)
            order << list->item(i)->data(kUrlRole).toString();
        iniSettings->setValue(p.cfgKey, order);
        iniSettings->sync();
        return;
    }
}

void Settings::onDefenderExclusionClicked() {
    // Toggle: remove the exclusion if it's set, otherwise add it. Both raise UAC and
    // block briefly while the elevated step runs. The result reflects what actually
    // persisted, so on Blocked/Error we explain why (Tamper Protection usually) and
    // offer to open Windows Security; we still re-query afterwards to settle the label.
    const bool wasExcluded = defenderExcluded;
    defenderExclusionBtn->setEnabled(false);
    const SgMediaControls::DefenderResult result =
        wasExcluded ? SgMediaControls::removeDefenderExclusion()
                    : SgMediaControls::addDefenderExclusion();

    // The elevated step verified the real state, so on Success persist it ourselves:
    // a non-elevated read can't see the exclusion list (it returns an admin-only
    // placeholder), so this stored flag is what keeps the label correct afterwards.
    if (result == SgMediaControls::DefenderResult::Success) {
        defenderExcluded = !wasExcluded;
        iniSettings->setValue("Setup/DefenderExcluded", defenderExcluded);
        iniSettings->sync();
    }

    const QString message = SgMediaControls::defenderResultMessage(result);
    if (!message.isEmpty()) {
        QMessageBox box(this);
        box.setIcon(QMessageBox::Warning);
        box.setWindowTitle("Defender Exclusion");
        box.setText(message);
        QPushButton* openBtn = box.addButton("Open Windows Security", QMessageBox::ActionRole);
        box.addButton(QMessageBox::Close);
        box.exec();
        if (box.clickedButton() == openBtn) SgMediaControls::openDefenderSettings();
    }

    refreshDefenderButton();
}

void Settings::refreshDefenderButton() {
    // Resolve the button label off the GUI thread. The query prints YES / NO / UNKNOWN:
    // UNKNOWN means a non-elevated Get-MpPreference couldn't read the exclusion list
    // (the usual case — it needs admin), so we fall back to the flag we persisted when
    // the verified elevated add/remove last ran, instead of wrongly showing "Add".
    defenderExclusionBtn->setEnabled(false);
    defenderExclusionBtn->setText("Checking...");

    auto* ps = new QProcess(this);
    connect(ps, &QProcess::finished, this,
        [this, ps](int, QProcess::ExitStatus) {
            const QString out = ps->readAllStandardOutput().trimmed();
            if (out == "YES")      defenderExcluded = true;
            else if (out == "NO")  defenderExcluded = false;
            else /* UNKNOWN */     defenderExcluded =
                iniSettings->value("Setup/DefenderExcluded", false).toBool();
            defenderExclusionBtn->setText(defenderExcluded ? "Remove Exclusion" : "Add Exclusion");
            defenderExclusionBtn->setEnabled(true);
            ps->deleteLater();
        });
    connect(ps, &QProcess::errorOccurred, this, [this, ps](QProcess::ProcessError) {
        // Couldn't run the query at all — fall back to the persisted flag.
        defenderExcluded = iniSettings->value("Setup/DefenderExcluded", false).toBool();
        defenderExclusionBtn->setText(defenderExcluded ? "Remove Exclusion" : "Add Exclusion");
        defenderExclusionBtn->setEnabled(true);
        ps->deleteLater();
    });
    ps->start("powershell.exe", { "-NoProfile", "-WindowStyle", "Hidden", "-Command",
                                  SgMediaControls::defenderExclusionQueryCommand() });
}

void Settings::onDesktopShortcutClicked() {
    if (SgMediaControls::desktopShortcutExists()) SgMediaControls::removeDesktopShortcut();
    else                                          SgMediaControls::createDesktopShortcut();
    refreshShortcutButtons();
}

void Settings::onStartMenuShortcutClicked() {
    if (SgMediaControls::startMenuShortcutExists()) SgMediaControls::removeStartMenuShortcut();
    else                                            SgMediaControls::createStartMenuShortcut();
    refreshShortcutButtons();
}

void Settings::refreshShortcutButtons() {
    // Cheap file check (no COM/elevation), so it's fine to run on every show.
    desktopShortcutBtn->setText(
        SgMediaControls::desktopShortcutExists() ? "Remove Desktop Shortcut" : "Add Desktop Shortcut");
    startMenuShortcutBtn->setText(
        SgMediaControls::startMenuShortcutExists() ? "Remove Start Menu Shortcut" : "Add Start Menu Shortcut");
}

void Settings::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    rebuildHomePickers();    // favourites may have changed since the page was built
    refreshDefenderButton(); // re-check in case the exclusion changed outside the app
    refreshShortcutButtons(); // re-check in case a shortcut was added/deleted outside the app
}

void Settings::loadSettings() {
    // Populate controls without each change auto-saving back to disk.
    m_loading = true;

    autoUpdateCheck->setChecked(iniSettings->value("General/AutoUpdate", true).toBool());
    rememberPositionCheck->setChecked(iniSettings->value("Playback/RememberPosition", true).toBool());
    clearHistoryOnCloseCheck->setChecked(iniSettings->value("Search/ClearHistoryOnExit", false).toBool());

    // Theme: coerce an unknown name (old/legacy config) back to Seagull, derive the
    // Light/Dark appearance from it, set the toggle, and fill the menu to match.
    QString savedTheme = iniSettings->value("Display/Theme", "Seagull").toString();
    if (!Theme::isKnown(savedTheme)) savedTheme = "Seagull";
    const bool darkTheme = Theme::isDark(savedTheme);
    (darkTheme ? darkModeBtn : lightModeBtn)->setChecked(true);
    populateThemeCombo(darkTheme, savedTheme);

    // Card size: stored as a pixel width. Match it to a named preset, else Custom.
    int cardPx = qBound(kCardMinPx, iniSettings->value("Display/CardWidth", 300).toInt(), kCardMaxPx); // default Large
    cardSizeSlider->blockSignals(true);
    cardSizeSlider->setValue(cardPx);
    cardSizeSlider->blockSignals(false);
    const QString cardPreset = presetName(cardPx);
    cardSizeCombo->setCurrentText(cardPreset.isEmpty() ? "Custom" : cardPreset);
    cardSizeSlider->setVisible(cardPreset.isEmpty());

    seekBarSizeCombo->setCurrentText(iniSettings->value("Display/SeekBarSize", "Small").toString());

    // A legacy saved "Seagull Sky" won't match any item, so the combo stays on
    // index 0 — Seagull Morning, its renamed successor.
    visualizerCombo->setCurrentText(iniSettings->value("Visualizer/Type", "Seagull Morning").toString());
    lighthouseCombo->setCurrentIndex(qMax(0,
        lighthouseCombo->findData(iniSettings->value("Visualizer/LighthouseBeats", 1).toInt())));
    // Behaviour is global — one key shared by every visualizer.
    behaviorCombo->setCurrentText(iniSettings->value("Visualizer/Behavior", "Drift").toString());
    maxGullsSpin->setValue(iniSettings->value("Visualizer/MaxGulls", 14).toInt());
    killGullsCheck->setChecked(iniSettings->value("Visualizer/KillOnEnd", true).toBool());

    // Type -> Format -> Quality cascade: set the type, build its format list, pick
    // the saved format, build the matching quality list, pick the saved quality.
    QString savedFormat = iniSettings->value("Download/Format", "mp4").toString();
    QString type = iniSettings->value("Download/Type").toString();
    if (type.isEmpty()) {
        // Migrate older configs that only stored a format: infer the type from it.
        static const QStringList audioFmts = { "mp3", "m4a", "flac", "wav", "opus", "aac", "vorbis" };
        type = audioFmts.contains(savedFormat) ? "Audio" : "Video";
    }
    (type == "Audio" ? typeAudioBtn : typeVideoBtn)->setChecked(true);
    updateDownloadFormatOptions();
    formatCombo->setCurrentText(savedFormat);
    updateDownloadQualityOptions();
    dlQualityCombo->setCurrentText(iniSettings->value("Download/Quality", "Best Available").toString());

    streamQualityCombo->setCurrentText(iniSettings->value("Streaming/Quality", "Best Available").toString());
    // Firefox is the only supported browser now. Coerce a stale Chrome/Edge/Brave choice
    // (from before the change) back to None so the combo and the saved value agree — else
    // cookieArgs() would keep passing the broken Chromium keyword read straight from the INI.
    const QString savedCookies = iniSettings->value("Streaming/CookiesBrowser", "None").toString();
    if (savedCookies != "None" && savedCookies != "Firefox")
        iniSettings->setValue("Streaming/CookiesBrowser", "None");
    cookiesBrowserCombo->setCurrentText(
        (savedCookies == "Firefox") ? QStringLiteral("Firefox") : QStringLiteral("None"));
    m_prevCookiesChoice = cookiesBrowserCombo->currentText(); // baseline for the activation warning

    // Recording: type drives the format list; the old Streaming/RecordFormat key
    // (pre recording settings) seeds the video format for existing configs.
    const QString recType = iniSettings->value("Recording/Type", "Video").toString();
    (recType == "Audio" ? recTypeAudioBtn : recTypeVideoBtn)->setChecked(true);
    updateRecordingFormatOptions();
    const QString legacyRecFmt = iniSettings->value("Streaming/RecordFormat", "MP4").toString().toUpper();
    recFormatCombo->setCurrentText(iniSettings->value("Recording/Format",
        recType == "Audio" ? QStringLiteral("M4A") : legacyRecFmt).toString());

    searchResultsSpin->setValue(iniSettings->value("Search/ResultLimit", 20).toInt());
    // Per-site home result limit + videos per channel. Migrate from the old global keys
    // so existing users keep their chosen values on first run after the upgrade.
    const bool hasLegacyAmount = iniSettings->contains("Search/HomeChannelAmount");
    const int legacyAmount = iniSettings->value("Search/HomeChannelAmount", 5).toInt();
    const int legacyVideos = iniSettings->value("Search/HomeVideosPerChannel", 5).toInt();
    for (const HomePicker& p : m_homePickers) {
        if (p.amountSpin) {
            // Fall back to the old global key only if it was actually set; otherwise the
            // per-site default (e.g. 15 for YouTube, 20 for PornHub).
            const int fallback = hasLegacyAmount ? legacyAmount : p.amountDefault;
            p.amountSpin->setValue(qBound(1, iniSettings->value(p.amountKey, fallback).toInt(), 20));
        }
        if (p.videosSpin)
            p.videosSpin->setValue(qBound(1, iniSettings->value(p.videosKey, legacyVideos).toInt(), 20));
        if (p.shuffleBtn) {
            const bool on = iniSettings->value(p.shuffleKey, true).toBool(); // default: mix by recency
            p.shuffleBtn->setChecked(on);
            p.shuffleBtn->setText(on ? QStringLiteral("Randomize order: On")
                                     : QStringLiteral("Randomize order: Off"));
        }
        if (p.continueCheck)
            p.continueCheck->setChecked(iniSettings->value(p.continueKey, true).toBool()); // default: show
        if (p.lazyCheck)
            p.lazyCheck->setChecked(iniSettings->value(p.lazyKey, false).toBool()); // default: off
        if (p.warning) p.warning->setVisible(false); // collapsed sections hide it; toggle re-evaluates
    }

    // SgPaths owns the key/default/legacy-fallback logic, so the UI always shows
    // exactly the folder the downloader/recorder will actually use. The typed
    // rows read with honourUnify=false: they show their own configured folders
    // even while unify overrides them, so toggling unify off restores them.
    homeFolderEdit->setText(SgPaths::homeFolder());
    dlFolderEdit->setText(SgPaths::downloadFolder());
    videoFolderEdit->setText(SgPaths::videoFolder(false));
    audioFolderEdit->setText(SgPaths::audioFolder(false));
    photoFolderEdit->setText(SgPaths::photoFolder(false));
    recFolderEdit->setText(SgPaths::recordingFolder(false));
    playlistFolderEdit->setText(SgPaths::playlistFolder(false));
    unifiedFolderEdit->setText(SgPaths::unifiedFolder());
    unifyCheck->setChecked(SgPaths::unifyMedia());
    applyUnifyState();

    smartSortCheck->setChecked(SgPaths::smartSortDownloads());
    applySmartSortState();

    // Seed the "already applied" trackers so the first unrelated save doesn't trigger
    // a redundant theme re-apply / grid re-flow.
    m_appliedTheme = themeCombo->currentText();
    m_appliedCardWidth = currentCardWidth();
    m_appliedSeekBarWidth = PlayerControls::widthForSize(seekBarSizeCombo->currentText());

    m_loading = false;
}

void Settings::saveSettings() {
    if (m_loading) return; // ignore the change signals fired while loading

    // Write values to INI groups
    iniSettings->setValue("General/AutoUpdate", autoUpdateCheck->isChecked());
    iniSettings->setValue("Playback/RememberPosition", rememberPositionCheck->isChecked());
    iniSettings->setValue("Search/ClearHistoryOnExit", clearHistoryOnCloseCheck->isChecked());
    iniSettings->setValue("Display/Theme", themeCombo->currentText());
    iniSettings->setValue("Display/CardWidth", currentCardWidth());
    iniSettings->setValue("Display/SeekBarSize", seekBarSizeCombo->currentText());
    iniSettings->setValue("Visualizer/Type", visualizerCombo->currentText());
    iniSettings->setValue("Visualizer/LighthouseBeats", lighthouseCombo->currentData().toInt());
    // Behaviour is global — one key shared by every visualizer.
    iniSettings->setValue("Visualizer/Behavior", behaviorCombo->currentText());
    iniSettings->setValue("Visualizer/MaxGulls", maxGullsSpin->value());
    iniSettings->setValue("Visualizer/KillOnEnd", killGullsCheck->isChecked());
    iniSettings->setValue("Download/Type", currentDownloadType());
    iniSettings->setValue("Download/Format", formatCombo->currentText());
    iniSettings->setValue("Download/Quality", dlQualityCombo->currentText());
    iniSettings->setValue("Streaming/Quality", streamQualityCombo->currentText());
    iniSettings->setValue("Streaming/CookiesBrowser", cookiesBrowserCombo->currentText());
    iniSettings->setValue("Recording/Type", currentRecordingType());
    iniSettings->setValue("Recording/Format", recFormatCombo->currentText());
    iniSettings->setValue("Search/ResultLimit", searchResultsSpin->value());
    for (const HomePicker& p : m_homePickers) {
        if (p.amountSpin) iniSettings->setValue(p.amountKey, p.amountSpin->value());
        if (p.videosSpin) iniSettings->setValue(p.videosKey, p.videosSpin->value());
        if (p.shuffleBtn) iniSettings->setValue(p.shuffleKey, p.shuffleBtn->isChecked());
        if (p.continueCheck) iniSettings->setValue(p.continueKey, p.continueCheck->isChecked());
        if (p.lazyCheck) iniSettings->setValue(p.lazyKey, p.lazyCheck->isChecked());
    }
    iniSettings->setValue("Paths/HomeFolder", homeFolderEdit->text());
    iniSettings->setValue("Paths/DownloadFolder", dlFolderEdit->text());
    iniSettings->setValue("Paths/VideoFolder", videoFolderEdit->text());
    iniSettings->setValue("Paths/AudioFolder", audioFolderEdit->text());
    iniSettings->setValue("Paths/PhotoFolder", photoFolderEdit->text());
    iniSettings->setValue("Paths/RecordingFolder", recFolderEdit->text());
    iniSettings->setValue("Paths/PlaylistFolder", playlistFolderEdit->text());
    iniSettings->setValue("Paths/UnifyMedia", unifyCheck->isChecked());
    iniSettings->setValue("Paths/SmartSort", smartSortCheck->isChecked());
    iniSettings->setValue("Paths/UnifiedFolder", unifiedFolderEdit->text());

    // Force write to disk immediately rather than waiting for OS garbage collection
    iniSettings->sync();

    // Apply the chosen theme across the whole app — but only when it actually
    // changed. Re-applying re-polishes every widget (all the search cards), which
    // is a multi-second hitch, so we must not do it for unrelated setting changes.
    const QString theme = themeCombo->currentText();
    if (theme != m_appliedTheme) {
        Theme::apply(theme);
        m_appliedTheme = theme;
    }

    // Likewise only resize the Search cards when the size actually changed.
    const int cardPx = currentCardWidth();
    if (cardPx != m_appliedCardWidth) {
        emit cardWidthChanged(cardPx);
        m_appliedCardWidth = cardPx;
    }

    // Only re-size the player seek bar when its width actually changed.
    const int seekBarPx = PlayerControls::widthForSize(seekBarSizeCombo->currentText());
    if (seekBarPx != m_appliedSeekBarWidth) {
        emit seekBarSizeChanged(seekBarPx);
        m_appliedSeekBarWidth = seekBarPx;
    }

    // Cheap to re-apply (the player just re-reads a couple of values).
    emit visualizerSettingsChanged();
}

void Settings::onCookiesBrowserChanged(const QString& text) {
    if (m_loading) { m_prevCookiesChoice = text; return; } // programmatic load, no prompt

    // Warn only when turning the feature ON (None -> a browser), not when switching
    // browsers or turning it off. The warning can be permanently dismissed.
    const bool turningOn = (m_prevCookiesChoice == "None" && text != "None");
    if (turningOn && !iniSettings->value("Search/CookiesWarningAck", false).toBool()) {
        QMessageBox box(this);
        box.setIcon(QMessageBox::Warning);
        box.setWindowTitle("Use a spare account");
        box.setText("Do not use your main Google or YouTube account for this.");
        box.setInformativeText(
            "Seagull will send that browser's YouTube login with its requests. Google "
            "treats automated access (which is what a downloader looks like) as a Terms "
            "of Service violation, so the account you are signed into can be rate-limited, "
            "temporarily locked, or in rare cases suspended.\n\n"
            "Sign that browser into a spare or throwaway Google account first, then enable "
            "this. Anyone with the exported cookies can also access that session, so clear "
            "them with \"Delete Cached Cookie Data\" when you are done.");
        QCheckBox* dontShow = new QCheckBox("Don't show this again", &box);
        box.setCheckBox(dontShow);
        box.setStandardButtons(QMessageBox::Ok | QMessageBox::Cancel);
        box.button(QMessageBox::Ok)->setText("I understand, use cookies");
        box.setDefaultButton(QMessageBox::Cancel);

        if (box.exec() != QMessageBox::Ok) {
            // Declined: revert to Off without re-triggering this handler.
            cookiesBrowserCombo->blockSignals(true);
            cookiesBrowserCombo->setCurrentText("None");
            cookiesBrowserCombo->blockSignals(false);
            m_prevCookiesChoice = "None";
            saveSettings();
            return;
        }
        if (dontShow->isChecked())
            iniSettings->setValue("Search/CookiesWarningAck", true);
    }

    m_prevCookiesChoice = text;
    saveSettings();
}

void Settings::deleteCookieData() {
    if (QMessageBox::question(this, "Delete cached cookie data",
            "This clears yt-dlp's cache, including any login or session tokens it derived "
            "from your browser cookies.\n\nContinue?",
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
        return;

    // yt-dlp --rm-cache-dir wipes its cache directory (signature/token/PO caches).
    // Fire-and-forget; the tool path is the same one the workers use.
    const QString exe = QCoreApplication::applicationDirPath() + "/tools/yt-dlp.exe";
    const bool ok = QProcess::startDetached(exe, { "--rm-cache-dir" });
    QMessageBox::information(this, "Delete cached cookie data",
        ok ? "Cleared yt-dlp's cached login and session data."
           : "Could not run yt-dlp to clear its cache.");
}

void Settings::resetDefaults() {
    // Set everything quietly, then write + apply once.
    m_loading = true;
    autoUpdateCheck->setChecked(true);
    rememberPositionCheck->setChecked(true);
    clearHistoryOnCloseCheck->setChecked(false);
    darkModeBtn->setChecked(true);          // Seagull is a dark theme
    populateThemeCombo(true, "Seagull");
    cardSizeSlider->blockSignals(true);
    cardSizeSlider->setValue(300);
    cardSizeSlider->blockSignals(false);
    cardSizeCombo->setCurrentText("Large");
    cardSizeSlider->hide();
    seekBarSizeCombo->setCurrentText("Small");
    visualizerCombo->setCurrentText("Seagull Morning");
    behaviorCombo->setCurrentText("Drift");
    iniSettings->setValue("Visualizer/Behavior", "Drift");
    maxGullsSpin->setValue(14);
    killGullsCheck->setChecked(true);
    typeVideoBtn->setChecked(true);
    updateDownloadFormatOptions();
    formatCombo->setCurrentText("mp4");
    updateDownloadQualityOptions();
    dlQualityCombo->setCurrentText("Best Available");
    streamQualityCombo->setCurrentText("Best Available");
    cookiesBrowserCombo->setCurrentText("None");
    recTypeVideoBtn->setChecked(true);
    updateRecordingFormatOptions();
    recFormatCombo->setCurrentText("MP4");
    searchResultsSpin->setValue(20);
    for (const HomePicker& p : m_homePickers) {
        if (p.amountSpin) p.amountSpin->setValue(p.amountDefault);
        if (p.videosSpin) p.videosSpin->setValue(5);
        if (p.shuffleBtn) {
            p.shuffleBtn->setChecked(true);
            p.shuffleBtn->setText(QStringLiteral("Randomize order: On"));
        }
        if (p.continueCheck) p.continueCheck->setChecked(true);
        if (p.lazyCheck) p.lazyCheck->setChecked(false); // lazy loading off by default
        if (p.warning) p.warning->setVisible(false);
    }
    homeFolderEdit->setText(QCoreApplication::applicationDirPath());
    // The user's Windows folders, same as a fresh install's defaults.
    dlFolderEdit->setText(QStandardPaths::writableLocation(QStandardPaths::DownloadLocation));
    const QString movies = QStandardPaths::writableLocation(QStandardPaths::MoviesLocation);
    videoFolderEdit->setText(movies);
    audioFolderEdit->setText(QStandardPaths::writableLocation(QStandardPaths::MusicLocation));
    photoFolderEdit->setText(QStandardPaths::writableLocation(QStandardPaths::PicturesLocation));
    recFolderEdit->setText(movies + "/Recordings");
    unifyCheck->setChecked(false);
    unifiedFolderEdit->setText(movies);
    applyUnifyState();
    smartSortCheck->setChecked(true); // smart sort on by default
    applySmartSortState();
    m_loading = false;
    saveSettings();
}