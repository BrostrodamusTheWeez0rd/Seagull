#include "SpellCheckLineEdit.h"

#include <QTimer>
#include <QPainter>
#include <QPainterPath>
#include <QPaintEvent>
#include <QContextMenuEvent>
#include <QMenu>
#include <QAction>
#include <QFont>
#include <QFontMetrics>
#include <QStyle>
#include <QStyleOptionFrame>
#include <QtGlobal>

SpellCheckLineEdit::SpellCheckLineEdit(SgSpellCheck* spell, QWidget* parent)
    : QLineEdit(parent), m_spell(spell) {
    m_timer = new QTimer(this);
    m_timer->setSingleShot(true);
    m_timer->setInterval(250); // debounce: don't COM-check on every keystroke
    connect(m_timer, &QTimer::timeout, this, &SpellCheckLineEdit::recheck);
    connect(this, &QLineEdit::textChanged, this, [this] {
        if (m_spell && m_spell->isAvailable())
            m_timer->start();
        else if (!m_errors.isEmpty()) {
            m_errors.clear();
            update();
        }
    });
}

void SpellCheckLineEdit::recheck() {
    m_errors = m_spell ? m_spell->check(text()) : QVector<SpellError>{};
    update();
}

// Pixel x of a character index. Derived from the live cursorRect(), which
// already folds in the frame, text margins, alignment, and the private
// horizontal scroll offset — so we read those off the cursor instead of trying
// to reconstruct QLineEdit's internal layout.
int SpellCheckLineEdit::xForChar(int charPos) const {
    const QString t = text();
    charPos = qBound(0, charPos, int(t.size()));
    const QFontMetrics fm(font());
    const int cp = qBound(0, cursorPosition(), int(t.size()));
    const int originX = cursorRect().x() - fm.horizontalAdvance(t.left(cp));
    return originX + fm.horizontalAdvance(t.left(charPos));
}

int SpellCheckLineEdit::errorIndexAt(int charPos) const {
    for (int i = 0; i < m_errors.size(); ++i) {
        const SpellError& e = m_errors[i];
        if (charPos >= e.start && charPos <= e.start + e.length)
            return i;
    }
    return -1;
}

void SpellCheckLineEdit::paintEvent(QPaintEvent* event) {
    QLineEdit::paintEvent(event); // text, selection, cursor, clear button
    if (m_errors.isEmpty()) return;

    const QString t = text();
    const QFontMetrics fm(font());

    // Clip to the text contents area so squiggles never paint over the frame or
    // a side widget (e.g. the clear button).
    QStyleOptionFrame opt;
    initStyleOption(&opt);
    const QRect content = style()->subElementRect(QStyle::SE_LineEditContents, &opt, this);

    int baseY = content.top() + (content.height() - fm.height()) / 2 + fm.ascent() + 1;
    baseY = qMin(baseY, content.bottom() - 1);

    QPainter p(this);
    p.setClipRect(content);
    p.setRenderHint(QPainter::Antialiasing, true);
    QPen pen(m_underlineColor);
    pen.setWidthF(1.1);
    p.setPen(pen);

    const qreal amp = 1.2;
    const qreal halfPeriod = 2.0;
    for (const SpellError& e : m_errors) {
        if (e.start < 0 || e.start + e.length > t.size()) continue;
        const int x0 = xForChar(e.start);
        const int x1 = xForChar(e.start + e.length);
        if (x1 <= x0) continue;

        QPainterPath path;
        path.moveTo(x0, baseY);
        bool up = true;
        for (qreal x = x0; x < x1; x += halfPeriod) {
            const qreal nx = qMin(x + halfPeriod, qreal(x1));
            path.lineTo(nx, baseY + (up ? -amp : amp));
            up = !up;
        }
        p.drawPath(path);
    }
}

void SpellCheckLineEdit::contextMenuEvent(QContextMenuEvent* event) {
    QMenu* menu = createStandardContextMenu();

    if (m_spell && m_spell->isAvailable()) {
        const int pos = cursorPositionAt(event->pos());
        const int idx = errorIndexAt(pos);
        if (idx >= 0) {
            const SpellError e = m_errors[idx];
            const QString word = text().mid(e.start, e.length);
            const QStringList suggestions = m_spell->suggest(word);

            QAction* firstStd = menu->actions().isEmpty() ? nullptr : menu->actions().first();
            QList<QAction*> spellActions;

            if (suggestions.isEmpty()) {
                QAction* none = new QAction(tr("No suggestions"), menu);
                none->setEnabled(false);
                spellActions << none;
            } else {
                for (const QString& s : suggestions) {
                    QAction* a = new QAction(s, menu);
                    QFont f = a->font();
                    f.setBold(true);
                    a->setFont(f);
                    connect(a, &QAction::triggered, this, [this, e, s] {
                        setSelection(e.start, e.length);
                        insert(s); // replaces the selection, undo-friendly
                    });
                    spellActions << a;
                }
            }

            QAction* sep1 = new QAction(menu);
            sep1->setSeparator(true);
            spellActions << sep1;

            QAction* add = new QAction(tr("Add to Dictionary"), menu);
            connect(add, &QAction::triggered, this, [this, word] {
                if (m_spell) m_spell->addToDictionary(word);
                recheck();
            });
            QAction* ign = new QAction(tr("Ignore"), menu);
            connect(ign, &QAction::triggered, this, [this, word] {
                if (m_spell) m_spell->ignore(word);
                recheck();
            });
            spellActions << add << ign;

            QAction* sep2 = new QAction(menu);
            sep2->setSeparator(true);
            spellActions << sep2;

            menu->insertActions(firstStd, spellActions);
        }
    }

    menu->exec(event->globalPos());
    delete menu;
}
