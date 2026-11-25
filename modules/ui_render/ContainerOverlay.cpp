#include "ContainerOverlay.h"

#include <QFont>
#include <QPen>
#include <QBrush>

namespace editor::ui {
namespace {

bool isTopLevelTextContainer(const data::DocumentSnapshot& snapshot, const data::ContainerId& id)
{
    for (const auto& child : snapshot.root.children) {
        if (child.id == id) {
            return child.type == data::ContainerType::Text;
        }
    }
    return false;
}

} // namespace

void ContainerOverlay::paintSelection(
    QPainter& painter,
    const interaction::ContainerSelectionState& selection,
    const data::LayoutResult& layout,
    qreal zoom,
    const data::DocumentSnapshot* snapshot)
{
    if (selection.empty())
        return;

    const qreal safeZoom = qMax(0.1, zoom);
    const qreal penWidth = 1.5 / safeZoom;
    const qreal handleW = 6.0 / safeZoom;
    const qreal handleH = 14.0 / safeZoom;
    const QColor themeBlue(47, 128, 237);

    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, false);

    for (const auto& selectedId : selection.selectedIds) {
        // 先在容器列表里找外框（含 padding）
        QRectF boxRect;
        bool found = false;
        for (const auto& box : layout.containers) {
            if (box.containerId == selectedId) {
                boxRect = box.rect;
                found = true;
                break;
            }
        }
        bool isImage = false;
        for (const auto& img : layout.images) {
            if (img.containerId == selectedId) {
                isImage = true;
                if (!found) {
                    boxRect = img.rect;
                    found = true;
                }
                break;
            }
        }

        if (!found || !boxRect.isValid())
            continue;

        // 1. 绘制 1.5px 蓝色选中框
        QPen borderPen(themeBlue, penWidth, Qt::SolidLine, Qt::SquareCap, Qt::MiterJoin);
        painter.setPen(borderPen);
        painter.setBrush(Qt::NoBrush);
        painter.drawRect(boxRect);

        // 2. 绘制调节手柄
        painter.setRenderHint(QPainter::Antialiasing, true);
        QPen handlePen(themeBlue, 1.0 / safeZoom);
        QBrush handleBrush(Qt::white);
        painter.setPen(handlePen);
        painter.setBrush(handleBrush);

        if (isImage) {
            // 图片：绘制 4 个角点方形缩放手柄
            const qreal cornerSize = 7.0 / safeZoom;
            const auto drawCorner = [&](QPointF center) {
                painter.drawRect(QRectF(center.x() - cornerSize / 2.0,
                                        center.y() - cornerSize / 2.0,
                                        cornerSize, cornerSize));
            };
            drawCorner(boxRect.topLeft());
            drawCorner(boxRect.topRight());
            drawCorner(boxRect.bottomLeft());
            drawCorner(boxRect.bottomRight());

            // 尺寸足够时叠加 4 个边中点手柄（单轴拉伸）
            const qreal minSpan = 24.0 / safeZoom;
            const qreal edgeLong = 12.0 / safeZoom;
            const qreal edgeShort = 5.0 / safeZoom;
            if (boxRect.height() >= minSpan) {
                painter.drawRoundedRect(QRectF(boxRect.left() - edgeShort / 2.0, boxRect.center().y() - edgeLong / 2.0,
                                               edgeShort, edgeLong), 1.0 / safeZoom, 1.0 / safeZoom);
                painter.drawRoundedRect(QRectF(boxRect.right() - edgeShort / 2.0, boxRect.center().y() - edgeLong / 2.0,
                                               edgeShort, edgeLong), 1.0 / safeZoom, 1.0 / safeZoom);
            }
            if (boxRect.width() >= minSpan) {
                painter.drawRoundedRect(QRectF(boxRect.center().x() - edgeLong / 2.0, boxRect.top() - edgeShort / 2.0,
                                               edgeLong, edgeShort), 1.0 / safeZoom, 1.0 / safeZoom);
                painter.drawRoundedRect(QRectF(boxRect.center().x() - edgeLong / 2.0, boxRect.bottom() - edgeShort / 2.0,
                                               edgeLong, edgeShort), 1.0 / safeZoom, 1.0 / safeZoom);
            }
        } else {
            // 非顶层文本与分组：绘制左右边缘中点宽度调节小手柄；顶层正文容器不绘制手柄
            const bool showWidthHandles = !snapshot || !isTopLevelTextContainer(*snapshot, selectedId);
            if (showWidthHandles) {
                const QPointF leftCenter(boxRect.left(), boxRect.center().y());
                const QPointF rightCenter(boxRect.right(), boxRect.center().y());

                const QRectF leftHandle(leftCenter.x() - handleW / 2.0, leftCenter.y() - handleH / 2.0, handleW, handleH);
                const QRectF rightHandle(rightCenter.x() - handleW / 2.0, rightCenter.y() - handleH / 2.0, handleW, handleH);

                painter.drawRoundedRect(leftHandle, 1.0 / safeZoom, 1.0 / safeZoom);
                painter.drawRoundedRect(rightHandle, 1.0 / safeZoom, 1.0 / safeZoom);
            }
        }
        painter.setRenderHint(QPainter::Antialiasing, false);
    }

    painter.restore();
}

void ContainerOverlay::paintHoverBorder(
    QPainter& painter,
    const data::ContainerId& hoveredId,
    const data::LayoutResult& layout,
    qreal zoom)
{
    if (hoveredId.isEmpty())
        return;

    for (const auto& box : layout.containers) {
        if (box.containerId == hoveredId && box.rect.isValid()) {
            const qreal safeZoom = qMax(0.1, zoom);
            painter.save();
            painter.setRenderHint(QPainter::Antialiasing, false);
            QPen hoverPen(QColor(47, 128, 237, 120), 1.0 / safeZoom, Qt::DashLine);
            painter.setPen(hoverPen);
            painter.setBrush(Qt::NoBrush);
            painter.drawRect(box.rect);
            painter.restore();
            return;
        }
    }
}

void ContainerOverlay::paintEmptyPlaceholder(
    QPainter& painter,
    const data::LayoutResult& layout,
    qreal zoom)
{
    if (!layout.containers.empty() || !layout.images.empty())
        return;

    const qreal safeZoom = qMax(0.1, zoom);
    const QRectF area = layout.contentRect.isValid() ? layout.contentRect : layout.pageRect;
    if (!area.isValid())
        return;

    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);

    const qreal boxW = qMin<qreal>(360.0, area.width() * 0.8);
    const qreal boxH = 100.0;
    const QRectF centerBox(area.center().x() - boxW / 2.0,
                          area.center().y() - boxH / 2.0,
                          boxW, boxH);

    // 弱灰色虚线框
    QPen dashPen(QColor(189, 195, 199), 1.5 / safeZoom, Qt::DashLine);
    painter.setPen(dashPen);
    painter.setBrush(QColor(248, 249, 250, 160));
    painter.drawRoundedRect(centerBox, 6.0 / safeZoom, 6.0 / safeZoom);

    // 居中引导文案
    QFont font(QStringLiteral("Microsoft YaHei"));
    font.setPixelSize(qMax(1, qRound(13.0)));
    painter.setFont(font);
    painter.setPen(QColor(127, 140, 141));
    painter.drawText(centerBox, Qt::AlignCenter, QStringLiteral("页面为空\n右键点击此处新增文本容器"));

    painter.restore();
}

} // namespace editor::ui
