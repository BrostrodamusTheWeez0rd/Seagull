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
class QPushButton;
class QFrame;

// Which content type's EQ is being viewed/edited. The equalizer applies to Audio
// media and Video media independently (Photo has no playback). Values double as the
// QButtonGroup ids for the Video/Audio pill.
enum class EqContentType { Video = 0, Audio = 1 };

// The equalizer (the Settings "Audio" page): a Video/Audio pill, a content-type-aware preset dropdown (stock +
// custom presets, with an "Add custom preset…" action), and a 10-band graphic EQ
// (libVLC's equalizer) adjusted in real time. Per-type state + custom presets persist
// to config.ini under Eq/. The tab edits/persists; VideoPlayer reads the saved EQ and
// applies it per media kind on play, and the orchestrator forwards live edits when the
// playing media's kind matches the edited type.
class EQ : public QWidget {
    Q_OBJECT
public:
    explicit EQ(QWidget* parent = nullptr);

    // Auto-follow the playing media kind. armFollow() re-enables following (called when
    // the Audio page is shown); followPlayingKind() switches the Video/Audio selector to
    // match what is playing, but only while still following — a manual pill click pins
    // the selection until the page is shown again. Both are silent (no live emit): they
    // only change which type is being viewed/edited, never what is playing.
    void armFollow();
    void followPlayingKind(EqContentType type);

signals:
    // A live edit for `type` (a slider moved, or a preset chosen). The orchestrator
    // applies it to playback only when the playing media's kind matches `type`.
    void eqChanged(EqContentType type, const QVector<float>& gains, float preampDb);

    // The power button toggled `type` on/off. Carries the current curve so the
    // orchestrator can apply it (on) or bypass the equalizer (off) live when the
    // playing media's kind matches `type`.
    void eqEnabledChanged(EqContentType type, bool enabled,
                          const QVector<float>& gains, float preampDb);

    // The Normalization power button toggled `type` on/off (peak protection: a
    // limiter/normaliser, independent of the EQ, persisted per type). The orchestrator
    // applies it live when the playing media's kind matches `type`.
    void normalizationChanged(EqContentType type, bool enabled);

private:
    struct Preset { QString name; float preamp = 0.0f; QVector<float> gains; };

    void buildUi();
    bool eventFilter(QObject* obj, QEvent* event) override;
    void positionPopup(ClickSlider* s);
    void setCustomState();
    void saveCurrentAsPreset();
    void retintSaveButton();
    void tintPowerGlyph(QPushButton* btn, bool on); // shared power-icon tint (accent on / dim off)
    void retintPowerButton();                   // tint the EQ power glyph to its on/off state
    void retintNormButton();                    // tint the Normalization power glyph
    void setEqEnabled(bool on);                  // power toggle: persist + reflect + live apply/bypass
    void setNormEnabled(bool on);                // normalization toggle: persist + reflect + live
    void setControlsEnabled(bool on);            // grey out the bands/presets when the EQ is off
    void changeEvent(QEvent* e) override;
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
    bool selectPresetByGains(const QVector<float>& gains, float preampDb);

    QVector<float> currentGains() const;
    float          currentPreamp() const;
    QString        ns() const;                   // "Eq/Audio/" or "Eq/Video/"
    QString        customArrayKey() const;       // "Eq/AudioCustom" or "Eq/VideoCustom"
    QVector<Preset> stockPresets() const;        // curated table for the current type
    QVector<Preset> customPresets();             // read from the QSettings array (non-const: beginReadArray)

    EqContentType m_type = EqContentType::Audio;
    int  m_bandCount = 10;
    bool m_loading = false;                      // suppress live-apply while setting sliders
    bool m_enabled = true;                       // current type's EQ on/off (power button), per-type
    bool m_normEnabled = true;                   // current type's normalization on/off, per-type
    bool m_followPlaying = true;                 // pill tracks the playing kind until a manual click pins it

    QButtonGroup*          m_typeGroup  = nullptr;
    QPushButton*           m_powerBtn = nullptr;     // top-right EQ on/off toggle for the current type
    QPushButton*           m_normBtn  = nullptr;     // Normalization on/off toggle for the current type
    QFrame*                m_bandFrame = nullptr;    // the sliders pill; greyed out while the EQ is off
    QComboBox*             m_presetCombo = nullptr;
    QVector<ClickSlider*>  m_bands;              // one per equalizer band
    ClickSlider*           m_preamp = nullptr;
    QTimer*                m_persistTimer = nullptr; // debounce config writes on drag
    QPushButton*           m_saveBtn = nullptr;
    QLabel*                m_popup = nullptr;        // live dB callout while dragging
    QTimer*                m_popupHideTimer = nullptr;
    QSettings              m_settings;
};

#endif // EQ_H
