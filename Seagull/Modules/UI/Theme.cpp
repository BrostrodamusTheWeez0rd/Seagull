#include "Theme.h"
#include <QApplication>
#include <QPalette>
#include <QStyleFactory>

namespace {
    QString g_current = "Seagull";
}

Theme::Colors Theme::colorsFor(const QString& name) {
    Colors c;
    if (name == "Light") {
        c.window     = QColor("#f4f5f7");
        c.base       = QColor("#ffffff");
        c.alt        = QColor("#eceef1");
        c.text       = QColor("#1c1f24");
        c.subtext    = QColor("#6b7280");
        c.accent     = QColor("#2f7fd1");
        c.accentText = QColor("#ffffff");
        c.border     = QColor("#d4d8de");
        c.overlayFg  = QColor("#1c1f24"); // inverted: dark chrome on the light pill
    }
    else if (name == "Dark") {
        // The app's original look: neutral dark greys, light text.
        c.window     = QColor("#1f1f1f");
        c.base       = QColor("#2a2a2a");
        c.alt        = QColor("#262626");
        c.text       = QColor("#e6e6e6");
        c.subtext    = QColor("#9a9a9a");
        c.accent     = QColor("#5b9bd5");
        c.accentText = QColor("#ffffff");
        c.border     = QColor("#3a3a3a");
        c.overlayFg  = QColor("#ffffff"); // the original white chrome over video
    }
    else {
        // Seagull (default): dark coastal slate with a sky-blue accent.
        c.window     = QColor("#11151c");
        c.base       = QColor("#1a2030");
        c.alt        = QColor("#161b27");
        c.text       = QColor("#e8eef4");
        c.subtext    = QColor("#8a98aa");
        c.accent     = QColor("#4ea8de");
        c.accentText = QColor("#0b0f14");
        c.border     = QColor("#27303f");
        c.overlayFg  = QColor("#4ea8de"); // Seagull's signature: blue outlines + highlights
    }
    return c;
}

static QPalette buildPalette(const Theme::Colors& c) {
    QPalette p;
    p.setColor(QPalette::Window,          c.window);
    p.setColor(QPalette::WindowText,      c.text);
    p.setColor(QPalette::Base,            c.base);
    p.setColor(QPalette::AlternateBase,   c.alt);
    p.setColor(QPalette::Text,            c.text);
    p.setColor(QPalette::Button,          c.window);
    p.setColor(QPalette::ButtonText,      c.text);
    // BrightText carries the overlay foreground (white on dark themes, dark on
    // light) so the player controls can read their icon tint straight from the
    // palette — it isn't used by the standard widgets here.
    p.setColor(QPalette::BrightText,      c.overlayFg);
    p.setColor(QPalette::ToolTipBase,     c.base);
    p.setColor(QPalette::ToolTipText,     c.text);
    p.setColor(QPalette::PlaceholderText, c.subtext);
    p.setColor(QPalette::Highlight,       c.accent);
    p.setColor(QPalette::HighlightedText, c.accentText);
    p.setColor(QPalette::Link,            c.accent);
    p.setColor(QPalette::Mid,             c.border);
    p.setColor(QPalette::Dark,            c.alt);

    // Greyed-out controls (e.g. the disabled Download/Stream buttons).
    p.setColor(QPalette::Disabled, QPalette::Text,       c.subtext);
    p.setColor(QPalette::Disabled, QPalette::ButtonText, c.subtext);
    p.setColor(QPalette::Disabled, QPalette::WindowText, c.subtext);
    return p;
}

