#include "ui/IconFactory.h"

#include <QApplication>
#include <QPainter>
#include <QPainterPath>

namespace ks::ui {
namespace {

void drawGlyph(QPainter &painter, const QString &name, const QRectF &r) {
    const double w = r.width();
    const double h = r.height();
    const QPointF c = r.center();
    QPen pen(painter.pen());

    if (name == QStringLiteral("device")) {
        QPainterPath path;
        path.addRoundedRect(QRectF(r.left() + w * 0.26, r.top() + h * 0.10, w * 0.48, h * 0.80),
                            w * 0.08, w * 0.08);
        painter.drawPath(path);
        painter.drawLine(QPointF(c.x() - w * 0.08, r.top() + h * 0.80),
                         QPointF(c.x() + w * 0.08, r.top() + h * 0.80));
    } else if (name == QStringLiteral("monitor")) {
        QPainterPath path;
        path.addRoundedRect(QRectF(r.left() + w * 0.10, r.top() + h * 0.16, w * 0.80, h * 0.52),
                            w * 0.06, w * 0.06);
        painter.drawPath(path);
        painter.drawLine(QPointF(c.x(), r.top() + h * 0.68), QPointF(c.x(), r.top() + h * 0.84));
        painter.drawLine(QPointF(c.x() - w * 0.18, r.top() + h * 0.86),
                         QPointF(c.x() + w * 0.18, r.top() + h * 0.86));
    } else if (name == QStringLiteral("gamepad")) {
        QPainterPath path;
        path.addRoundedRect(QRectF(r.left() + w * 0.06, r.top() + h * 0.30, w * 0.88, h * 0.42),
                            h * 0.20, h * 0.20);
        painter.drawPath(path);
        painter.drawLine(QPointF(r.left() + w * 0.28, r.top() + h * 0.44),
                         QPointF(r.left() + w * 0.28, r.top() + h * 0.60));
        painter.drawLine(QPointF(r.left() + w * 0.20, r.top() + h * 0.52),
                         QPointF(r.left() + w * 0.36, r.top() + h * 0.52));
        painter.setBrush(pen.color());
        painter.drawEllipse(QPointF(r.left() + w * 0.68, r.top() + h * 0.48), w * 0.035, w * 0.035);
        painter.drawEllipse(QPointF(r.left() + w * 0.76, r.top() + h * 0.56), w * 0.035, w * 0.035);
        painter.setBrush(Qt::NoBrush);
    } else if (name == QStringLiteral("sliders")) {
        painter.drawLine(QPointF(r.left() + w * 0.12, r.top() + h * 0.30),
                         QPointF(r.right() - w * 0.12, r.top() + h * 0.30));
        painter.drawLine(QPointF(r.left() + w * 0.12, r.top() + h * 0.50),
                         QPointF(r.right() - w * 0.12, r.top() + h * 0.50));
        painter.drawLine(QPointF(r.left() + w * 0.12, r.top() + h * 0.70),
                         QPointF(r.right() - w * 0.12, r.top() + h * 0.70));
        painter.setBrush(pen.color());
        painter.drawEllipse(QPointF(r.left() + w * 0.38, r.top() + h * 0.30), w * 0.07, w * 0.07);
        painter.drawEllipse(QPointF(r.left() + w * 0.62, r.top() + h * 0.50), w * 0.07, w * 0.07);
        painter.drawEllipse(QPointF(r.left() + w * 0.46, r.top() + h * 0.70), w * 0.07, w * 0.07);
        painter.setBrush(Qt::NoBrush);
    } else if (name == QStringLiteral("settings")) {
        painter.drawEllipse(c, w * 0.18, h * 0.18);
        for (int i = 0; i < 8; ++i) {
            const double angle = i * M_PI / 4.0;
            painter.drawLine(QPointF(c.x() + std::cos(angle) * w * 0.26,
                                     c.y() + std::sin(angle) * h * 0.26),
                             QPointF(c.x() + std::cos(angle) * w * 0.36,
                                     c.y() + std::sin(angle) * h * 0.36));
        }
    } else if (name == QStringLiteral("refresh")) {
        QRectF arc(r.left() + w * 0.16, r.top() + h * 0.16, w * 0.68, h * 0.68);
        painter.drawArc(arc, 40 * 16, 280 * 16);
        QPainterPath arrow;
        arrow.moveTo(r.right() - w * 0.16, r.top() + h * 0.20);
        arrow.lineTo(r.right() - w * 0.30, r.top() + h * 0.24);
        arrow.lineTo(r.right() - w * 0.22, r.top() + h * 0.38);
        painter.drawPath(arrow);
    } else if (name == QStringLiteral("play")) {
        QPainterPath path;
        path.moveTo(r.left() + w * 0.30, r.top() + h * 0.20);
        path.lineTo(r.left() + w * 0.78, c.y());
        path.lineTo(r.left() + w * 0.30, r.bottom() - h * 0.20);
        path.closeSubpath();
        painter.setBrush(pen.color());
        painter.drawPath(path);
        painter.setBrush(Qt::NoBrush);
    } else if (name == QStringLiteral("stop")) {
        painter.setBrush(pen.color());
        painter.drawRoundedRect(QRectF(r.left() + w * 0.28, r.top() + h * 0.28, w * 0.44, h * 0.44),
                                w * 0.05, w * 0.05);
        painter.setBrush(Qt::NoBrush);
    } else if (name == QStringLiteral("edit")) {
        painter.drawLine(QPointF(r.left() + w * 0.22, r.bottom() - h * 0.22),
                         QPointF(r.right() - w * 0.20, r.top() + h * 0.22));
        painter.drawLine(QPointF(r.left() + w * 0.22, r.bottom() - h * 0.22),
                         QPointF(r.left() + w * 0.38, r.bottom() - h * 0.24));
    } else if (name == QStringLiteral("lock")) {
        QPainterPath body;
        body.addRoundedRect(QRectF(r.left() + w * 0.24, r.top() + h * 0.44, w * 0.52, h * 0.38),
                            w * 0.06, w * 0.06);
        painter.drawPath(body);
        painter.drawArc(QRectF(r.left() + w * 0.32, r.top() + h * 0.18, w * 0.36, h * 0.42),
                        0, 180 * 16);
    } else if (name == QStringLiteral("check")) {
        QPainterPath path;
        path.moveTo(r.left() + w * 0.24, c.y());
        path.lineTo(r.left() + w * 0.44, r.bottom() - h * 0.28);
        path.lineTo(r.right() - w * 0.24, r.top() + h * 0.28);
        painter.drawPath(path);
    } else if (name == QStringLiteral("chevron-left") ||
               name == QStringLiteral("chevron-right")) {
        const double dir = name == QStringLiteral("chevron-left") ? 1.0 : -1.0;
        QPainterPath path;
        path.moveTo(c.x() + dir * w * 0.14, r.top() + h * 0.24);
        path.lineTo(c.x() - dir * w * 0.12, c.y());
        path.lineTo(c.x() + dir * w * 0.14, r.bottom() - h * 0.24);
        painter.drawPath(path);
    } else if (name == QStringLiteral("mouse")) {
        QPainterPath body;
        body.addRoundedRect(QRectF(r.left() + w * 0.30, r.top() + h * 0.12, w * 0.40, h * 0.72),
                            w * 0.20, w * 0.20);
        painter.drawPath(body);
        painter.drawLine(QPointF(c.x(), r.top() + h * 0.14), QPointF(c.x(), r.top() + h * 0.40));
    } else if (name == QStringLiteral("close")) {
        painter.drawLine(QPointF(r.left() + w * 0.30, r.top() + h * 0.30),
                         QPointF(r.right() - w * 0.30, r.bottom() - h * 0.30));
        painter.drawLine(QPointF(r.right() - w * 0.30, r.top() + h * 0.30),
                         QPointF(r.left() + w * 0.30, r.bottom() - h * 0.30));
    } else if (name == QStringLiteral("minimize")) {
        painter.drawLine(QPointF(r.left() + w * 0.28, c.y()), QPointF(r.right() - w * 0.28, c.y()));
    } else if (name == QStringLiteral("maximize")) {
        painter.drawRect(QRectF(r.left() + w * 0.30, r.top() + h * 0.30, w * 0.40, h * 0.40));
    } else {
        painter.drawEllipse(c, w * 0.30, h * 0.30);
    }
}

} // namespace

QPixmap IconFactory::pixmap(const QString &name, const QColor &color, int size) {
    QPixmap pixmap(size, size);
    pixmap.fill(Qt::transparent);

    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    QPen pen(color);
    pen.setWidthF(1.5);
    pen.setCapStyle(Qt::RoundCap);
    pen.setJoinStyle(Qt::RoundJoin);
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);
    drawGlyph(painter, name, QRectF(1.0, 1.0, size - 2.0, size - 2.0));
    painter.end();
    return pixmap;
}

QIcon IconFactory::icon(const QString &name, const QColor &color, int size) {
    return QIcon(pixmap(name, color, size));
}

QPixmap IconFactory::appIcon(int size) {
    const qreal dpr = qApp->devicePixelRatio();
    QIcon icon(QStringLiteral(":/icons/app-icon.svg"));
    QPixmap pixmap = icon.pixmap(QSize(size, size), dpr);
    pixmap.setDevicePixelRatio(dpr);
    return pixmap;
}

} // namespace ks::ui
