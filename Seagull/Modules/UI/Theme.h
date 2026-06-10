#ifndef THEME_H
#define THEME_H

#include <QString>
#include <QStringList>
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
        QColor overlayFg;   // player-overlay outline + hover-highlight: white on dark
                            // themes, inverted (dark) on light — distinct from accent
    };

    Colors      colorsFor(const QString& name);   // any name from names(); unknown -> Seagull
    QStringList names();                          // every selectable theme, in menu order
    void        apply(const QString& name);       // sets Fusion style + palette + global QSS on qApp
    QString     currentName();

}

#endif
