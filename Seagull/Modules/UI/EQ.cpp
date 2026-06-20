#include "EQ.h"
#include "Widgets/ClickSlider.h"
#include "../Backend/PlaybackEngine.h"  // band count / frequencies (static)
#include "../Backend/SgPaths.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QPushButton>
#include <QButtonGroup>
#include <QFrame>
#include <QComboBox>
#include <QLabel>
#include <QInputDialog>
#include <QLineEdit>
#include <QTimer>
#include <QCoreApplication>
#include <QStringList>
#include <QVariantMap>
#include <QPainter>
#include <QEvent>
#include <QtGlobal>

namespace {
constexpr int kGainRange = 20; // ±dB, matches VLC's internal clamp

// "31", "1k", "16k" — band-frequency caption.
QString formatFreq(float hz) {
    if (hz <= 0) return QString();
    if (hz >= 1000.0f) {
        const float k = hz / 1000.0f;
        return (k == int(k)) ? QStringLiteral("%1k").arg(int(k))
                             : QStringLiteral("%1k").arg(k, 0, 'f', 1);
    }
    return QString::number(int(hz));
}

QString gainsToCsv(const QVector<float>& g) {
    QStringList parts;
    for (float v : g) parts << QString::number(v, 'f', 1);
    return parts.join(',');
}
QVector<float> csvToGains(const QString& csv) {
    QVector<float> g;
    const QStringList parts = csv.split(',', Qt::SkipEmptyParts);
    for (const QString& p : parts) g << p.toFloat();
    return g;
}

// dB scale painted alongside the sliders. Heights mirror the slider groove so
// tick labels land at the correct positions relative to handle travel.
// Every 5 dB is labeled; 10 dB anchors are brighter, 5 dB intermediates dimmer.
class EqScaleWidget : public QWidget {
public:
    explicit EqScaleWidget(QWidget* parent = nullptr) : QWidget(parent) {
        setFixedWidth(32);
        setMinimumHeight(170);
        setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
    }
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, false);
        QFont f = font();
        f.setPixelSize(9);
        p.setFont(f);

        const QColor base = palette().color(QPalette::WindowText);
        // QSS handle height 12px → half = 6; groove runs [hh .. height()-hh].
        const int hh = 6;
        const int travel = height() - 2 * hh;
        const int tickW = 3; // horizontal tick stub extending right from the label

        for (int db = 20; db >= -20; db -= 5) {
            const float t = (20.0f - db) / 40.0f;
            const int y   = hh + qRound(t * travel);

            const bool major = (db % 10 == 0);
            QColor c = base;
            c.setAlphaF(major ? 0.65 : 0.32);
            p.setPen(c);

            const QString s = db > 0 ? QStringLiteral("+%1").arg(db)
                                     : (db == 0 ? QStringLiteral("0")
                                                : QString::number(db));
            // Label right-aligned, leaving tickW px on the right for the stub.
            p.drawText(QRect(0, y - 5, width() - tickW - 2, 10),
                       Qt::AlignRight | Qt::AlignVCenter, s);
            // Tick stub
            p.drawLine(width() - tickW, y, width() - 1, y);
        }
    }
};
} // namespace

EQ::EQ(QWidget* parent)
    : QWidget(parent),
      m_settings(SgPaths::configFile(), QSettings::IniFormat) {
    buildUi();

    m_persistTimer = new QTimer(this);
    m_persistTimer->setSingleShot(true);
    m_persistTimer->setInterval(300); // coalesce config writes while dragging
    connect(m_persistTimer, &QTimer::timeout, this, [this]() { persistActive(); });

    populatePresets();
    loadActiveIntoSliders(); // show the active type's saved curve (no emit on startup)
}

