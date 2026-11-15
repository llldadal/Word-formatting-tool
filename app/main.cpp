#include "EditorIntegration.h"
#include "MainWindow.h"

#include <QApplication>

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    MainWindow window;
    editor::app::EditorIntegration integration(window);
    window.show();
    return app.exec();
}
