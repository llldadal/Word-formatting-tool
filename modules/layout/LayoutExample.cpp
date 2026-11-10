#include "LayoutModule.h"

#include <QGuiApplication>
#include <QDebug>

int main(int argc, char* argv[])
{
    QGuiApplication application(argc, argv);

    // 创建一份文档，使用结构体里的默认页面设置。
    editor::data::DocumentSnapshot document;

    document.revision = 2;
    document.page.size = QSizeF(1000, 800);
    document.page.margins = QMarginsF(20, 30, 40, 50);

    // 调用我们刚写的排版函数。
    auto result = editor::layout::composeLayout(document);

    // 打印计算结果。
    qDebug() << "revision:" << result.documentRevision;
    qDebug() << "page:" << result.pageRect;
    qDebug() << "content:" << result.contentRect;

    return 0;
}