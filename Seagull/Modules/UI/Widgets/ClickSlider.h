#ifndef CLICKSLIDER_H
#define CLICKSLIDER_H

#include <QSlider>
#include <QStyle>
#include <QMouseEvent>

// A slider that jumps to the click position (and lets you keep dragging from
// there), instead of QSlider's default page-step on groove clicks. Works for the
// horizontal seeker, the vertical volume bar, and the EQ band sliders (passing
// `vertical` as upsideDown makes top = max on vertical sliders).
class ClickSlider : public QSlider {
public:
    using QSlider::QSlider;
protected:
    void mousePressEvent(QMouseEvent* e) override {
        if (e->button() == Qt::LeftButton && maximum() > minimum()) {
            const bool vertical = orientation() == Qt::Vertical;
            const int pos  = vertical ? e->pos().y() : e->pos().x();
            const int span = vertical ? height() : width();
            const int v = QStyle::sliderValueFromPosition(
                minimum(), maximum(), pos, span, vertical);
            setValue(v); // moves the handle under the cursor so the base class
            // then enters its normal drag, giving click-then-drag scrubbing.
        }
        QSlider::mousePressEvent(e);
    }
};

#endif // CLICKSLIDER_H