void EQ::buildUi() {
    m_bandCount = PlaybackEngine::equalizerBandCount();
    if (m_bandCount <= 0) m_bandCount = 10; // defensive

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(16, 16, 16, 16);
    root->setSpacing(14);

    // --- 1 + 2. Shared container: type pill and preset combo share the same width ---
    // Fixed size policy on the container means it sizes to its sizeHint (the wider of
    // the two children). Both children have Expanding policy so they fill that width.
    auto* topGroup = new QWidget(this);
    topGroup->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
    auto* topLayout = new QVBoxLayout(topGroup);
    topLayout->setContentsMargins(0, 0, 0, 0);
    topLayout->setSpacing(8);

    // --- 1. Video / Audio pill (same idiom as the Library type pill) ---
    auto* pill = new QFrame(topGroup);
    pill->setObjectName("eqTypePill");
    pill->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    auto* pillLay = new QHBoxLayout(pill);
    pillLay->setContentsMargins(8, 5, 8, 5);
    pillLay->setSpacing(4);
    m_typeGroup = new QButtonGroup(this);
    m_typeGroup->setExclusive(true);
    const struct { const char* label; EqContentType type; } kinds[] = {
        { "Video", EqContentType::Video },
        { "Audio", EqContentType::Audio },
    };
    for (const auto& k : kinds) {
        auto* b = new QPushButton(k.label, pill);
        b->setObjectName("eqTypeButton");
        b->setCheckable(true);
        b->setCursor(Qt::PointingHandCursor);
        b->setChecked(k.type == m_type);
        m_typeGroup->addButton(b, static_cast<int>(k.type));
        pillLay->addWidget(b);
    }
    connect(m_typeGroup, &QButtonGroup::idClicked, this,
            [this](int id) { selectType(static_cast<EqContentType>(id)); });
    topLayout->addWidget(pill);

    // --- 2. Preset dropdown + save button ---
    m_presetCombo = new QComboBox(topGroup);
    m_presetCombo->setObjectName("eqPresetCombo");
    m_presetCombo->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_presetCombo->setEditable(true);
    m_presetCombo->setInsertPolicy(QComboBox::NoInsert);
    m_presetCombo->setCompleter(nullptr);
    m_presetCombo->lineEdit()->setReadOnly(true);
    m_presetCombo->lineEdit()->setAlignment(Qt::AlignCenter);
    m_presetCombo->setCursor(Qt::PointingHandCursor);
    m_presetCombo->lineEdit()->installEventFilter(this);
    connect(m_presetCombo, &QComboBox::activated, this, &EQ::onPresetActivated);

    m_saveBtn = new QPushButton(topGroup);
    m_saveBtn->setObjectName("eqSaveButton");
    m_saveBtn->setFixedSize(26, 26);
    m_saveBtn->setIconSize(QSize(15, 15));
    m_saveBtn->setToolTip("Save preset");
    m_saveBtn->setCursor(Qt::PointingHandCursor);
    m_saveBtn->hide();
    connect(m_saveBtn, &QPushButton::clicked, this, &EQ::saveCurrentAsPreset);
    connect(m_presetCombo, &QComboBox::currentIndexChanged, this, [this](int idx) {
        m_saveBtn->setVisible(idx < 0);
    });

    auto* presetRow = new QHBoxLayout();
    presetRow->setSpacing(4);
    presetRow->setContentsMargins(0, 0, 0, 0);
    presetRow->addWidget(m_presetCombo);
    presetRow->addWidget(m_saveBtn);
    topLayout->addLayout(presetRow);

    // --- Power toggle: pinned top-right, turns the current type's EQ on/off ---
    m_powerBtn = new QPushButton(this);
    m_powerBtn->setObjectName("eqPowerButton");
    m_powerBtn->setFixedSize(26, 26);
    m_powerBtn->setIconSize(QSize(16, 16));
    m_powerBtn->setToolTip("Turn the equalizer on or off");
    m_powerBtn->setCursor(Qt::PointingHandCursor);
    connect(m_powerBtn, &QPushButton::clicked, this, [this]() { setEqEnabled(!m_enabled); });

    // Header row: keep the type pill / preset combo centred while the power button
    // sits at the far right. A left spacer the width of the button balances it.
    auto* headerRow = new QHBoxLayout();
    headerRow->setContentsMargins(0, 0, 0, 0);
    headerRow->setSpacing(0);
    headerRow->addSpacing(26);
    headerRow->addStretch(1);
    headerRow->addWidget(topGroup);
    headerRow->addStretch(1);
    headerRow->addWidget(m_powerBtn, 0, Qt::AlignTop);
    root->addLayout(headerRow);

    // --- Drag popup: follows the handle, shows live dB value ---
    m_popup = new QLabel(this, Qt::ToolTip | Qt::FramelessWindowHint);
    m_popup->setObjectName("eqSliderPopup");
    m_popup->setAttribute(Qt::WA_ShowWithoutActivating);
    m_popup->setAlignment(Qt::AlignCenter);
    m_popup->hide();

    m_popupHideTimer = new QTimer(this);
    m_popupHideTimer->setSingleShot(true);
    m_popupHideTimer->setInterval(500);
    connect(m_popupHideTimer, &QTimer::timeout, m_popup, &QLabel::hide);

    // --- 3. Graphic EQ: pilled frame containing preamp + band columns ---
    auto* bandFrame = new QFrame(this);
    m_bandFrame = bandFrame;
    bandFrame->setObjectName("eqBandFrame");
    auto* bandFrameLayout = new QVBoxLayout(bandFrame);
    bandFrameLayout->setContentsMargins(12, 14, 12, 14);
    bandFrameLayout->setSpacing(0);

    auto* eqRow = new QHBoxLayout();
    eqRow->setSpacing(4);
    bandFrameLayout->addLayout(eqRow);

    auto makeColumn = [this, eqRow](const QString& caption, const QString& objName) -> ClickSlider* {
        auto* col = new QVBoxLayout();
        col->setSpacing(2);
        auto* s = new ClickSlider(Qt::Vertical, this);
        s->setObjectName(objName);
        s->setRange(-kGainRange, kGainRange);
        s->setValue(0);
        s->setMinimumHeight(170);
        connect(s, &QSlider::valueChanged, this, [this, s](int) {
            // Show the dB callout for any user-driven change — including scroll-wheel
            // and arrow keys, which never emit sliderPressed. During a handle drag the
            // press/release pair owns the popup; on a wheel tick there's no release, so
            // (re)arm the auto-hide so it fades shortly after scrolling stops.
            if (!m_loading) {
                positionPopup(s);
                m_popup->show();
                if (!s->isSliderDown()) m_popupHideTimer->start();
            }
            onSliderMoved();
        });
        connect(s, &QSlider::sliderPressed, this, [this, s]() {
            m_popupHideTimer->stop();
            positionPopup(s);
            m_popup->show();
        });
        connect(s, &QSlider::sliderReleased, this, [this]() {
            m_popupHideTimer->start(500);
        });
        col->addWidget(s, 1, Qt::AlignHCenter);
        auto* cap = new QLabel(caption, this);
        cap->setObjectName("eqBandLabel");
        cap->setAlignment(Qt::AlignHCenter);
        col->addWidget(cap);
        eqRow->addLayout(col);
        return s;
    };

    // dB scale column — same layout structure (widget + dummy label) as slider
    // columns so the scale widget's height matches the sliders exactly.
    {
        auto* scaleCol = new QVBoxLayout();
        scaleCol->setSpacing(2);
        auto* scaleWidget = new EqScaleWidget(this);
        scaleCol->addWidget(scaleWidget, 1);
        auto* dummy = new QLabel("", this);
        dummy->setObjectName("eqBandLabel");
        scaleCol->addWidget(dummy);
        eqRow->addLayout(scaleCol);
        eqRow->addSpacing(6);
    }

    m_preamp = makeColumn("Preamp", "eqPreampSlider");
    eqRow->addSpacing(6);

    for (int i = 0; i < m_bandCount; ++i)
        m_bands << makeColumn(formatFreq(PlaybackEngine::equalizerBandFrequency(i)), "eqSlider");

    root->addWidget(bandFrame, 1);
    retintSaveButton();
    retintPowerButton();
}