void Theme::apply(const QString& name) {
    g_current = name;
    Colors c = colorsFor(name);

    auto* app = qobject_cast<QApplication*>(QApplication::instance());
    if (!app) return;

    // Fusion ignores the OS theme, so our palette fully controls the look.
    app->setStyle(QStyleFactory::create("Fusion"));
    app->setPalette(buildPalette(c));

    // A small global sheet for the few things the palette doesn't cover well,
    // plus our object-name-targeted Queue widgets. Re-applied on every switch.
    const QString ss = QString(
        "QToolTip { background-color:%1; color:%2; border:1px solid %3; }"
        "QProgressBar { border:1px solid %3; border-radius:4px; text-align:center; background:%4; }"
        "QProgressBar::chunk { background-color:%5; border-radius:4px; }"
        "QLabel#metaUploader, QLabel#metaStats { color:%6; }"
        "QTextEdit#logConsole { background-color:%4; color:%6; }"
    ).arg(c.base.name(), c.text.name(), c.border.name(), c.alt.name(), c.accent.name(), c.subtext.name());

    // The player overlays (banner + controls bar) sit over video as translucent
    // pills. They're styled here, by object name, so they follow the theme like
    // everything else. Pill surfaces use the window colour at ~84% alpha so the
    // video shows through; outlines, the hover-highlight and the seeker/volume
    // fills + handles all use overlayFg (blue in Seagull, white in Dark, inverted
    // dark in Light) — the chrome is one per-theme colour, not the accent.
    auto rgba = [](const QColor& col, int a) {
        return QString("rgba(%1,%2,%3,%4)").arg(col.red()).arg(col.green()).arg(col.blue()).arg(a);
    };
    const QString pill      = rgba(c.window, 215);    // translucent pill surface
    const QString itemHover = rgba(c.overlayFg, 50);  // quality-item hover wash
    const QString line      = c.overlayFg.name();     // outlines, highlight, fills, handles
    const QString onLine    = c.window.name();        // glyph/text over a highlight fill (inverse)

    const QString overlay = QString(
        // --- Title bar (banner) ---
        "QFrame#pillFrame { background-color:%1; border-radius:20px; border:1px solid %6; }"
        "QLabel#playerTitleLabel { color:%2; font-family:'Segoe UI'; font-size:14px; font-weight:bold; background:transparent; border:none; }"
        // --- Controls bar pill ---
        "QFrame#PillFrame { background-color:%1; border-radius:25px; border:1px solid %6; }"
        "QLabel#playerTimeLabel { color:%2; font-family:'Segoe UI'; font-size:11px; background:transparent; }"
        // round icon buttons: overlayFg outline; hover fills overlayFg with the
        // inverse (window) glyph/text so it reads against the fill.
        "QPushButton#playerCtlButton { background-color:transparent; border:1px solid %6; border-radius:15px; min-width:30px; min-height:30px; font-size:16px; color:%2; }"
        "QPushButton#playerCtlButton:hover { background-color:%6; color:%3; border:1px solid %6; }"
        // position seeker: filled progress + handle use overlayFg (per-theme chrome)
        "QSlider#playerSeekSlider::groove:horizontal { border:none; height:6px; background:%4; border-radius:3px; }"
        "QSlider#playerSeekSlider::sub-page:horizontal { background:%6; border-radius:3px; }"
        "QSlider#playerSeekSlider::handle:horizontal { background:%6; width:12px; margin:-3px 0; border-radius:6px; border:none; }"
        // volume popup
        "QFrame#volumePopup { background:%1; border-radius:20px; border:1px solid %6; }"
        "QSlider#volumeSlider { background:transparent; border:none; }"
        "QSlider#volumeSlider::groove:vertical { border:none; background:%4; width:6px; border-radius:3px; }"
        "QSlider#volumeSlider::add-page:vertical { border:none; background:%6; border-radius:3px; }"
        "QSlider#volumeSlider::sub-page:vertical { border:none; background:%4; border-radius:3px; }"
        "QSlider#volumeSlider::handle:vertical { border:none; background:%6; height:10px; margin:0 -3px; border-radius:5px; }"
        // quality popup
        "QFrame#qualityPopup { background:%1; border-radius:20px; border:1px solid %6; }"
        "QPushButton#qualityItem { background:transparent; color:%2; border:none; font-size:11px; padding-left:8px; }"
        "QPushButton#qualityItem:hover { background:%5; }"
        "QPushButton#qualityItem[active=\"true\"] { font-weight:bold; color:%6; border-left:3px solid %6; padding-left:5px; }"
        // banner Info / Share round icon buttons (icon tinted to overlayFg in code;
        // hover is a wash so the icon stays visible)
        "QPushButton#bannerActionButton { background:transparent; border:1px solid %6; border-radius:11px; min-width:22px; min-height:22px; }"
        "QPushButton#bannerActionButton:hover { background:%5; }"
    ).arg(pill, c.text.name(), onLine, c.alt.name(), itemHover, line);

    app->setStyleSheet(ss + overlay);
}

QString Theme::currentName() { return g_current; }
