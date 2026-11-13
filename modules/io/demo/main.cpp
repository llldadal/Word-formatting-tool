#include "EditorWindow.h"

#include <QApplication>
#include <QCoreApplication>

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("QtDocumentEditor"));
    QCoreApplication::setOrganizationName(QStringLiteral("v0.1"));

    editor::demo::EditorWindow window;
    window.show();
    return app.exec();
}
