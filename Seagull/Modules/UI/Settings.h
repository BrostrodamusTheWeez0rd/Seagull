#pragma once

#include <QWidget>
#include <QSettings>
#include <QListWidget>
#include <QStackedWidget>
#include <QComboBox>
#include <QLineEdit>
#include <QPushButton>
#include <QButtonGroup>

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
    void updateDownloadFormatOptions();  // repopulate format list from the Download Type toggle
    void updateDownloadQualityOptions(); // repopulate quality list (resolutions vs bitrates)
    void onDownloadTypeChanged();        // type toggle -> refresh formats + qualities + save
    QString currentDownloadType() const; // "Video" or "Audio"

    bool m_loading = false; // suppresses auto-save while loadSettings populates controls

    // Side tab layout components
    QListWidget* sidebar;
    QStackedWidget* stackedWidget;

    // Display Tab elements
    QComboBox* themeCombo;

    // Download & Streaming Tab elements
    QButtonGroup* typeGroup;     // Video | Audio toggle
    QPushButton* typeVideoBtn;
    QPushButton* typeAudioBtn;
    QComboBox* formatCombo;
    QComboBox* dlQualityCombo;
    QComboBox* streamQualityCombo;
    QLineEdit* homeFolderEdit;
    QLineEdit* dlFolderEdit;

    QPushButton* resetBtn;

    // The INI file handler
    QSettings* iniSettings;
};