#include "Settings.h"
#include "Theme.h"
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QFormLayout>
#include <QFileDialog>
#include <QCoreApplication>
#include <QGroupBox>
#include <QPushButton>
#include <QTabWidget>
#include <QTextBrowser>
#include <QFile>
#include <QRegularExpression>

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
    // Force QSettings to create a "config.ini" right next to your .exe
    QString iniPath = QCoreApplication::applicationDirPath() + "/config.ini";
    iniSettings = new QSettings(iniPath, QSettings::IniFormat, this);

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
    sidebar->addItem("Display");
    sidebar->addItem("Download & Streaming");
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

    auto* homeLayout = new QHBoxLayout();
    homeFolderEdit = new QLineEdit();
    homeFolderEdit->setReadOnly(true); // Don't let users type invalid paths manually
    auto* homeBtn = new QPushButton("Choose Home Folder");
    homeLayout->addWidget(homeFolderEdit);
    homeLayout->addWidget(homeBtn);

    auto* dlFolderLayout = new QHBoxLayout();
    dlFolderEdit = new QLineEdit();
    dlFolderEdit->setReadOnly(true);
    auto* dlBtn = new QPushButton("Choose Download Folder");
    dlFolderLayout->addWidget(dlFolderEdit);
    dlFolderLayout->addWidget(dlBtn);

    auto* recFolderLayout = new QHBoxLayout();
    recFolderEdit = new QLineEdit();
    recFolderEdit->setReadOnly(true);
    recFolderEdit->setToolTip("Where the Record button saves recordings and clips.");
    auto* recBtn = new QPushButton("Choose Recording Folder");
    recFolderLayout->addWidget(recFolderEdit);
    recFolderLayout->addWidget(recBtn);

    dlLayout->addRow("Download Type:", typeRow);
    dlLayout->addRow("Download Format:", formatCombo);
    dlLayout->addRow("Download Quality:", dlQualityCombo);
    dlLayout->addRow("Stream Quality:", streamQualityCombo);
    dlLayout->addRow("Recording Type:", recTypeRow);
    dlLayout->addRow("Recording Format:", recFormatCombo);
    dlLayout->addRow("Recording Directory:", recFolderLayout);
    dlLayout->addRow("Home Directory:", homeLayout);
    dlLayout->addRow("Download Directory:", dlFolderLayout);

    stackedWidget->addWidget(dlWidget);

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

    stackedWidget->addWidget(searchWidget);

    // === Info Tab: bundled docs in a tabbed reader ===
    auto* infoWidget = new QWidget();
    auto* infoLayout = new QVBoxLayout(infoWidget);
    infoLayout->setContentsMargins(20, 20, 20, 20);

    auto* docTabs = new QTabWidget();

    // Read Me — drop the Screenshots section (its images live on disk/GitHub, not
    // in resources, so they'd render as broken icons in-app).
    QString readme = readDoc(":/docs/README.md");
    readme.remove(QRegularExpression("\\n## Screenshots[\\s\\S]*?(?=\\n## )"));
    auto* readmeView = new QTextBrowser();
    readmeView->setOpenExternalLinks(true);
    readmeView->setMarkdown(readme);
    docTabs->addTab(readmeView, "Read Me");

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

    // Connect folder browse buttons
    connect(homeBtn, &QPushButton::clicked, this, &Settings::browseHomeFolder);
    connect(dlBtn, &QPushButton::clicked, this, &Settings::browseDownloadFolder);
    connect(recBtn, &QPushButton::clicked, this, &Settings::browseRecordingFolder);

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
    connect(typeGroup, &QButtonGroup::buttonClicked, this, [this](QAbstractButton*) {
        onDownloadTypeChanged(); // refresh format + quality lists, then save
        });
    connect(formatCombo, &QComboBox::currentTextChanged, this, &Settings::saveSettings);
    connect(dlQualityCombo, &QComboBox::currentTextChanged, this, &Settings::saveSettings);
    connect(searchResultsSpin, &QSpinBox::valueChanged, this, &Settings::saveSettings);
    connect(streamQualityCombo, &QComboBox::currentTextChanged, this, &Settings::saveSettings);
    connect(recTypeGroup, &QButtonGroup::buttonClicked, this, [this](QAbstractButton*) {
        onRecordingTypeChanged(); // refresh the recording format list, then save
        });
    connect(recFormatCombo, &QComboBox::currentTextChanged, this, &Settings::saveSettings);
    connect(homeFolderEdit, &QLineEdit::textChanged, this, &Settings::saveSettings);
    connect(dlFolderEdit, &QLineEdit::textChanged, this, &Settings::saveSettings);
    connect(recFolderEdit, &QLineEdit::textChanged, this, &Settings::saveSettings);

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
    if (currentDownloadType() == "Audio")
        formatCombo->addItems({ "Best Available", "mp3", "m4a", "flac", "wav", "opus", "aac", "vorbis" });
    else
        formatCombo->addItems({ "Best Available", "mp4", "mkv", "webm", "avi", "flv", "mov" });
    int idx = formatCombo->findText(prev);
    formatCombo->setCurrentIndex(idx >= 0 ? idx : 0);
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
        recFormatCombo->addItems({ "MKV", "MP4", "TS" });
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