// --- Type / presets ---------------------------------------------------------

void EQ::selectType(EqContentType t) {
    if (t == m_type) return;
    m_type = t;
    populatePresets();
    loadActiveIntoSliders(); // viewing-only: never emit (don't disturb what's playing)
}

void EQ::populatePresets() {
    m_presetCombo->blockSignals(true);
    m_presetCombo->clear();
    auto add = [this](const QString& text, const QString& role, const QVariant& id) {
        QVariantMap d;
        d["role"] = role;
        if (id.isValid()) d["id"] = id;
        m_presetCombo->addItem(text, d);
    };
    const QVector<Preset> stock = stockPresets();
    for (int i = 0; i < stock.size(); ++i) add(stock[i].name, "stock", i);
    const QVector<Preset> custom = customPresets();
    for (const Preset& p : custom) add(p.name, "custom", p.name);
    m_presetCombo->setCurrentIndex(-1);
    m_presetCombo->blockSignals(false);
}

void EQ::onPresetActivated(int index) {
    if (index < 0) return;
    const QVariantMap d = m_presetCombo->itemData(index).toMap();
    const QString role = d.value("role").toString();

    QVector<float> gains;
    float preamp = 0.0f;
    if (role == "stock") {
        const QVector<Preset> stock = stockPresets();
        const int i = d.value("id").toInt();
        if (i < 0 || i >= stock.size()) return;
        gains = stock[i].gains;
        preamp = stock[i].preamp;
    } else if (role == "custom") {
        const QString name = d.value("id").toString();
        bool found = false;
        for (const Preset& p : customPresets())
            if (p.name == name) { gains = p.gains; preamp = p.preamp; found = true; break; }
        if (!found) return;
    } else {
        return;
    }
    setSliders(gains, preamp); // silent
    emitLive();                // apply now (orchestrator gates on the playing kind)
    persistActive();           // becomes this type's active curve
}

