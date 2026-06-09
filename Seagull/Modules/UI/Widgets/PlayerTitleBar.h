#ifndef PLAYERTITLEBAR_H
#define PLAYERTITLEBAR_H

#include <QWidget>
#include <QLabel>

class QMovie;
class QPushButton;

class PlayerTitleBar : public QWidget {
    Q_OBJECT
public:
    explicit PlayerTitleBar(QWidget* parent = nullptr);
    void setTitle(const QString& title);
    void setLoading(bool loading);      // show/hide the animated seagull beside the title
    void setActionsVisible(bool on);    // show the Info/Share buttons (online streams only)

signals:
    void infoRequested();   // Info button clicked -> host pops the metadata modal
    void shareRequested();  // Share button clicked -> host copies the source URL

protected:
    void changeEvent(QEvent* event) override; // re-tint the action icons on theme change

private:
    void refreshActionIcons(); // tint the Info/Share SVGs to the theme's overlayFg

    QLabel* titleLabel;
    QLabel* m_spinner;      // animated seagull shown while a stream is loading
    QMovie* m_movie;        // SeagullAnim.gif driving m_spinner
    QPushButton* infoBtn;
    QPushButton* shareBtn;
};

#endif // PLAYERTITLEBAR_H