void Settings::browseHomeFolder() {
    QString dir = QFileDialog::getExistingDirectory(this, "Select Home Folder", homeFolderEdit->text());
    if (!dir.isEmpty()) {
        homeFolderEdit->setText(dir);
    }
}

void Settings::browseDownloadFolder() {
    QString dir = QFileDialog::getExistingDirectory(this, "Select Download Folder", dlFolderEdit->text());
    if (!dir.isEmpty()) {
        dlFolderEdit->setText(dir);
    }
}

void Settings::browseRecordingFolder() {
    QString dir = QFileDialog::getExistingDirectory(this, "Select Recording Folder", recFolderEdit->text());
    if (!dir.isEmpty()) {
        recFolderEdit->setText(dir);
    }
}

void Settings::loadSettings() {
    // Populate controls without each change auto-saving back to disk.
    m_loading = true;

    themeCombo->setCurrentText(iniSettings->value("Display/Theme", "Seagull").toString());

    // Card size: stored as a pixel width. Match it to a named preset, else Custom.
    int cardPx = qBound(kCardMinPx, iniSettings->value("Display/CardWidth", 240).toInt(), kCardMaxPx);
    cardSizeSlider->blockSignals(true);
    cardSizeSlider->setValue(cardPx);
    cardSizeSlider->blockSignals(false);
    const QString cardPreset = presetName(cardPx);
    cardSizeCombo->setCurrentText(cardPreset.isEmpty() ? "Custom" : cardPreset);
    cardSizeSlider->setVisible(cardPreset.isEmpty());

    // Type -> Format -> Quality cascade: set the type, build its format list, pick
    // the saved format, build the matching quality list, pick the saved quality.
    QString savedFormat = iniSettings->value("Download/Format", "Best Available").toString();
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

    // Recording: type drives the format list; the old Streaming/RecordFormat key
    // (pre recording settings) seeds the video format for existing configs.
    const QString recType = iniSettings->value("Recording/Type", "Video").toString();
    (recType == "Audio" ? recTypeAudioBtn : recTypeVideoBtn)->setChecked(true);
    updateRecordingFormatOptions();
    const QString legacyRecFmt = iniSettings->value("Streaming/RecordFormat", "MKV").toString().toUpper();
    recFormatCombo->setCurrentText(iniSettings->value("Recording/Format",
        recType == "Audio" ? QStringLiteral("M4A") : legacyRecFmt).toString());

    searchResultsSpin->setValue(iniSettings->value("Search/ResultLimit", 20).toInt());

    homeFolderEdit->setText(iniSettings->value("Paths/HomeFolder", QCoreApplication::applicationDirPath()).toString());
    dlFolderEdit->setText(iniSettings->value("Paths/DownloadFolder", QCoreApplication::applicationDirPath() + "/Downloads").toString());
    recFolderEdit->setText(iniSettings->value("Paths/RecordingFolder", dlFolderEdit->text()).toString());

    // Seed the "already applied" trackers so the first unrelated save doesn't trigger
    // a redundant theme re-apply / grid re-flow.
    m_appliedTheme = themeCombo->currentText();
    m_appliedCardWidth = currentCardWidth();

    m_loading = false;
}

void Settings::saveSettings() {
    if (m_loading) return; // ignore the change signals fired while loading

    // Write values to INI groups
    iniSettings->setValue("Display/Theme", themeCombo->currentText());
    iniSettings->setValue("Display/CardWidth", currentCardWidth());
    iniSettings->setValue("Download/Type", currentDownloadType());
    iniSettings->setValue("Download/Format", formatCombo->currentText());
    iniSettings->setValue("Download/Quality", dlQualityCombo->currentText());
    iniSettings->setValue("Streaming/Quality", streamQualityCombo->currentText());
    iniSettings->setValue("Recording/Type", currentRecordingType());
    iniSettings->setValue("Recording/Format", recFormatCombo->currentText());
    iniSettings->setValue("Search/ResultLimit", searchResultsSpin->value());
    iniSettings->setValue("Paths/HomeFolder", homeFolderEdit->text());
    iniSettings->setValue("Paths/DownloadFolder", dlFolderEdit->text());
    iniSettings->setValue("Paths/RecordingFolder", recFolderEdit->text());

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
}

void Settings::resetDefaults() {
    // Set everything quietly, then write + apply once.
    m_loading = true;
    themeCombo->setCurrentText("Seagull");
    cardSizeSlider->blockSignals(true);
    cardSizeSlider->setValue(240);
    cardSizeSlider->blockSignals(false);
    cardSizeCombo->setCurrentText("Medium");
    cardSizeSlider->hide();
    typeVideoBtn->setChecked(true);
    updateDownloadFormatOptions();
    formatCombo->setCurrentText("Best Available");
    updateDownloadQualityOptions();
    dlQualityCombo->setCurrentText("Best Available");
    streamQualityCombo->setCurrentText("Best Available");
    recTypeVideoBtn->setChecked(true);
    updateRecordingFormatOptions();
    recFormatCombo->setCurrentText("MKV");
    searchResultsSpin->setValue(20);
    homeFolderEdit->setText(QCoreApplication::applicationDirPath());
    dlFolderEdit->setText(QCoreApplication::applicationDirPath() + "/Downloads");
    recFolderEdit->setText(QCoreApplication::applicationDirPath() + "/Downloads");
    m_loading = false;
    saveSettings();
}