// --- Slider events ----------------------------------------------------------

void EQ::onSliderMoved() {
    if (m_loading) return;                  // programmatic set, not a user drag
    setCustomState();
    emitLive();                             // real-time apply
    m_persistTimer->start();                // debounced persist
}

void EQ::emitLive() {
    emit eqChanged(m_type, currentGains(), currentPreamp());
}

bool EQ::eventFilter(QObject* obj, QEvent* event) {
    if (obj == m_presetCombo->lineEdit() && event->type() == QEvent::MouseButtonPress) {
        m_presetCombo->showPopup();
        return true;
    }
    return QWidget::eventFilter(obj, event);
}

void EQ::setCustomState() {
    m_presetCombo->setCurrentIndex(-1);
    m_presetCombo->lineEdit()->setText(QStringLiteral("Custom"));
    if (m_saveBtn) m_saveBtn->setVisible(true);
}

void EQ::saveCurrentAsPreset() {
    const QVector<float> gains = currentGains();
    const float preamp = currentPreamp();
    bool ok = false;
    const QString name = QInputDialog::getText(this, "Save EQ Preset",
        "Name this preset:", QLineEdit::Normal, QString(), &ok).trimmed();
    if (!ok || name.isEmpty()) return;
    saveCustomPreset(name, gains, preamp);
    populatePresets();
    selectCustomByName(name);
}

void EQ::retintSaveButton() {
    if (!m_saveBtn) return;
    QPixmap pm = QIcon(QStringLiteral(":/Assets/icons/save.svg")).pixmap(QSize(15, 15));
    QPainter p(&pm);
    p.setCompositionMode(QPainter::CompositionMode_SourceIn);
    p.fillRect(pm.rect(), palette().color(QPalette::WindowText));
    p.end();
    m_saveBtn->setIcon(QIcon(pm));
}

void EQ::retintPowerButton() {
    if (!m_powerBtn) return;
    // Lit in the accent colour when on; dimmed to a faint outline when off.
    QColor c = m_enabled ? palette().color(QPalette::Highlight)
                         : palette().color(QPalette::WindowText);
    if (!m_enabled) c.setAlphaF(0.40);
    QPixmap pm = QIcon(QStringLiteral(":/Assets/icons/power.svg")).pixmap(QSize(16, 16));
    QPainter p(&pm);
    p.setCompositionMode(QPainter::CompositionMode_SourceIn);
    p.fillRect(pm.rect(), c);
    p.end();
    m_powerBtn->setIcon(QIcon(pm));
}

