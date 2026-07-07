#ifndef PLAYERTITLEBAR_H
#define PLAYERTITLEBAR_H

#include <QWidget>
#include <QLabel>

class QMovie;
class QPushButton;
class MarqueeLabel;

// Title/banner overlay: title text + loading seagull. (Info/Share moved out:
// the description lives in the shell's dynamic Description tab, Share floats
// beside the tab bar's "+".)
class PlayerTitleBar : public QWidget {
    Q_OBJECT
public:
    explicit PlayerTitleBar(QWidget* parent = nullptr);
    void setTitle(const QString& title);
    void setLoading(bool loading);      // show/hide the animated seagull beside the title

signals:
    void closeRequested();              // the banner's X — hard-stop + tear down the player

private:
    MarqueeLabel* titleLabel; // elides in the fixed-width pill, marquees on hover
    QLabel* m_spinner;      // animated seagull shown while a stream is loading
    QMovie* m_movie;        // SeagullAnim.gif driving m_spinner
    QPushButton* m_closeBtn; // far-right X
};

#endif // PLAYERTITLEBAR_H
