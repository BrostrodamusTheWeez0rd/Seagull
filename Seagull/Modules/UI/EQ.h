#ifndef EQ_H
#define EQ_H

#include <QWidget>
#include <QString>
#include <QVector>
#include <QSettings>

class QComboBox;
class QButtonGroup;
class QLabel;
class QTimer;
class ClickSlider;

// Which content type's EQ is being viewed/edited. The equalizer applies to Audio
// media and Video media independently (Photo has no playback). Values double as the
// QButtonGroup ids for the Video/Audio pill.
enum class EqContentType { Video = 0, Audio = 1 };

// The "EQ" tab: a Video/Audio pill, a content-type-aware preset dropdown (stock +
// custom presets, with an "Add custom preset…" action), and a 10-band graphic EQ
// (libVLC's equalizer) adjusted in real time. Per-type state + custom presets persist
// to config.ini under Eq/. The tab edits/persists; VideoPlayer reads the saved EQ and
// applies it per media kind on play, and the orchestrator forwards live edits when the
// playing media's kind matches the edited type.
class EQ : public QWidget {
    Q_OBJECT
public:
    explicit EQ(QWidget* parent = nullptr);

signals:
    // A live edit for `type` (a slider moved, or a preset chosen). The orchestrator
    // applies it to playback only when the playing media's kind matches `type`.
    void eqChanged(EqContentType type, const QVector<float>& gains, float preampDb);

private:
    struct Preset { QString name; float preamp = 0.0f; QVector<float> gains; };

    void buildUi();
    void selectType(EqContentType t);          // pill: switch the viewed/edited type (no emit)
    void populatePresets();                     // Custom + stock + custom + "Add custom preset…"
    void onPresetActivated(int index);          // user picked a dropdown entry
    void onSliderMoved();                        // any band / preamp slider moved
    void loadActiveIntoSliders();               // read Eq/<type>/* into the sliders (silent)
    void setSliders(const QVector<float>& gains, float preamp); // silent set (no emit)
    void emitLive();                             // emit eqChanged from the current slider state
    void persistActive();                        // write Eq/<type>/{Enabled,Gains,Preamp}
    void saveCustomPreset(const QString& name, const QVector<float>& gains, float preamp);
    void selectCustomByName(const QString& name);

    QVector<float> currentGains() const;
    float          currentPreamp() const;
    QString        ns() const;                   // "Eq/Audio/" or "Eq/Video/"
    QString        customArrayKey() const;       // "Eq/AudioCustom" or "Eq/VideoCustom"
    QVector<Preset> stockPresets() const;        // curated table for the current type
    QVector<Preset> customPresets();             // read from the QSettings array (non-const: beginReadArray)

    EqContentType m_type = EqContentType::Audio;
    int  m_bandCount = 10;
    bool m_loading = false;                      // suppress live-apply while setting sliders

    QButtonGroup*          m_typeGroup  = nullptr;
    QComboBox*             m_presetCombo = nullptr;
    QVector<ClickSlider*>  m_bands;              // one per equalizer band
    ClickSlider*           m_preamp = nullptr;
    QTimer*                m_persistTimer = nullptr; // debounce config writes on drag
    QSettings              m_settings;
};

#endif // EQ_H