void EQ::setControlsEnabled(bool on) {
    // Grey out the bands + preset picker while the EQ is off; the Video/Audio pill
    // stays live so each type can still be switched and toggled independently.
    // Disable the sliders directly (not just the frame) so the :disabled handle
    // styling reliably kicks in regardless of widget parenting.
    if (m_bandFrame)   m_bandFrame->setEnabled(on);
    for (ClickSlider* s : m_bands) if (s) s->setEnabled(on);
    if (m_preamp)      m_preamp->setEnabled(on);
    if (m_presetCombo) m_presetCombo->setEnabled(on);
    if (m_saveBtn)     m_saveBtn->setEnabled(on);
}

void EQ::setEqEnabled(bool on) {
    m_enabled = on;
    m_settings.setValue(ns() + "Enabled", on); // per-type, persisted immediately
    m_settings.sync();
    retintPowerButton();
    setControlsEnabled(on);
    // Apply the current curve (on) or bypass (off) live; the orchestrator gates on
    // whether the playing media's kind matches this type.
    emit eqEnabledChanged(m_type, on, currentGains(), currentPreamp());
}

void EQ::changeEvent(QEvent* e) {
    if (e->type() == QEvent::PaletteChange) {
        retintSaveButton();
        retintPowerButton();
    }
    QWidget::changeEvent(e);
}

void EQ::positionPopup(ClickSlider* s) {
    const int val = s->value();

    QString label;
    if (s == m_preamp) {
        label = QStringLiteral("Pre");
    } else {
        const int i = m_bands.indexOf(s);
        if (i >= 0)
            label = formatFreq(PlaybackEngine::equalizerBandFrequency(i));
    }

    const QString db = val > 0 ? QStringLiteral("+%1 dB").arg(val)
                               : val < 0 ? QStringLiteral("%1 dB").arg(val)
                                         : QStringLiteral("0 dB");
    m_popup->setText(label.isEmpty() ? db : label + "  " + db);
    m_popup->adjustSize();

    // Same handle-position formula as EqScaleWidget (QSS handle height = 12px).
    const int hh = 6;
    const int travel = s->height() - 2 * hh;
    const float t = float(s->maximum() - val) / float(s->maximum() - s->minimum());
    const int handleY = hh + qRound(t * travel);
    const QPoint hCenter = s->mapToGlobal(QPoint(s->width() / 2, handleY));
    m_popup->move(hCenter.x() - m_popup->width() / 2,
                  hCenter.y() - m_popup->height() - 6);
}

void EQ::setSliders(const QVector<float>& gains, float preamp) {
    m_loading = true;
    for (int i = 0; i < m_bands.size(); ++i)
        m_bands[i]->setValue(i < gains.size() ? qRound(gains[i]) : 0);
    if (m_preamp) m_preamp->setValue(qRound(preamp));
    m_loading = false;
}

QVector<float> EQ::currentGains() const {
    QVector<float> g;
    g.reserve(m_bands.size());
    for (ClickSlider* s : m_bands) g << float(s->value());
    return g;
}

float EQ::currentPreamp() const {
    return m_preamp ? float(m_preamp->value()) : 0.0f;
}

// --- Persistence ------------------------------------------------------------

QString EQ::ns() const {
    return m_type == EqContentType::Audio ? QStringLiteral("Eq/Audio/")
                                          : QStringLiteral("Eq/Video/");
}
QString EQ::customArrayKey() const {
    return m_type == EqContentType::Audio ? QStringLiteral("Eq/AudioCustom")
                                          : QStringLiteral("Eq/VideoCustom");
}

void EQ::loadActiveIntoSliders() {
    const QString n = ns();
    const float preamp = m_settings.value(n + "Preamp", 0.0).toFloat();
    QVector<float> gains = csvToGains(m_settings.value(n + "Gains").toString());
    if (gains.size() != m_bandCount) gains = QVector<float>(m_bandCount, 0.0f); // default flat
    setSliders(gains, preamp);
    if (!selectPresetByGains(gains, preamp))
        setCustomState();
    // Reflect this type's saved power state (default on). Silent: this is a view
    // load, so it never emits — it must not disturb what's currently playing.
    m_enabled = m_settings.value(n + "Enabled", true).toBool();
    retintPowerButton();
    setControlsEnabled(m_enabled);
}

