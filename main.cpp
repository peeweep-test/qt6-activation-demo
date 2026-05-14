#include "mainwindow.h"

#include <QApplication>

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);

    MainWindow w;
    w.setWindowTitle("Activation Demo (Primary)");
    w.show();

    return app.exec();
}
