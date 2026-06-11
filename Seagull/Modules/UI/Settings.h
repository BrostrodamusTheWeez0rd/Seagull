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

class Settings : public QWidget {
    Q_OBJECT

public:
    explicit Settings(QWidget* parent = nullptr);
    ~Settings();

signals:
    void cardWidthChanged(int width); // Display "Card size" -> Search card width (px)

private slots:
    void saveSettings();
    void loadSettings();
    void resetDefaults();
    void onCardSizeChanged();            // card-size combo -> show/hide slider + save

private:
    void setupUI();
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

    bool m_loading = false; // suppresses auto-save while loadSettings populates controls

    // What we last pushed to the rest of the app, so a save that didn't touch these
    // doesn't needlessly re-apply the global theme (a full re-polish of every widget)
    // or re-flow the search grid.
    QString m_appliedTheme;
    int     m_appliedCardWidth = -1;

    // Side tab layout components
    QListWidget* sidebar;
    QStackedWidget* stackedWidget;

    // Display Tab elements
    QComboBox* themeCombo;
    QComboBox* cardSizeCombo;   // Small / Medium / Large / Extra Large / Custom
    QSlider*   cardSizeSlider;  // shown only for Custom; spans Small..Extra Large

    // Download & Streaming Tab elements
    QButtonGroup* typeGroup;     // Video | Audio toggle
    QPushButton* typeVideoBtn;
    QPushButton* typeAudioBtn;
    QComboBox* formatCombo;
    QComboBox* dlQualityCombo;
    QComboBox* streamQualityCombo;
    QButtonGroup* recTypeGroup;  // Recording: Video | Audio toggle
    QPushButton* recTypeVideoBtn;
    QPushButton* recTypeAudioBtn;
    QComboBox* recFormatCombo;   // Recording container/codec, per the type
    QLineEdit* homeFolderEdit;
    QLineEdit* dlFolderEdit;     // downloads — dedicated, outside the unify system
    QLineEdit* videoFolderEdit;  // video downloads
    QLineEdit* audioFolderEdit;  // audio downloads / extractions
    QLineEdit* photoFolderEdit;  // saved images
    QLineEdit* recFolderEdit;    // where recordings + clips are saved

    // "Unify media folders": ticking swaps the four typed folder rows for one
    // Media Folder row (the unified row is hidden until then). Applies only to
    // the media folders (Videos/Audio/Photos/Recordings) — Home is untouched.
    QCheckBox* unifyCheck;
    QLineEdit* unifiedFolderEdit;
    QWidget*   unifiedFolderRow;        // the row container, for show/hide
    QList<QWidget*> typedFolderRows;    // the four per-type row containers
    QFormLayout* foldersForm = nullptr; // the form the folder rows live in

    // Search Tab elements
    QSpinBox* searchResultsSpin; // how many results a search fetches

    QPushButton* resetBtn;

    // The INI file handler
    QSettings* iniSettings;
};