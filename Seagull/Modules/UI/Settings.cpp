#include "Settings.h"
#include "Theme.h"
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QFormLayout>
#include <QFileDialog>
#include <QCoreApplication>
#include <QGroupBox>
#include <QPushButton>

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

    formatCombo = new QComboBox();
    // Comprehensive list of video and audio formats handled by yt-dlp
    formatCombo->addItems({
        "Best Available",
        "mp4", "mkv", "webm", "avi", "flv", "mov",
        "mp3", "m4a", "flac", "wav", "opus", "aac", "vorbis"
        });
    // Force a scrollbar after 8 items are shown
    formatCombo->setMaxVisibleItems(8);

    dlQualityCombo = new QComboBox();
    // Populated by updateDownloadQualityOptions() to match the selected format
    // (resolutions for video formats, bitrates for audio formats).
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

    dlLayout->addRow("Download Format:", formatCombo);
    dlLayout->addRow("Download Quality:", dlQualityCombo);
    dlLayout->addRow("Stream Quality:", streamQualityCombo);
    dlLayout->addRow("Home Directory:", homeLayout);
    dlLayout->addRow("Download Directory:", dlFolderLayout);

    stackedWidget->addWidget(dlWidget);

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
    connect(formatCombo, &QComboBox::currentTextChanged, this, [this]() {
        updateDownloadQualityOptions(); // resolutions vs bitrates for this format
        saveSettings();
        });
    connect(dlQualityCombo, &QComboBox::currentTextChanged, this, &Settings::saveSettings);
    connect(streamQualityCombo, &QComboBox::currentTextChanged, this, &Settings::saveSettings);
    connect(homeFolderEdit, &QLineEdit::textChanged, this, &Settings::saveSettings);
    connect(dlFolderEdit, &QLineEdit::textChanged, this, &Settings::saveSettings);

    // Set default tab
    sidebar->setCurrentRow(0);
}

void Settings::updateDownloadQualityOptions() {
    static const QStringList audioFormats = {
        "mp3", "m4a", "flac", "wav", "opus", "aac", "vorbis"
    };
    const QString fmt = formatCombo->currentText();
    const QString prev = dlQualityCombo->currentText();

    dlQualityCombo->blockSignals(true);
    dlQualityCombo->clear();
    if (audioFormats.contains(fmt)) {
        dlQualityCombo->addItems({
            "Best Available", "320 kbps", "256 kbps", "192 kbps", "128 kbps", "96 kbps"
            });
    }
    else { // video formats and "Best Available"
        dlQualityCombo->addItems({
            "Best Available", "4320p (8K)", "2160p (4K)", "1440p (2K)", "1080p", "720p", "480p", "360p"
            });
    }
    // Keep the previous choice if it's still valid, otherwise fall back to Best.
    int idx = dlQualityCombo->findText(prev);
    dlQualityCombo->setCurrentIndex(idx >= 0 ? idx : 0);
    dlQualityCombo->blockSignals(false);
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

    // Format first, then build the matching quality list, then select the saved quality.
    formatCombo->setCurrentText(iniSettings->value("Download/Format", "Best Available").toString());
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
    formatCombo->setCurrentText("Best Available");
    updateDownloadQualityOptions();
    dlQualityCombo->setCurrentText("Best Available");
    streamQualityCombo->setCurrentText("Best Available");
    homeFolderEdit->setText(QCoreApplication::applicationDirPath());
    dlFolderEdit->setText(QCoreApplication::applicationDirPath() + "/Downloads");
    m_loading = false;
    saveSettings();
}