void EQ::persistActive() {
    const QString n = ns();
    m_settings.setValue(n + "Enabled", m_enabled); // power button owns this
    m_settings.setValue(n + "Preamp", currentPreamp());
    m_settings.setValue(n + "Gains", gainsToCsv(currentGains()));
    m_settings.sync();
}

QVector<EQ::Preset> EQ::customPresets() {
    QVector<Preset> out;
    const int n = m_settings.beginReadArray(customArrayKey());
    for (int i = 0; i < n; ++i) {
        m_settings.setArrayIndex(i);
        Preset p;
        p.name = m_settings.value("Name").toString();
        p.preamp = m_settings.value("Preamp", 0.0).toFloat();
        p.gains = csvToGains(m_settings.value("Gains").toString());
        if (!p.name.isEmpty() && p.gains.size() == m_bandCount) out << p;
    }
    m_settings.endArray();
    return out;
}

void EQ::saveCustomPreset(const QString& name, const QVector<float>& gains, float preamp) {
    QVector<Preset> presets = customPresets();
    bool replaced = false;
    for (Preset& p : presets)
        if (p.name == name) { p.gains = gains; p.preamp = preamp; replaced = true; break; }
    if (!replaced) presets << Preset{ name, preamp, gains };

    m_settings.remove(customArrayKey()); // clear stale entries before rewriting the array
    m_settings.beginWriteArray(customArrayKey());
    for (int i = 0; i < presets.size(); ++i) {
        m_settings.setArrayIndex(i);
        m_settings.setValue("Name", presets[i].name);
        m_settings.setValue("Preamp", presets[i].preamp);
        m_settings.setValue("Gains", gainsToCsv(presets[i].gains));
    }
    m_settings.endArray();
    m_settings.sync();
}

void EQ::selectCustomByName(const QString& name) {
    for (int i = 0; i < m_presetCombo->count(); ++i) {
        const QVariantMap d = m_presetCombo->itemData(i).toMap();
        if (d.value("role").toString() == "custom" && d.value("id").toString() == name) {
            m_presetCombo->setCurrentIndex(i);
            return;
        }
    }
}

bool EQ::selectPresetByGains(const QVector<float>& gains, float preampDb) {
    auto gainsMatch = [](const QVector<float>& a, const QVector<float>& b) {
        if (a.size() != b.size()) return false;
        for (int i = 0; i < a.size(); ++i)
            if (qRound(a[i]) != qRound(b[i])) return false;
        return true;
    };
    const int preampInt = qRound(preampDb);

    const QVector<Preset> stock = stockPresets();
    for (int i = 0; i < stock.size(); ++i) {
        if (qRound(stock[i].preamp) != preampInt || !gainsMatch(gains, stock[i].gains)) continue;
        for (int c = 0; c < m_presetCombo->count(); ++c) {
            const QVariantMap d = m_presetCombo->itemData(c).toMap();
            if (d.value("role").toString() == "stock" && d.value("id").toInt() == i) {
                m_presetCombo->setCurrentIndex(c);
                return true;
            }
        }
    }
    for (const Preset& p : customPresets()) {
        if (qRound(p.preamp) == preampInt && gainsMatch(gains, p.gains)) {
            selectCustomByName(p.name);
            return true;
        }
    }
    return false;
}

