#ifndef THEME_H
#define THEME_H

#include <QString>
#include <QColor>

// Central app theming. Each theme is a small set of colors; apply() turns those
// into a Fusion QPalette + a small global stylesheet on qApp, so every standard
// widget (buttons, inputs, tables, trees, tabs, menus) follows the theme without
// per-widget code. The player overlays (pills over video) stay dark on purpose.
namespace Theme {

    struct Colors {
        QColor window;      // app background
        QColor base;        // inputs, tables, console surfaces
        QColor alt;         // alternating rows / secondary surfaces
        QColor text;        // primary text
        QColor subtext;     // secondary / dimmed text
        QColor accent;      // selection / highlight / progress
        QColor accentText;  // text drawn on top of the accent
        QColor border;      // lines and borders
    };

    Colors  colorsFor(const QString& name);   // "Seagull" | "Dark" | "Light"
    void    apply(const QString& name);        // sets Fusion style + palette + global QSS on qApp
    QString currentName();

}

#endif
