#include "Seagull.h"
#include <QApplication>
#include "Modules/UI/MainWindow.h"

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);

    // Instantiate the UI module
    MainWindow window;
    window.show();

    return app.exec();
}