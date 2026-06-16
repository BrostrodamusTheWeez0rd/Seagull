#include "EQ.h"
#include "Widgets/ClickSlider.h"
#include "../Backend/PlaybackEngine.h"  // band count / frequencies (static)

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
} // namespace

EQ::EQ(QWidget* parent)
    : QWidget(parent),
      m_settings(QCoreApplication::applicationDirPath() + "/config.ini", QSettings::IniFormat) {
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

    // --- 1. Video / Audio pill (same idiom as the Library type pill) ---
    auto* pill = new QFrame(this);
    pill->setObjectName("eqTypePill");
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
    root->addWidget(pill, 0, Qt::AlignHCenter);

    // --- 2. Preset dropdown ---
    {
        auto* row = new QHBoxLayout();
        row->addStretch();
        auto* lbl = new QLabel("Preset:", this);
        lbl->setObjectName("eqBandLabel");
        m_presetCombo = new QComboBox(this);
        m_presetCombo->setObjectName("eqPresetCombo");
        m_presetCombo->setMinimumWidth(220);
        connect(m_presetCombo, &QComboBox::activated, this, &EQ::onPresetActivated);
        row->addWidget(lbl);
        row->addWidget(m_presetCombo);
        row->addStretch();
        root->addLayout(row);
    }

    // --- 3. Graphic EQ: preamp column + band columns ---
    auto* eqRow = new QHBoxLayout();
    eqRow->setSpacing(10);

    auto makeColumn = [this, eqRow](const QString& caption, const QString& objName) -> ClickSlider* {
        auto* col = new QVBoxLayout();
        col->setSpacing(4);
        auto* s = new ClickSlider(Qt::Vertical, this);
        s->setObjectName(objName);
        s->setRange(-kGainRange, kGainRange);
        s->setValue(0);
        s->setMinimumHeight(170);
        connect(s, &QSlider::valueChanged, this, [this](int) { onSliderMoved(); });
        col->addWidget(s, 1, Qt::AlignHCenter);
        auto* cap = new QLabel(caption, this);
        cap->setObjectName("eqBandLabel");
        cap->setAlignment(Qt::AlignHCenter);
        col->addWidget(cap);
        eqRow->addLayout(col);
        return s;
    };

    m_preamp = makeColumn("Preamp", "eqPreampSlider");
    eqRow->addSpacing(10); // small gap between preamp and the bands

    for (int i = 0; i < m_bandCount; ++i)
        m_bands << makeColumn(formatFreq(PlaybackEngine::equalizerBandFrequency(i)), "eqSlider");

    root->addLayout(eqRow, 1);
    root->addStretch();
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
    add("Custom", "manual", {});                       // index 0 = live / unsaved state
    const QVector<Preset> stock = stockPresets();
    for (int i = 0; i < stock.size(); ++i) add(stock[i].name, "stock", i);
    const QVector<Preset> custom = customPresets();
    for (const Preset& p : custom) add(p.name, "custom", p.name);
    add("Add custom preset...", "add", {});
    m_presetCombo->setCurrentIndex(0);
    m_presetCombo->blockSignals(false);
}

void EQ::onPresetActivated(int index) {
    if (index < 0) return;
    const QVariantMap d = m_presetCombo->itemData(index).toMap();
    const QString role = d.value("role").toString();
    if (role == "manual") return; // the live/unsaved state; nothing to load

    if (role == "add") {
        const QVector<float> gains = currentGains();
        const float preamp = currentPreamp();
        bool ok = false;
        const QString name = QInputDialog::getText(this, "Save EQ Preset",
            "Name this preset:", QLineEdit::Normal, QString(), &ok).trimmed();
        if (ok && !name.isEmpty()) {
            saveCustomPreset(name, gains, preamp);
            populatePresets();
            selectCustomByName(name);
        } else {
            m_presetCombo->setCurrentIndex(0); // never leave the action item selected
        }
        return;
    }

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
    m_presetCombo->setCurrentIndex(0);      // reflect "Custom" (unsaved) state
    emitLive();                             // real-time apply
    m_persistTimer->start();                // debounced persist
}

void EQ::emitLive() {
    emit eqChanged(m_type, currentGains(), currentPreamp());
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
    m_presetCombo->setCurrentIndex(0); // show as "Custom" (we don't reverse-match presets)
}

void EQ::persistActive() {
    const QString n = ns();
    m_settings.setValue(n + "Enabled", true);
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
        return {
            P("Flat",         0, { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 }),
            P("Rock",         0, { 5, 3,-1,-2,-1, 1, 3, 4, 4, 4 }),
            P("Pop",          0, {-1, 2, 4, 4, 2, 0,-1,-1, 1, 2 }),
            P("Jazz",         0, { 3, 2, 1, 2,-1,-1, 0, 1, 2, 3 }),
            P("Classical",    0, { 4, 3, 2, 1,-1,-1, 0, 2, 3, 4 }),
            P("Dance",        0, { 6, 5, 2, 0, 0,-3,-4,-4, 1, 2 }),
            P("Bass Boost",   0, { 7, 6, 5, 3, 1, 0, 0, 0, 0, 0 }),
            P("Treble Boost", 0, { 0, 0, 0, 0, 0, 1, 3, 5, 6, 7 }),
            P("Vocal",        0, {-2,-1, 0, 2, 4, 4, 3, 1, 0,-1 }),
            P("Electronic",   0, { 5, 4, 1, 0,-2, 2, 1, 2, 4, 5 }),
        };
    }
    // Video
    return {
        P("Flat",          0, { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 }),
        P("Movie",         0, { 3, 2, 1, 0, 1, 2, 2, 1, 2, 3 }),
        P("Cinema",        0, { 4, 3, 1, 0, 0, 1, 2, 2, 3, 4 }),
        P("Night",         0, {-4,-3,-1, 1, 2, 2, 1, 0,-1,-2 }),
        P("Dialog Boost",  0, {-3,-2, 0, 2, 4, 4, 3, 1,-1,-2 }),
        P("Action",        0, { 6, 4, 1, 0,-1, 1, 2, 3, 4, 5 }),
        P("Music Video",   0, {-1, 2, 4, 4, 2, 0,-1,-1, 1, 2 }),
        P("Bright",        0, { 0, 0, 0, 0, 1, 2, 3, 4, 5, 5 }),
        P("Warm",          0, { 3, 3, 2, 1, 0,-1,-2,-2,-1, 0 }),
        P("Loudness",      0, { 6, 4, 1, 0,-1,-1, 0, 2, 4, 6 }),
    };
}
