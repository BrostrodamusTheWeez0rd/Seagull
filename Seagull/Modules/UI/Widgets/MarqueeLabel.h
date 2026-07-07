#ifndef MARQUEELABEL_H
#define MARQUEELABEL_H

#include <QLabel>
#include <QTimer>

// A QLabel that never overflows. Text that doesn't fit is elided with a
// trailing "…" — wrapping first when the label is tall enough for more than
// one line (the video cards give it two). While hovered (or armed by the
// owner via setMarqueeOn — the cards arm it from anywhere on the card), a
// truncated title marquees as a single vertically-centred line: scroll to the
// end, hold so the tail can be read, snap back, repeat. Text that fits never
// moves. Subclasses QLabel so the theme's QLabel#objectName style rules keep
// applying unchanged.
class MarqueeLabel : public QLabel {
    Q_OBJECT
public:
    explicit MarqueeLabel(const QString& text = {}, QWidget* parent = nullptr);

    // External hover source (e.g. VideoCard's enter/leave). Tracked separately
    // from the label's own hover, so the mouse sliding off the title but staying
    // on the card doesn't kill a card-armed marquee.
    void setMarqueeOn(bool on);

protected:
    void paintEvent(QPaintEvent* event) override;
    void enterEvent(QEnterEvent* event) override;
    void leaveEvent(QEvent* event) override;

    // QLabel's minimum is the full text width, which would stop layouts from
    // compressing us (the whole point is surviving compression by eliding).
    QSize minimumSizeHint() const override;

private slots:
    void onTick();
    void onPause();

private:
    // Wrap into at most maxLines lines of the given width, last line elided.
    // Sets *truncated when the full text didn't survive intact.
    QStringList layoutLines(int width, int maxLines, bool* truncated) const;

    bool isTruncated() const;
    int  scrollRange() const; // full single-line width minus visible width
    void updateArmed();       // start/stop from the two hover sources' current state
    void stopMarquee();

    enum class Phase { Idle, Wait, Scroll, EndHold, GapHold };
    Phase  m_phase = Phase::Idle;
    QTimer m_tick;   // scroll driver (1 px per tick)
    QTimer m_pause;  // single-shot: start delay / end-of-scroll hold / restart gap
    int    m_offset = 0;        // current scroll shift, px
    bool   m_scrolling = false; // painting the single-line marquee view
    bool   m_selfHover = false; // mouse over the label itself
    bool   m_externalOn = false; // armed by the owner (setMarqueeOn)
};

#endif // MARQUEELABEL_H
