#include "BasicDemo_SG3000_CL.h"
#include <QtWidgets/QApplication>

int main(int argc, char *argv[])
{
    QApplication a(argc, argv);
    BasicDemo_SG3000_CL w;
    w.show();
    return a.exec();
}
