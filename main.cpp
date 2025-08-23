#include "mainwindow.h"

#include <QApplication>

int main(int argc, char *argv[])
{
    QApplication a(argc, argv);
    QCoreApplication::setApplicationName("Vulture");
    MainWindow w;
    w.show();
    return a.exec();
}
