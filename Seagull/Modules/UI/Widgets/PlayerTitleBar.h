#ifndef PLAYERTITLEBAR_H
#define PLAYERTITLEBAR_H

#include <QWidget>
#include <QLabel>

class QMovie;

class PlayerTitleBar : public QWidget {
    Q_OBJECT
public:
    explicit PlayerTitleBar(QWidget* parent = nullptr);
    void setTitle(const QString& title);
    void setLoading(bool loading); // show/hide the animated seagull beside the title

private:
    QLabel* titleLabel;
    QLabel* m_spinner;  // animated seagull shown while a stream is loading
    QMovie* m_movie;    // SeagullAnim.gif driving m_spinner
};

#endif // PLAYERTITLEBAR_H
