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
    p.setColor(QPalette::BrightText,      Qt::white);
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
    app->setStyleSheet(ss);
}

QString Theme::currentName() { return g_current; }
