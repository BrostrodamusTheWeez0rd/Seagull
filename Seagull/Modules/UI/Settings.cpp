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
    themeCombo->addItems({ "Seagull", "Dark", "Light" });
    displayLayout->addRow("Theme:", themeCombo);
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

    dlLayout->addRow("Download Type:", typeRow);
    dlLayout->addRow("Download Format:", formatCombo);
    dlLayout->addRow("Download Quality:", dlQualityCombo);
    dlLayout->addRow("Stream Quality:", streamQualityCombo);
    dlLayout->addRow("Home Directory:", homeLayout);
    dlLayout->addRow("Download Directory:", dlFolderLayout);

    stackedWidget->addWidget(dlWidget);

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
    connect(typeGroup, &QButtonGroup::buttonClicked, this, [this](QAbstractButton*) {
        onDownloadTypeChanged(); // refresh format + quality lists, then save
        });
    connect(formatCombo, &QComboBox::currentTextChanged, this, &Settings::saveSettings);
    connect(dlQualityCombo, &QComboBox::currentTextChanged, this, &Settings::saveSettings);
    connect(streamQualityCombo, &QComboBox::currentTextChanged, this, &Settings::saveSettings);
    connect(homeFolderEdit, &QLineEdit::textChanged, this, &Settings::saveSettings);
    connect(dlFolderEdit, &QLineEdit::textChanged, this, &Settings::saveSettings);

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

void Settings::loadSettings() {
    // Populate controls without each change auto-saving back to disk.
    m_loading = true;

    themeCombo->setCurrentText(iniSettings->value("Display/Theme", "Seagull").toString());

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

    homeFolderEdit->setText(iniSettings->value("Paths/HomeFolder", QCoreApplication::applicationDirPath()).toString());
    dlFolderEdit->setText(iniSettings->value("Paths/DownloadFolder", QCoreApplication::applicationDirPath() + "/Downloads").toString());

    m_loading = false;
}

void Settings::saveSettings() {
    if (m_loading) return; // ignore the change signals fired while loading

    // Write values to INI groups
    iniSettings->setValue("Display/Theme", themeCombo->currentText());
    iniSettings->setValue("Download/Type", currentDownloadType());
    iniSettings->setValue("Download/Format", formatCombo->currentText());
    iniSettings->setValue("Download/Quality", dlQualityCombo->currentText());
    iniSettings->setValue("Streaming/Quality", streamQualityCombo->currentText());
    iniSettings->setValue("Paths/HomeFolder", homeFolderEdit->text());
    iniSettings->setValue("Paths/DownloadFolder", dlFolderEdit->text());

    // Force write to disk immediately rather than waiting for OS garbage collection
    iniSettings->sync();

    // Apply the chosen theme across the whole app immediately.
    Theme::apply(themeCombo->currentText());
}

void Settings::resetDefaults() {
    // Set everything quietly, then write + apply once.
    m_loading = true;
    themeCombo->setCurrentText("Seagull");
    typeVideoBtn->setChecked(true);
    updateDownloadFormatOptions();
    formatCombo->setCurrentText("Best Available");
    updateDownloadQualityOptions();
    dlQualityCombo->setCurrentText("Best Available");
    streamQualityCombo->setCurrentText("Best Available");
    homeFolderEdit->setText(QCoreApplication::applicationDirPath());
    dlFolderEdit->setText(QCoreApplication::applicationDirPath() + "/Downloads");
    m_loading = false;
    saveSettings();
}