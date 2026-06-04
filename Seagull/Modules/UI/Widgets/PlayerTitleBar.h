#ifndef PLAYERTITLEBAR_H
#define PLAYERTITLEBAR_H

#include <QWidget>
#include <QLabel>

class PlayerTitleBar : public QWidget {
    Q_OBJECT
public:
    explicit PlayerTitleBar(QWidget* parent = nullptr);
    void setTitle(const QString& title);

private:
    QLabel* titleLabel;
};

#endif // PLAYERTITLEBAR_H