// --- Stock presets ----------------------------------------------------------
// Curated 10-band tables (dB). VLC's own presets are generic with no audio/video
// split, so we define both sets here. Names/curves are a first pass, easy to tune.
QVector<EQ::Preset> EQ::stockPresets() const {
    // Guard: the tables are 10-band; if VLC ever reports a different count, offer
    // only a correctly-sized Flat so nothing is mis-shapen.
    if (m_bandCount != 10)
        return { Preset{ "Flat", 0.0f, QVector<float>(m_bandCount, 0.0f) } };

    auto P = [](const char* name, float pre, std::initializer_list<float> g) {
        return Preset{ QString::fromLatin1(name), pre, QVector<float>(g) };
    };

    if (m_type == EqContentType::Audio) {
        // Bands: 31Hz 62Hz 125Hz 250Hz 500Hz 1kHz 2kHz 4kHz 8kHz 16kHz
        return {
            P("Flat",       0, {  0,  0,  0,  0,  0,  0,  0,  0,  0,  0 }),
            // Smiley face: sub punch + upper-mid bite, deep mud scoop at 250-500Hz
            P("Rock",       0, {  6,  5,  2, -4, -5, -2,  2,  6,  7,  5 }),
            // Clean and sparkly: cut 250Hz mud, push 2-4kHz presence and air
            P("Pop",        0, {  2,  2,  0, -4, -2,  1,  4,  6,  5,  4 }),
            // Warm double-bass body, smooth recessed presence, open high air
            P("Jazz",       0, {  4,  4,  3,  2, -1, -3, -1,  1,  3,  4 }),
            // Concert hall: cut sub, deeply recessed mids, heavy string/brass shimmer
            P("Classical",  4, { -2, -2, -1, -3, -5, -5, -2,  1,  6,  8 }),
            // Hard 808 sub with preamp pullback so it doesn't clip
            P("Hip-Hop",   -4, {  9,  8,  5, -1, -2,  1,  3,  2, -1, -2 }),
            // Deep sub, wide mid scoop, synth sparkle — the hollow EDM shape
            P("Electronic", -3, {  8,  6,  0, -5, -6, -3,  0,  4,  7,  8 }),
            // Cut sub rumble and 250Hz boxiness, push 1-4kHz presence window hard
            P("Vocal",      4, { -6, -5, -2, -5,  0,  4,  7,  7,  4, -1 }),
            // Clean low shelf with preamp compensation so it doesn't clip
            P("Bass Boost", -4, {  9,  8,  6,  3,  1,  0,  0,  0,  0,  0 }),
            // Boosted low-mids, heavy high roll-off — vinyl and tape warmth
            P("Lo-Fi",      3, { -3,  2,  7,  8,  5,  1, -3, -7,-11,-14 }),
        };
    }
    // Video — Bands: 31Hz 62Hz 125Hz 250Hz 500Hz 1kHz 2kHz 4kHz 8kHz 16kHz
    return {
        P("Flat",         0, {  0,  0,  0,  0,  0,  0,  0,  0,  0,  0 }),
        // Bass impact + forward dialogue at 1-3kHz + subtle air
        P("Movie",        0, {  3,  3,  0, -2,  1,  4,  5,  3,  2,  3 }),
        // Cut sub rumble, heavily push speech range — voice clarity without waking anyone
        P("Night Mode",   3, { -7, -6, -2,  0,  3,  7,  7,  4,  0, -2 }),
        // Maximum LFE impact: huge sub, punchy score, dramatic highs
        P("Action",      -4, {  9,  7,  3, -1,  0,  2,  4,  5,  6,  5 }),
        // Hard bass cut, extreme presence push — maximum intelligibility for dialogue
        P("Dialog",       5, { -9, -8, -4, -2,  2,  6,  9,  7,  2, -4 }),
        // Near-flat with a minimal narration nudge — least colored after Flat
        P("Documentary",  0, {  1,  1,  0, -1,  0,  2,  3,  2,  1,  1 }),
        // Pop-tuned for music content: punchy bass, cut mud, bright presence
        P("Music Video",  0, {  4,  3,  0, -4, -2,  1,  4,  6,  5,  4 }),
        // Heavy sub and dense low-mids, muffled highs — dark and oppressive
        P("Horror",      -3, {  8,  7,  6,  4,  1, -2, -5, -7, -6, -4 }),
        // Cut low-mid mud, very forward 2-4kHz voice acting, OST sparkle
        P("Anime",        1, {  0, -1, -2, -4, -1,  2,  5,  7,  6,  5 }),
        // Fletcher-Munson curve: bass + treble lift, scooped mids, for low-volume listening
        P("Loudness",    -2, {  7,  5,  2, -1, -3, -2,  0,  2,  5,  8 }),
    };
}
