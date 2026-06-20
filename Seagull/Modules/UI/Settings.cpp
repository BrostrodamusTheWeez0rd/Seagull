#include "Settings.h"
#include "Theme.h"
#include "../Backend/SgPaths.h"
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

// The build stamps this in (see CMakeLists). Fallback keeps a stray build compiling.
#ifndef SEAGULL_VERSION
#define SEAGULL_VERSION "dev"
#endif

namespace {
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

void Settings::setupUI() {
    auto* outerLayout = new QVBoxLayout(this);
    auto* mainLayout = new QHBoxLayout();
    outerLayout->addLayout(mainLayout);

    // --- 1. Left Sidebar ---
    sidebar = new QListWidget(this);
    sidebar->setMaximumWidth(200);
    sidebar->addItem("General");
    sidebar->addItem("Display");
    sidebar->addItem("Download & Streaming");
    sidebar->addItem("Folders & Recording");
    sidebar->addItem("Search");
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
    autoUpdateCheck->setToolTip("Install yt-dlp / ffmpeg / Deno updates in the background "
        "at startup. When off, Seagull asks before updating.");
    generalLayout->addRow("Auto Update:", autoUpdateCheck);

    checkUpdatesBtn = new QPushButton("Check for Updates");
    checkUpdatesBtn->setToolTip("Check now for a newer version of Seagull.");
    generalLayout->addRow("", checkUpdatesBtn);
    connect(checkUpdatesBtn, &QPushButton::clicked, this, &Settings::checkForUpdatesRequested);

    stackedWidget->addWidget(generalWidget);

    // === Display Tab ===
    auto* displayWidget = new QWidget();
    auto* displayLayout = new QFormLayout(displayWidget);
    displayLayout->setContentsMargins(20, 20, 20, 20);

    themeCombo = new QComboBox();
    themeCombo->addItems(Theme::names());
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

    // Visualizer picker + its per-visualizer settings, swapped underneath it.
    visualizerCombo = new QComboBox();
    visualizerCombo->addItems({ "Seagull Sky", "Seagull Waves" });
    visualizerCombo->setToolTip("Which visualizer the player's visualizer button shows for audio.");
    displayLayout->addRow("Visualizer:", visualizerCombo);

    // All visualizer settings in one tight form folded under the picker. Behaviour,
    // the cap, and end-of-song are all global — they apply to whichever visualizer
    // is selected. (No QStackedWidget — its unstable size hint left a shifting gap
    // here.)
    auto* vizBlock = new QWidget();
    auto* vizForm = new QFormLayout(vizBlock);
    vizForm->setContentsMargins(0, 0, 0, 0);

    behaviorCombo = new QComboBox();
    behaviorCombo->addItems({ "Drift", "Reverse", "Swooping", "Flocking" });
    behaviorCombo->setToolTip("How the seagulls move: Drift (left to right), "
        "Reverse (right to left), Swooping, or Flocking.");
    vizForm->addRow("Seagull behavior:", behaviorCombo);

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

    // === Download & Streaming Tab ===
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

    auto* dlFolderRow = makeFolderRow(dlFolderEdit, "Select Downloads Folder",
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

    dlLayout->addRow("Downloads Folder:", dlFolderRow);
    dlLayout->addRow("Download Type:", typeRow);
    dlLayout->addRow("Download Format:", formatCombo);
    dlLayout->addRow("Download Quality:", dlQualityCombo);
    dlLayout->addRow("Stream Quality:", streamQualityCombo);

    stackedWidget->addWidget(dlWidget);

    // === Folders & Recording Tab ===
    // The media folder paths (the unify toggle swaps the four typed rows for the
    // single Media Folder row) with the recording settings underneath.
    auto* dirsWidget = new QWidget();
    auto* dirsLayout = new QFormLayout(dirsWidget);
    dirsLayout->setContentsMargins(20, 20, 20, 20);
    foldersForm = dirsLayout; // applyUnifyState shows/hides folder rows on it

    dirsLayout->addRow("Unify Folders:", unifyCheck);
    dirsLayout->addRow("Media Folder:", unifiedFolderRow);
    dirsLayout->addRow("Videos Folder:", videoFolderRow);
    dirsLayout->addRow("Audio Folder:", audioFolderRow);
    dirsLayout->addRow("Photos Folder:", photoFolderRow);
    dirsLayout->addRow("Recordings Folder:", recFolderRow);
    dirsLayout->addRow("Playlists Folder:", playlistFolderRow);
    dirsLayout->addRow("Home Directory:", homeRow);
    dirsLayout->addRow("Recording Type:", recTypeRow);
    dirsLayout->addRow("Recording Format:", recFormatCombo);

    stackedWidget->addWidget(dirsWidget);

    // === Search Tab ===
    auto* searchWidget = new QWidget();
    auto* searchLayout = new QFormLayout(searchWidget);
    searchLayout->setContentsMargins(20, 20, 20, 20);

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

    // Browser cookies: the most effective fix for YouTube's "confirm you're not a
    // bot" wall. When set, yt-dlp reuses that browser's logged-in session so the
    // requests look like a normal viewer. Off by default; turning it on pops a
    // warning about not using your main account (see onCookiesBrowserChanged).
    cookiesBrowserCombo = new QComboBox();
    cookiesBrowserCombo->addItems({ "None", "Firefox", "Chrome", "Edge", "Brave" });
    cookiesBrowserCombo->setToolTip(
        "Reuse a browser's YouTube login to cut down on \"confirm you're not a bot\" "
        "errors. Firefox is the most reliable on Windows. Chrome, Edge, and Brave can "
        "fail to read cookies while the browser is open or due to their cookie "
        "encryption. Use a spare account, never your main one.");
    searchLayout->addRow("Cookies from browser:", cookiesBrowserCombo);

    deleteCookiesBtn = new QPushButton("Delete Cached Cookie Data");
    deleteCookiesBtn->setMaximumWidth(220);
    deleteCookiesBtn->setToolTip("Clears yt-dlp's cache, including any login/session tokens it "
        "derived from your browser cookies. Use this after turning cookies off to leave nothing behind.");
    searchLayout->addRow("", deleteCookiesBtn);

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

    // --- Bottom button bar: Reset to Default only (settings auto-apply) ---
    auto* buttonBar = new QHBoxLayout();
    buttonBar->setContentsMargins(0, 10, 10, 10);
    resetBtn = new QPushButton("Reset to Default");
    buttonBar->addStretch();
    buttonBar->addWidget(resetBtn);
    outerLayout->addLayout(buttonBar);

    connect(resetBtn, &QPushButton::clicked, this, &Settings::resetDefaults);

    // Auto-apply: every control change writes config and applies immediately.
    connect(themeCombo, &QComboBox::currentTextChanged, this, &Settings::saveSettings);
    connect(cardSizeCombo, &QComboBox::currentTextChanged, this, &Settings::onCardSizeChanged);
    connect(cardSizeSlider, &QSlider::valueChanged, this, &Settings::saveSettings);
    connect(visualizerCombo, &QComboBox::currentTextChanged, this, &Settings::saveSettings);
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

void Settings::loadSettings() {
    // Populate controls without each change auto-saving back to disk.
    m_loading = true;

    autoUpdateCheck->setChecked(iniSettings->value("General/AutoUpdate", true).toBool());
    clearHistoryOnCloseCheck->setChecked(iniSettings->value("Search/ClearHistoryOnExit", false).toBool());

    themeCombo->setCurrentText(iniSettings->value("Display/Theme", "Seagull").toString());

    // Card size: stored as a pixel width. Match it to a named preset, else Custom.
    int cardPx = qBound(kCardMinPx, iniSettings->value("Display/CardWidth", 360).toInt(), kCardMaxPx); // default Extra Large
    cardSizeSlider->blockSignals(true);
    cardSizeSlider->setValue(cardPx);
    cardSizeSlider->blockSignals(false);
    const QString cardPreset = presetName(cardPx);
    cardSizeCombo->setCurrentText(cardPreset.isEmpty() ? "Custom" : cardPreset);
    cardSizeSlider->setVisible(cardPreset.isEmpty());

    visualizerCombo->setCurrentText(iniSettings->value("Visualizer/Type", "Seagull Sky").toString());
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
    cookiesBrowserCombo->setCurrentText(iniSettings->value("Streaming/CookiesBrowser", "None").toString());
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

    // Seed the "already applied" trackers so the first unrelated save doesn't trigger
    // a redundant theme re-apply / grid re-flow.
    m_appliedTheme = themeCombo->currentText();
    m_appliedCardWidth = currentCardWidth();

    m_loading = false;
}

void Settings::saveSettings() {
    if (m_loading) return; // ignore the change signals fired while loading

    // Write values to INI groups
    iniSettings->setValue("General/AutoUpdate", autoUpdateCheck->isChecked());
    iniSettings->setValue("Search/ClearHistoryOnExit", clearHistoryOnCloseCheck->isChecked());
    iniSettings->setValue("Display/Theme", themeCombo->currentText());
    iniSettings->setValue("Display/CardWidth", currentCardWidth());
    iniSettings->setValue("Visualizer/Type", visualizerCombo->currentText());
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
    iniSettings->setValue("Paths/HomeFolder", homeFolderEdit->text());
    iniSettings->setValue("Paths/DownloadFolder", dlFolderEdit->text());
    iniSettings->setValue("Paths/VideoFolder", videoFolderEdit->text());
    iniSettings->setValue("Paths/AudioFolder", audioFolderEdit->text());
    iniSettings->setValue("Paths/PhotoFolder", photoFolderEdit->text());
    iniSettings->setValue("Paths/RecordingFolder", recFolderEdit->text());
    iniSettings->setValue("Paths/PlaylistFolder", playlistFolderEdit->text());
    iniSettings->setValue("Paths/UnifyMedia", unifyCheck->isChecked());
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
    clearHistoryOnCloseCheck->setChecked(false);
    themeCombo->setCurrentText("Seagull");
    cardSizeSlider->blockSignals(true);
    cardSizeSlider->setValue(360);
    cardSizeSlider->blockSignals(false);
    cardSizeCombo->setCurrentText("Extra Large");
    cardSizeSlider->hide();
    visualizerCombo->setCurrentText("Seagull Sky");
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
    m_loading = false;
    saveSettings();
}