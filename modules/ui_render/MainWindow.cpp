#include "MainWindow.h"
#include "ui_MainWindow.h"

#include <QAction>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFontComboBox>
#include <QLabel>
#include <QScrollArea>
#include <QScrollBar>
#include <QStatusBar>
#include <QStringList>
#include <QTimer>
#include <QToolBar>

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
{
    ui->setupUi(this);
    buildToolBar();

    m_pointerLabel = new QLabel(QStringLiteral("纸张坐标：-"), this);
    m_caretLabel = new QLabel(QStringLiteral("光标：未定位"), this);
    m_overflowLabel = new QLabel(QStringLiteral("溢出：无"), this);
    m_zoomLabel = new QLabel(QStringLiteral("缩放：100%"), this);
    m_resultLabel = new QLabel(QStringLiteral("就绪"), this);
    statusBar()->addPermanentWidget(m_pointerLabel);
    statusBar()->addPermanentWidget(m_caretLabel);
    statusBar()->addPermanentWidget(m_overflowLabel);
    statusBar()->addPermanentWidget(m_zoomLabel);
    statusBar()->addPermanentWidget(m_resultLabel);

    connect(ui->actionNew, &QAction::triggered, this, &MainWindow::newDocument);
    connect(ui->actionExportWord, &QAction::triggered, this, &MainWindow::exportWord);
    connect(ui->actionQuit, &QAction::triggered, this, &QWidget::close);
    connect(ui->actionBold, &QAction::triggered, this, &MainWindow::applyBold);
    connect(ui->actionItalic, &QAction::triggered, this, &MainWindow::applyItalic);
    connect(ui->actionUnderline, &QAction::triggered, this, &MainWindow::applyUnderline);
    connect(ui->actionZoomIn, &QAction::triggered, this, [this]() { setZoom(ui->canvas->zoom() * 1.1); });
    connect(ui->actionZoomOut, &QAction::triggered, this, [this]() { setZoom(ui->canvas->zoom() / 1.1); });
    connect(ui->actionZoomActual, &QAction::triggered, this, [this]() { setZoom(1.0); });

    connect(ui->actionFitWidth, &QAction::triggered, this, [this]() {
        setZoom(ui->canvas->zoomToFitWidth(ui->scrollArea->viewport()->width()));
    });

    connect(ui->actionShowBoxes, &QAction::toggled, ui->canvas, &CanvasWidget::setShowLayoutBoxes);
    connect(m_fontCombo, &QFontComboBox::currentFontChanged, this, &MainWindow::applyFontFamily);
    connect(m_styleCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &MainWindow::applyParagraphStyle);
    connect(ui->canvas, &CanvasWidget::pointerMoved, this, &MainWindow::onPointerMoved);

    //留空壳，防止程序崩溃，等待主程序注入真实数据
    m_snapshot.page.size = QSizeF(760, 1000);

    m_layoutResult.pageRect = QRectF(0, 0, 760, 1000);
    m_layoutResult.layoutBounds = m_layoutResult.pageRect;

    updateLayoutData(m_layoutResult);

    ui->canvas->setFocus();
}

MainWindow::~MainWindow()
{
    delete ui;
}

void MainWindow::updateLayoutData(const editor::data::LayoutResult& result)
{
    m_layoutResult = result;
    ui->canvas->setDocumentData(m_snapshot, m_layoutResult);
    updateStatus();
}

void MainWindow::buildToolBar()
{
    ui->mainToolBar->setMovable(false);
    m_fontCombo = new QFontComboBox(ui->mainToolBar);
    m_fontCombo->setMinimumWidth(140);
    m_sizeSpin = new QDoubleSpinBox(ui->mainToolBar);
    m_sizeSpin->setRange(6.0, 96.0);
    m_sizeSpin->setValue(12.0);
    m_sizeSpin->setSuffix(QStringLiteral(" pt"));
    m_sizeSpin->setMaximumWidth(100);
    m_styleCombo = new QComboBox(ui->mainToolBar);
    m_styleCombo->addItems({QStringLiteral("正文"), QStringLiteral("标题"), QStringLiteral("标题 1")});

    ui->mainToolBar->addWidget(m_fontCombo);
    ui->mainToolBar->addWidget(m_sizeSpin);
    ui->mainToolBar->addSeparator();
    ui->mainToolBar->addAction(ui->actionBold);
    ui->mainToolBar->addAction(ui->actionItalic);
    ui->mainToolBar->addAction(ui->actionUnderline);
    ui->mainToolBar->addSeparator();
    ui->mainToolBar->addWidget(new QLabel(QStringLiteral("段落"), ui->mainToolBar));
    ui->mainToolBar->addWidget(m_styleCombo);
    ui->mainToolBar->addSeparator();
    ui->mainToolBar->addAction(ui->actionZoomOut);
    ui->mainToolBar->addAction(ui->actionZoomIn);
    ui->mainToolBar->addAction(ui->actionZoomActual);
}

