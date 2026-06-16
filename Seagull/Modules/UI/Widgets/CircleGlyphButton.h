#pragma once
#include <QToolButton>

// A QToolButton whose symbol (the tab bar's "+" and close "×") is drawn as two
// centred strokes rather than a font glyph. Text glyphs carry asymmetric side
// bearings and line-box leading, so a "+" or "×" never lands dead-centre in a tight
// circle no matter the padding. Stroking through rect().center() has no font
// geometry to drift: each arm is mirrored about the centre, so the symbol is exactly
// half the diameter on both axes. The circular background/border/hover still come
// from the global stylesheet (by object name).
class CircleGlyphButton : public QToolButton {
    Q_OBJECT
public:
    enum class Shape { Plus, Cross };
    // Colour intent. Both symbols draw in the theme's dimmed-text colour so they
    // match; the only difference is hover:
    //   Dim       -> dimmed text in every state (the "+", whose hover fill is subtle).
    //   DimInvert -> dimmed text, flipping to the base colour on hover (the close "×",
    //                whose hover fill is the accent, so the glyph must invert to read).
    enum class Tone { Dim, DimInvert };

    explicit CircleGlyphButton(QWidget* parent = nullptr);
    void setSymbol(Shape shape, Tone tone);

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    Shape m_shape = Shape::Plus;
    Tone  m_tone  = Tone::Dim;
};
