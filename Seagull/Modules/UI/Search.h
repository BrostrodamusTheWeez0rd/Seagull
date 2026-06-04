#ifndef SEARCH_H
#define SEARCH_H
#include <QWidget>
#include <QVBoxLayout>
#include <QLabel>
class Search : public QWidget {
public: explicit Search(QWidget* p = nullptr) : QWidget(p) {
    auto* l = new QVBoxLayout(this);
    l->addWidget(new QLabel("Search W.I.P", this));
}
};
#endif