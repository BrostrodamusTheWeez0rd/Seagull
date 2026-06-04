#include "Settings.h"
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QFormLayout>
#include <QFileDialog>
#include <QCoreApplication>
#include <QGroupBox>

Settings::Settings(QWidget* parent) : QWidget(parent) {
    // Force QSettings to create a "config.ini" right next to your .exe
    QString iniPath = QCoreApplication::applicationDirPath() + "/config.ini";
    iniSettings = new QSettings(iniPath, QSettings::IniFormat, this);

    setupUI();
    loadSettings();

    // Auto-save whenever a dropdown value changes
    connect(themeCombo, &QComboBox::currentTextChanged, this, &Settings::saveSettings);
    connect(formatCombo, &QComboBox::currentTextChanged, this, &Settings::saveSettings);
    connect(dlQualityCombo, &QComboBox::currentTextChanged, this, &Settings::saveSettings);
    connect(streamQualityCombo, &QComboBox::currentTextChanged, this, &Settings::saveSettings);
}

Settings::~Settings() {
    // iniSettings is parented to 'this', so it cleans up automatically
}

void Settings::setupUI() {
    auto* mainLayout = new QHBoxLayout(this);

    // --- 1. Left Sidebar ---
    sidebar = new QListWidget(this);
    sidebar->setMaximumWidth(200);
    sidebar->addItem("Display");
    sidebar->addItem("Download & Streaming");
    // Simple styling for a flat, modern look
    sidebar->setStyleSheet(
        "QListWidget { border: none; background-color: transparent; outline: none; }"
        "QListWidget::item { padding: 10px; border-radius: 5px; }"
        "QListWidget::item:selected { background-color: rgba(255, 255, 255, 30); }"
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
    // Unified quality list starting with Best Available
    dlQualityCombo->addItems({
        "Best Available",
        "4320p (8K)", "2160p (4K)", "1440p (2K)", "1080p", "720p", "480p", "360p",
        "320 kbps", "256 kbps", "192 kbps", "128 kbps", "96 kbps"
        });
    dlQualityCombo->setMaxVisibleItems(8);

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

    dlLayout->addRow("Default Format:", formatCombo);
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

    // Set default tab
    sidebar->setCurrentRow(0);
}

void Settings::browseHomeFolder() {
    QString dir = QFileDialog::getExistingDirectory(this, "Select Home Folder", homeFolderEdit->text());
    if (!dir.isEmpty()) {
        homeFolderEdit->setText(dir);
        saveSettings();
    }
}

void Settings::browseDownloadFolder() {
    QString dir = QFileDialog::getExistingDirectory(this, "Select Download Folder", dlFolderEdit->text());
    if (!dir.isEmpty()) {
        dlFolderEdit->setText(dir);
        saveSettings();
    }
}

void Settings::loadSettings() {
    // Read from INI, apply fallback defaults if the key doesn't exist yet
    themeCombo->setCurrentText(iniSettings->value("Display/Theme", "Seagull").toString());
    formatCombo->setCurrentText(iniSettings->value("Download/Format", "Best Available").toString());
    dlQualityCombo->setCurrentText(iniSettings->value("Download/Quality", "Best Available").toString());
    streamQualityCombo->setCurrentText(iniSettings->value("Streaming/Quality", "Best Available").toString());

    homeFolderEdit->setText(iniSettings->value("Paths/HomeFolder", QCoreApplication::applicationDirPath()).toString());
    dlFolderEdit->setText(iniSettings->value("Paths/DownloadFolder", QCoreApplication::applicationDirPath() + "/Downloads").toString());
}

void Settings::saveSettings() {
    // Write values to INI groups
    iniSettings->setValue("Display/Theme", themeCombo->currentText());
    iniSettings->setValue("Download/Format", formatCombo->currentText());
    iniSettings->setValue("Download/Quality", dlQualityCombo->currentText());
    iniSettings->setValue("Streaming/Quality", streamQualityCombo->currentText());
    iniSettings->setValue("Paths/HomeFolder", homeFolderEdit->text());
    iniSettings->setValue("Paths/DownloadFolder", dlFolderEdit->text());

    // Force write to disk immediately rather than waiting for OS garbage collection
    iniSettings->sync();
}