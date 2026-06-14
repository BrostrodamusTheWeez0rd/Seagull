#ifndef SPELLCHECKLINEEDIT_H
#define SPELLCHECKLINEEDIT_H

#include <QLineEdit>
#include <QColor>
#include <QVector>
#include "../../Backend/SgSpellCheck.h"  // SpellError

class QTimer;

// A QLineEdit that draws red wavy underlines under misspelled words and offers
// suggestions in its right-click menu, via the Windows OS spell checker
// (SgSpellCheck).
//
// Single-line edits have no rich-text formatting, so the squiggles are painted
// by hand in paintEvent over the base-painted text. Works standalone (the File
// Explorer search box) and as the inner editor of an editable QComboBox via
// QComboBox::setLineEdit (the Search history combo) — the combo keeps its model,
// dropdown, and completer; only the text field is swapped.
class SpellCheckLineEdit : public QLineEdit {
    Q_OBJECT
public:
    explicit SpellCheckLineEdit(SgSpellCheck* spell, QWidget* parent = nullptr);

    void setUnderlineColor(const QColor& c) { m_underlineColor = c; update(); }

protected:
    void paintEvent(QPaintEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;

private:
    void recheck();                       // re-run the check (debounced via m_timer)
    int  errorIndexAt(int charPos) const; // which error range contains charPos, or -1
    int  xForChar(int charPos) const;     // pixel x of a character index in the text

    SgSpellCheck*       m_spell = nullptr;
    QTimer*             m_timer = nullptr;
    QVector<SpellError> m_errors;
    QColor              m_underlineColor { Qt::red };
};

#endif // SPELLCHECKLINEEDIT_H
