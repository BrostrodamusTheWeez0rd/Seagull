#include "FlowLayout.h"
#include <QWidget>

FlowLayout::FlowLayout(QWidget* parent, int margin, int hSpacing, int vSpacing)
    : QLayout(parent), m_hSpace(hSpacing), m_vSpace(vSpacing) {
    setContentsMargins(margin, margin, margin, margin);
}

FlowLayout::~FlowLayout() {
    QLayoutItem* item;
    while ((item = takeAt(0)))
        delete item;
}

void FlowLayout::addItem(QLayoutItem* item) { itemList.append(item); }

int FlowLayout::horizontalSpacing() const {
    return m_hSpace >= 0 ? m_hSpace : smartSpacing(QStyle::PM_LayoutHorizontalSpacing);
}
int FlowLayout::verticalSpacing() const {
    return m_vSpace >= 0 ? m_vSpace : smartSpacing(QStyle::PM_LayoutVerticalSpacing);
}

int FlowLayout::count() const { return itemList.size(); }
QLayoutItem* FlowLayout::itemAt(int index) const { return itemList.value(index); }

QLayoutItem* FlowLayout::takeAt(int index) {
    if (index >= 0 && index < itemList.size())
        return itemList.takeAt(index);
    return nullptr;
}

Qt::Orientations FlowLayout::expandingDirections() const { return {}; }
bool FlowLayout::hasHeightForWidth() const { return true; }

int FlowLayout::heightForWidth(int width) const {
    return doLayout(QRect(0, 0, width, 0), true);
}

void FlowLayout::setGeometry(const QRect& rect) {
    QLayout::setGeometry(rect);
    doLayout(rect, false);
}

QSize FlowLayout::sizeHint() const { return minimumSize(); }

QSize FlowLayout::minimumSize() const {
    QSize size;
    for (const QLayoutItem* item : itemList)
        size = size.expandedTo(item->minimumSize());
    const QMargins m = contentsMargins();
    size += QSize(m.left() + m.right(), m.top() + m.bottom());
    return size;
}

int FlowLayout::doLayout(const QRect& rect, bool testOnly) const {
    int left, top, right, bottom;
    getContentsMargins(&left, &top, &right, &bottom);
    const QRect effective = rect.adjusted(left, top, -right, -bottom);
    const int availW = effective.width();

    int spaceX = horizontalSpacing();
    if (spaceX < 0) spaceX = 8;
    int spaceY = verticalSpacing();
    if (spaceY < 0) spaceY = 8;

    // Lay out only visible items, so hidden ones (e.g. filtered-out cards) leave
    // no gap.
    QList<QLayoutItem*> items;
    items.reserve(itemList.size());
    for (QLayoutItem* it : itemList) {
        QWidget* w = it->widget();
        if (w && w->isHidden()) continue;
        items.append(it);
    }

    const int n = items.size();
    int y = effective.y();
    int lastBottom = effective.y();

    // Lay out a row at a time. A full (wrapped) row is justified — first card flush
    // left, last card flush right, leftover spread into the gaps — so the grid uses
    // the whole width. The final partial row is centered so it isn't stretched out.
    // Pure positioning; cards keep their fixed size (nothing re-renders).
    int i = 0;
    while (i < n) {
        int sumWidths = 0;  // item widths only
        int rowWidth = 0;   // widths + the minimum gaps between them
        int lineHeight = 0;
        int j = i;
        while (j < n) {
            const QSize hint = items[j]->sizeHint();
            const int add = (j == i) ? hint.width() : spaceX + hint.width();
            if (j > i && rowWidth + add > availW) break; // always keep at least one
            rowWidth += add;
            sumWidths += hint.width();
            lineHeight = qMax(lineHeight, hint.height());
            ++j;
        }

        const int count = j - i;
        const bool lastRow = (j == n);

        int x = effective.x();
        int gap = spaceX;
        if (!lastRow && count > 1) {
            // Justify: distribute the slack so the row spans the full width.
            gap = qMax(spaceX, (availW - sumWidths) / (count - 1));
        }
        else {
            // Final/partial row (or a lone card): center it.
            x += qMax(0, (availW - rowWidth) / 2);
        }

        for (int k = i; k < j; ++k) {
            const QSize hint = items[k]->sizeHint();
            if (!testOnly)
                items[k]->setGeometry(QRect(QPoint(x, y), hint));
            x += hint.width() + gap;
        }

        lastBottom = y + lineHeight;
        y = lastBottom + spaceY;
        i = j;
    }

    return lastBottom - rect.y() + bottom;
}

int FlowLayout::smartSpacing(QStyle::PixelMetric pm) const {
    QObject* parent = this->parent();
    if (!parent) return -1;
    if (parent->isWidgetType()) {
        QWidget* pw = static_cast<QWidget*>(parent);
        return pw->style()->pixelMetric(pm, nullptr, pw);
    }
    return static_cast<QLayout*>(parent)->spacing();
}