void MainWindow::newDocument() { showResult(QStringLiteral("已新建空白文档 (待接入IO模块)")); }
void MainWindow::exportWord() { showResult(QStringLiteral("导出入口已触发 (待接入IO模块)")); }
void MainWindow::onPointerMoved(const QPointF& paperPoint) { m_pointerLabel->setText(QStringLiteral("纸张坐标：(%1, %2)").arg(qRound(paperPoint.x())).arg(qRound(paperPoint.y()))); }
void MainWindow::applyFontFamily(const QFont& font) { showResult("更改字体：" + font.family()); }
void MainWindow::applyBold(bool checked) { showResult(checked ? "加粗：开" : "加粗：关"); }
void MainWindow::applyItalic(bool checked) { showResult(checked ? "斜体：开" : "斜体：关"); }
void MainWindow::applyUnderline(bool checked) { showResult(checked ? "下划线：开" : "下划线：关"); }
void MainWindow::applyParagraphStyle(int index) { showResult("更改段落样式"); }

void MainWindow::setZoom(qreal zoom)
{
    zoom = qBound(0.25, zoom, 4.0);
    QScrollBar* horizontal = ui->scrollArea->horizontalScrollBar();
    QScrollBar* vertical = ui->scrollArea->verticalScrollBar();
    const QPointF viewportCenter(horizontal->value() + ui->scrollArea->viewport()->width() / 2.0,
                                 vertical->value() + ui->scrollArea->viewport()->height() / 2.0);
    const QPointF centerPaper = ui->canvas->widgetToPaper(viewportCenter);
    ui->canvas->setZoom(zoom);
    QTimer::singleShot(0, this, [this, centerPaper]() {
        const QPointF center = ui->canvas->paperToWidget(centerPaper);
        ui->scrollArea->horizontalScrollBar()->setValue(static_cast<int>(center.x() - ui->scrollArea->viewport()->width() / 2.0));
        ui->scrollArea->verticalScrollBar()->setValue(static_cast<int>(center.y() - ui->scrollArea->viewport()->height() / 2.0));
    });
    updateStatus();
}

void MainWindow::updateStatus()
{
    m_zoomLabel->setText(QStringLiteral("缩放：%1%").arg(qRound(ui->canvas->zoom() * 100.0)));
    const QRectF& page = m_layoutResult.pageRect;
    const QRectF& bounds = m_layoutResult.layoutBounds;

    QStringList parts;
    if (page.isValid() && bounds.isValid()) {
        const auto append = [&parts](qreal amount, const QString& direction) {
            if (amount > 0.5) {
                parts << QStringLiteral("%1 %2 px").arg(direction).arg(qRound(amount));
            }
        };
        append(bounds.bottom() - page.bottom(), QStringLiteral("向下"));
        append(bounds.right() - page.right(), QStringLiteral("向右"));
        append(page.left() - bounds.left(), QStringLiteral("向左"));
        append(page.top() - bounds.top(), QStringLiteral("向上"));
    }

    if (!parts.isEmpty()) {
        m_overflowLabel->setText(QStringLiteral("溢出：") + parts.join(QStringLiteral("，")));
        m_overflowLabel->setStyleSheet(QStringLiteral("color:#c0392b;"));
    } else {
        m_overflowLabel->setText(QStringLiteral("溢出：无"));
        m_overflowLabel->setStyleSheet(QStringLiteral("color:#2e7d32;"));
    }
}

void MainWindow::showResult(const QString& text)
{
    m_resultLabel->setText(text);
    statusBar()->showMessage(text, 3000);
}