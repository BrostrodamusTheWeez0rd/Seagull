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
    else if (name == "Coastal Dusk") {
        // Twilight over the water: deep indigo with a warm coral accent.
        c.window     = QColor("#1a1526");
        c.base       = QColor("#241d34");
        c.alt        = QColor("#1f1830");
        c.text       = QColor("#ece6f2");
        c.subtext    = QColor("#9a8fb0");
        c.accent     = QColor("#ff7e6b");
        c.accentText = QColor("#1a1526");
        c.border     = QColor("#352a48");
        c.overlayFg  = QColor("#ff7e6b");
    }
    else if (name == "Foggy Shore") {
        // A soft light theme: misty grey-green with a muted teal accent.
        c.window     = QColor("#eef1f0");
        c.base       = QColor("#ffffff");
        c.alt        = QColor("#e2e8e6");
        c.text       = QColor("#243230");
        c.subtext    = QColor("#6d7d79");
        c.accent     = QColor("#3a8f86");
        c.accentText = QColor("#ffffff");
        c.border     = QColor("#cdd6d3");
        c.overlayFg  = QColor("#243230"); // inverted: dark chrome on the light pill
    }
    else if (name == "Storm Petrel") {
        // Near-black charcoal with an electric stormy-cyan accent.
        c.window     = QColor("#0e1113");
        c.base       = QColor("#181c1f");
        c.alt        = QColor("#131719");
        c.text       = QColor("#dfe6ea");
        c.subtext    = QColor("#8794a0");
        c.accent     = QColor("#36c5d9");
        c.accentText = QColor("#0e1113");
        c.border     = QColor("#232a2f");
        c.overlayFg  = QColor("#36c5d9");
    }
    else if (name == "Golden Beach") {
        // Warm sand light theme with an amber sunset accent.
        c.window     = QColor("#faf4e8");
        c.base       = QColor("#fffdf8");
        c.alt        = QColor("#f1e7d4");
        c.text       = QColor("#3a2f1c");
        c.subtext    = QColor("#8a7857");
        c.accent     = QColor("#e0892f");
        c.accentText = QColor("#ffffff");
        c.border     = QColor("#e2d4ba");
        c.overlayFg  = QColor("#3a2f1c"); // inverted: dark chrome on the light pill
    }
    else if (name == "Deep Tide") {
        // Dark ocean teal with a bright aqua accent.
        c.window     = QColor("#0c1a1c");
        c.base       = QColor("#11282b");
        c.alt        = QColor("#0f2225");
        c.text       = QColor("#def0f0");
        c.subtext    = QColor("#7fa3a4");
        c.accent     = QColor("#2fd6c3");
        c.accentText = QColor("#07211f");
        c.border     = QColor("#1b3a3d");
        c.overlayFg  = QColor("#2fd6c3");
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

QStringList Theme::names() {
    // Order shown in the Settings theme menu. The three originals first, then the
    // seagull/coastal-inspired set.
    return {
        "Seagull", "Dark", "Light",
        "Coastal Dusk", "Foggy Shore", "Storm Petrel", "Golden Beach", "Deep Tide"
    };
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
        // Tabs: outlined tiles with rounded tops. Unselected sit in the alt
        // colour with dimmed text; the selected tab rises to the base colour and
        // overlaps the pane's top border (-1px) so it reads as one surface.
        "QTabWidget::pane { border:1px solid %3; border-top-left-radius:0; top:-1px; }"
        // The right padding reserves the blank strip that MainWindow's manually
        // placed close button (positionCloseButtons) overlays on each tab — it
        // keeps the label clear of the x. Width here must track that button's
        // size + insets (14px button, ~6px edge gap).
        "QTabBar::tab { background:%4; color:%6; border:1px solid %3; border-top-left-radius:8px; border-top-right-radius:8px; padding:5px 22px 5px 12px; margin-right:2px; }"
        "QTabBar::tab:selected { background:%1; color:%2; border-bottom-color:%1; margin-bottom:-1px; }"
        "QTabBar::tab:hover:!selected { color:%2; }"
        // Tab bar: small round per-tab close button (a bordered themed chip that
        // fills with the accent on hover) and the floating "+" that trails the last
        // tab (radii match the fixed sizes set in MainWindow).
        "QToolButton#tabCloseButton { background:%4; color:%6; border:1px solid %3; border-radius:7px; font-size:10px; font-weight:bold; padding:0; }"
        "QToolButton#tabCloseButton:hover { background:%5; color:%1; border:1px solid %5; }"
        "QToolButton#tabPlusButton, QToolButton#tabShareButton { background:transparent; color:%2; border:1px solid %3; border-radius:9px; font-size:13px; font-weight:bold; padding:0; }"
        "QToolButton#tabPlusButton:hover, QToolButton#tabShareButton:hover { background:%4; }"
        // Hide the popup arrow; the glyph itself is centred by CircleGlyphButton's
        // own paint (by ink box), so no padding tricks are needed here.
        "QToolButton#tabPlusButton::menu-indicator { image:none; width:0; }"
    ).arg(c.base.name(), c.text.name(), c.border.name(), c.alt.name(), c.accent.name(), c.subtext.name());

    // Search tab: the result cards and the status/"loading more" pill. Cards are
    // rounded, themed tiles whose thumbnail is itself a rounded pill (the pixmap is
    // pre-rounded in code; the rule here just sets the placeholder/background look).
    // %1 base  %2 alt  %3 border  %4 text  %5 accent  %6 accentText  %7 subtext
    const QString cards = QString(
        "QWidget#videoCard { background-color:%1; border:1px solid %3; border-radius:14px; }"
        "QWidget#videoCard:hover { border:1px solid %5; }"
        "QLabel#videoCardThumb { background-color:%2; color:%7; border-radius:10px; }"
        "QLabel#videoCardTitle { color:%4; background:transparent; }"
        "QPushButton#videoCardButton { background-color:%2; color:%4; border:1px solid %3; border-radius:10px; padding:5px 4px; }"
        "QPushButton#videoCardButton:hover { background-color:%5; color:%6; border:1px solid %5; }"
        "QFrame#searchStatusPill { background-color:%2; border:1px solid %3; border-radius:14px; }"
        "QLabel#searchStatus { background:transparent; color:%4; }"
        "QFrame#searchSeparator { background-color:%3; border:none; min-height:1px; max-height:1px; }"
    ).arg(c.base.name(), c.alt.name(), c.border.name(), c.text.name(),
          c.accent.name(), c.accentText.name(), c.subtext.name());

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
        // Library tab: the floating translucent type-switcher pill over the card
        // grid (same pill surface/outline language as the player overlays).
        "QFrame#libraryTypePill { background-color:%1; border:1px solid %6; border-radius:17px; }"
        "QPushButton#libraryTypeButton { background:transparent; color:%2; border:none; border-radius:12px; padding:5px 16px; font-weight:bold; }"
        "QPushButton#libraryTypeButton:hover { background-color:%5; }"
        "QPushButton#libraryTypeButton:checked { background-color:%6; color:%3; }"
        "QLineEdit#librarySearchBar { background-color:%4; color:%2; border:1px solid %6; border-radius:11px; padding:2px 8px; }"
        // Floating round magnifier at the top-right (same pill surface/outline as
        // the type switcher; the glyph itself is tinted in code).
        "QPushButton#librarySearchButton { background-color:%1; border:1px solid %6; border-radius:17px; }"
        "QPushButton#librarySearchButton:hover { background-color:%5; }"
        "QLabel#libraryEmptyLabel { color:%2; background:transparent; }"
        // Search tab: small filter pill (All / Videos / Shorts) — same pill idiom.
        "QFrame#searchFilterPill { background-color:%1; border:1px solid %6; border-radius:14px; }"
        "QPushButton#searchFilterButton { background:transparent; color:%2; border:none; border-radius:10px; padding:3px 12px; font-weight:bold; }"
        "QPushButton#searchFilterButton:hover { background-color:%5; }"
        "QPushButton#searchFilterButton:checked { background-color:%6; color:%3; }"
        // Player: circular splitter-toggle chevron at the bottom of the video
        // (YouTube-style show/hide for the tabs pane) — same idiom as the
        // round control buttons (outline circle, hover fills with the inverse
        // glyph). %7 is the window colour at alpha 2: visually transparent
        // like the control buttons, but a true 0 alpha would make this
        // standalone layered window click-through to the video.
        // Glyph uses overlayFg (%6) like the control buttons' tinted icons —
        // plain text colour (%2) reads as a mismatched white in dark themes.
        "QPushButton#splitterToggleButton { background-color:%7; border:1px solid %6; border-radius:15px; color:%6; font-size:16px; }"
        "QPushButton#splitterToggleButton:hover { background-color:%6; color:%3; border:1px solid %6; }"
        // Small round prev/next-visualizer triangles flanking the visualizer button
        // (same circle idiom as the control buttons, just smaller).
        "QPushButton#vizNavButton { background-color:%7; border:1px solid %6; border-radius:11px; color:%6; font-size:10px; }"
        "QPushButton#vizNavButton:hover { background-color:%6; color:%3; border:1px solid %6; }"
        // Banner close (X): flat, turns red on hover to signal the teardown.
        "QPushButton#bannerCloseButton { background:transparent; border:none; color:%2; font-size:13px; font-weight:bold; }"
        "QPushButton#bannerCloseButton:hover { color:#e25555; }"
        // Photo viewer: large circular prev/next arrows glued to the left/right
        // edges of the image — same overlay idiom as the chevron, just bigger.
        "QPushButton#photoNavButton { background-color:%7; border:1px solid %6; border-radius:22px; color:%6; font-size:24px; font-weight:bold; }"
        "QPushButton#photoNavButton:hover { background-color:%6; color:%3; border:1px solid %6; }"
        // EQ tab: Video/Audio pill (same idiom as the Library type pill), preset
        // dropdown, band captions, and the vertical band + preamp sliders.
        "QFrame#eqTypePill { background-color:%1; border:1px solid %6; border-radius:17px; }"
        "QPushButton#eqTypeButton { background:transparent; color:%2; border:none; border-radius:12px; padding:5px 18px; font-weight:bold; }"
        "QPushButton#eqTypeButton:hover { background-color:%5; }"
        "QPushButton#eqTypeButton:checked { background-color:%6; color:%3; }"
        "QComboBox#eqPresetCombo { background-color:%4; color:%2; border:1px solid %6; border-radius:11px; padding:3px 10px; }"
        "QLabel#eqBandLabel { color:%2; background:transparent; font-size:10px; }"
        // Bipolar (center-detent) sliders: a plain groove + overlayFg handle, no
        // fill direction (the handle position conveys the ± gain).
        "QSlider#eqSlider, QSlider#eqPreampSlider { background:transparent; border:none; }"
        "QSlider#eqSlider::groove:vertical, QSlider#eqPreampSlider::groove:vertical { border:none; background:%4; width:6px; border-radius:3px; }"
        "QSlider#eqSlider::add-page:vertical, QSlider#eqPreampSlider::add-page:vertical { border:none; background:%4; border-radius:3px; }"
        "QSlider#eqSlider::sub-page:vertical, QSlider#eqPreampSlider::sub-page:vertical { border:none; background:%4; border-radius:3px; }"
        "QSlider#eqSlider::handle:vertical, QSlider#eqPreampSlider::handle:vertical { border:none; background:%6; height:12px; margin:0 -4px; border-radius:6px; }"
    ).arg(pill, c.text.name(), onLine, c.alt.name(), itemHover, line, rgba(c.window, 2));

    app->setStyleSheet(ss + cards + overlay);
}

QString Theme::currentName() { return g_current; }
