#include "MarqueeLabel.h"

#include <QPainter>
#include <QFontMetrics>
#include <QTextLayout>
#include <QTextOption>

namespace {
constexpr int kTickMs     = 25;   // 1 px per tick ≈ 40 px/s — readable, not frantic
constexpr int kStartDelay = 400;  // hover must settle before anything moves
constexpr int kEndHold    = 1100; // pause with the tail visible so it can be read
constexpr int kRestartGap = 600;  // pause back at the start before the next pass
}

MarqueeLabel::MarqueeLabel(const QString& text, QWidget* parent) : QLabel(text, parent) {
    m_tick.setInterval(kTickMs);
    m_pause.setSingleShot(true);
    connect(&m_tick,  &QTimer::timeout, this, &MarqueeLabel::onTick);
    connect(&m_pause, &QTimer::timeout, this, &MarqueeLabel::onPause);
}

void MarqueeLabel::setMarqueeOn(bool on) {
    m_externalOn = on;
    updateArmed();
}

void MarqueeLabel::enterEvent(QEnterEvent* event) {
    m_selfHover = true;
    updateArmed();
    QLabel::enterEvent(event);
}

void MarqueeLabel::leaveEvent(QEvent* event) {
    m_selfHover = false;
    updateArmed();
    QLabel::leaveEvent(event);
}

void MarqueeLabel::updateArmed() {
    if (m_selfHover || m_externalOn) {
        if (m_phase != Phase::Idle || !isTruncated()) return; // already running / nothing to reveal
        m_phase = Phase::Wait;
        m_pause.start(kStartDelay);
    } else {
        stopMarquee();
    }
}

QSize MarqueeLabel::minimumSizeHint() const {
    const QSize s = QLabel::minimumSizeHint();
    return QSize(qMin(s.width(), fontMetrics().height() * 2), s.height());
}

void MarqueeLabel::onPause() {
    switch (m_phase) {
    case Phase::Wait:      // start delay elapsed — begin the pass
        m_offset = 0;
        m_scrolling = true;
        m_phase = Phase::Scroll;
        m_tick.start();
        update();
        break;
    case Phase::EndHold:   // tail has been readable — snap home
        m_offset = 0;
        m_phase = Phase::GapHold;
        m_pause.start(kRestartGap);
        update();
        break;
    case Phase::GapHold:   // and go again
        m_phase = Phase::Scroll;
        m_tick.start();
        break;
    default:
        break;
    }
}

void MarqueeLabel::onTick() {
    const int range = scrollRange();
    if (range <= 0) { stopMarquee(); return; } // text changed under us and now fits
    if (m_offset >= range) {
        m_tick.stop();
        m_phase = Phase::EndHold;
        m_pause.start(kEndHold);
        return;
    }
    ++m_offset;
    update();
}

void MarqueeLabel::stopMarquee() {
    m_tick.stop();
    m_pause.stop();
    m_phase = Phase::Idle;
    m_scrolling = false;
    m_offset = 0;
    update();
}

int MarqueeLabel::scrollRange() const {
    return fontMetrics().horizontalAdvance(text()) - contentsRect().width();
}

QStringList MarqueeLabel::layoutLines(int width, int maxLines, bool* truncated) const {
    *truncated = false;

    QTextLayout tl(text(), font());
    QTextOption opt;
    opt.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    tl.setTextOption(opt);
    tl.beginLayout();
    QList<QPair<int, int>> ranges; // (start, length) per line
    for (;;) {
        QTextLine line = tl.createLine();
        if (!line.isValid()) break;
        line.setLineWidth(width);
        ranges.append(qMakePair(line.textStart(), line.textLength()));
        if (ranges.size() == maxLines) break; // remainder is elided below
    }
    tl.endLayout();

    QStringList out;
    for (int i = 0; i < ranges.size(); ++i) {
        if (i < ranges.size() - 1) {
            out.append(text().mid(ranges[i].first, ranges[i].second).trimmed());
            continue;
        }
        // Last visible line carries everything left over, elided to fit.
        const QString rest = text().mid(ranges[i].first).trimmed();
        const QString elided = fontMetrics().elidedText(rest, Qt::ElideRight, width);
        *truncated = (elided != rest);
        out.append(elided);
    }
    return out;
}

bool MarqueeLabel::isTruncated() const {
    const QRect cr = contentsRect();
    if (text().isEmpty() || cr.isEmpty()) return false;
    const QFontMetrics fm = fontMetrics();
    const int lineCount = qMax(1, (cr.height() + 2) / fm.height());
    if (lineCount == 1) return fm.horizontalAdvance(text()) > cr.width();
    bool truncated = false;
    layoutLines(cr.width(), lineCount, &truncated);
    return truncated;
}

void MarqueeLabel::paintEvent(QPaintEvent* event) {
    const QRect cr = contentsRect();
    if (text().isEmpty() || cr.isEmpty()) { QLabel::paintEvent(event); return; }

    const QFontMetrics fm = fontMetrics();

    if (m_scrolling) {
        // The marquee: one vertically-centred line of the FULL text, shifted
        // left by the current offset and clipped to the label.
        QPainter p(this);
        p.setPen(palette().color(QPalette::WindowText));
        p.setClipRect(cr);
        const int shift = qMin(m_offset, qMax(0, scrollRange()));
        const int y = cr.y() + (cr.height() - fm.height()) / 2 + fm.ascent();
        p.drawText(QPoint(cr.x() - shift, y), text());
        return;
    }

    const int lineCount = qMax(1, (cr.height() + 2) / fm.height());
    bool truncated = false;
    const QStringList lines = layoutLines(cr.width(), lineCount, &truncated);

    // Short single-line text: QLabel's native paint is pixel-identical to what
    // it always did (alignment, QSS, disabled state), so defer to it.
    if (!truncated && lines.size() == 1) { QLabel::paintEvent(event); return; }

    // Wrapped and/or elided: draw the block ourselves, vertically centred,
    // honouring the label's horizontal alignment per line.
    QPainter p(this);
    p.setPen(palette().color(QPalette::WindowText));
    int y = cr.y() + qMax(0, (cr.height() - int(lines.size()) * fm.height()) / 2) + fm.ascent();
    for (const QString& line : lines) {
        int x = cr.x();
        if (alignment() & Qt::AlignHCenter)
            x += qMax(0, (cr.width() - fm.horizontalAdvance(line)) / 2);
        else if (alignment() & Qt::AlignRight)
            x += qMax(0, cr.width() - fm.horizontalAdvance(line));
        p.drawText(QPoint(x, y), line);
        y += fm.height();
    }
}
