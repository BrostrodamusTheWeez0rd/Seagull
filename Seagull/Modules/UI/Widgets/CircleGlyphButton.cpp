#include "CircleGlyphButton.h"
#include "../Theme.h"
#include <QPainter>
#include <QPen>
#include <QPointF>
#include <algorithm>

CircleGlyphButton::CircleGlyphButton(QWidget* parent) : QToolButton(parent) {}

void CircleGlyphButton::setSymbol(Shape shape, Tone tone) {
    m_shape = shape;
    m_tone = tone;
    update();
}

void CircleGlyphButton::paintEvent(QPaintEvent* event) {
    // Stylesheet draws the themed circle (border, fill, hover); the button has no
    // text/icon, so the base paints no symbol of its own to fight with.
    QToolButton::paintEvent(event);

    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    // Live theme colours so a theme switch repaints correctly (no stored state).
    // Both symbols share the dimmed-text colour; only the close "×" inverts to the
    // base colour on hover (its hover fill is the accent).
    const Theme::Colors c = Theme::colorsFor(Theme::currentName());
    const QColor fg = (m_tone == Tone::DimInvert && underMouse()) ? c.base : c.subtext;

    const qreal d = std::min(width(), height());
    QPen pen(fg, std::max(1.5, d * 0.11)); // stroke weight tracks the chip size
    pen.setCapStyle(Qt::RoundCap);
    p.setPen(pen);

    // Both arms are mirrored about the centre, so the symbol is exactly centred.
    const QPointF o = QRectF(rect()).center();
    if (m_shape == Shape::Plus) {
        const qreal a = d * 0.24; // half-length of each arm
        p.drawLine(QPointF(o.x() - a, o.y()), QPointF(o.x() + a, o.y()));
        p.drawLine(QPointF(o.x(), o.y() - a), QPointF(o.x(), o.y() + a));
    } else { // Cross
        const qreal a = d * 0.20; // a touch shorter so the diagonals don't read large
        p.drawLine(QPointF(o.x() - a, o.y() - a), QPointF(o.x() + a, o.y() + a));
        p.drawLine(QPointF(o.x() - a, o.y() + a), QPointF(o.x() + a, o.y() - a));
    }
}
