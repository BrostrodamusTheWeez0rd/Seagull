#pragma once

#include <QWidget>
#include <QSettings>
#include <QListWidget>
#include <QStackedWidget>
#include <QComboBox>
#include <QLineEdit>
#include <QPushButton>

class Settings : public QWidget {
    Q_OBJECT

public:
    explicit Settings(QWidget* parent = nullptr);
    ~Settings();

private slots:
    void browseHomeFolder();
    void browseDownloadFolder();
    void saveSettings();
    void loadSettings();
    void resetDefaults();

private:
    void setupUI();

    // Side tab layout components
    QListWidget* sidebar;
    QStackedWidget* stackedWidget;

    // Display Tab elements
    QComboBox* themeCombo;

    // Download & Streaming Tab elements
    QComboBox* formatCombo;
    QComboBox* dlQualityCombo;
    QComboBox* streamQualityCombo;
    QLineEdit* homeFolderEdit;
    QLineEdit* dlFolderEdit;

    QPushButton* applyBtn;
    QPushButton* resetBtn;

    // The INI file handler
    QSettings* iniSettings;
};