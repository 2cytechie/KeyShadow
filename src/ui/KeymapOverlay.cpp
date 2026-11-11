#include "ui/KeymapOverlay.h"

#include "ui/KeyNames.h"
#include "ui/Theme.h"

#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

namespace ks::ui {
namespace {

constexpr double kDimAlpha = 0.35;
constexpr int kGridCells = 16;

} // namespace

KeymapOverlay::KeymapOverlay(QWidget *parent) : QWidget(parent) {
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_NoSystemBackground);
    setAutoFillBackground(false);
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    setCursor(Qt::ArrowCursor);
}

void KeymapOverlay::setProfile(const Profile &profile) {
    m_profile = profile;
    update();
}

void KeymapOverlay::setSelected(int index) {
    m_selected = index;
    update();
}

void KeymapOverlay::setGridEnabled(bool enabled) {
    m_grid = enabled;
    update();
}

void KeymapOverlay::setSnapEnabled(bool enabled) {
    m_snap = enabled;
    update();
}

void KeymapOverlay::setOverlaps(const QVector<QPair<int, int>> &pairs) {
    m_overlaps = pairs;
    update();
}

void KeymapOverlay::setEmptyHint(const QString &text) {
    m_emptyHint = text;
    update();
}

QPointF KeymapOverlay::toWidget(const QPointF &normalized) const {
    return QPointF(normalized.x() * width(), normalized.y() * height());
}

QPointF KeymapOverlay::toNormalized(const QPoint &pos) const {
    return QPointF(std::clamp(pos.x() / static_cast<double>(std::max(1, width())), 0.0, 1.0),
                   std::clamp(pos.y() / static_cast<double>(std::max(1, height())), 0.0, 1.0));
}

double KeymapOverlay::pixelRadius(double normalizedRadius) const {
    return normalizedRadius * std::min(width(), height());
}

int KeymapOverlay::hitTest(const QPoint &pos) const {
    for (int i = m_profile.bindings.size() - 1; i >= 0; --i) {
        const KeyBinding &binding = m_profile.bindings.at(i);
        const QPointF center = toWidget(binding.center);
        const double radius = std::max(18.0, pixelRadius(binding.radius));
        if (QLineF(center, pos).length() <= radius) {
            return i;
        }
    }
    return -1;
}

void KeymapOverlay::paintEvent(QPaintEvent *event) {
    (void)event;
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);

    // 压暗画面，保证键位与文字可读
    painter.fillRect(rect(), QColor(0, 0, 0, static_cast<int>(kDimAlpha * 255)));

    // 网格
    if (m_grid) {
        QColor grid = Theme::color(QStringLiteral("border-subtle"));
        grid.setAlpha(102);
        painter.setPen(QPen(grid, 1));
        for (int i = 1; i < kGridCells; ++i) {
            const double x = width() * i / static_cast<double>(kGridCells);
            const double y = height() * i / static_cast<double>(kGridCells);
            painter.drawLine(QPointF(x, 0), QPointF(x, height()));
            painter.drawLine(QPointF(0, y), QPointF(width(), y));
        }
    }

    // 视角触发区
    const QPointF lookCenter = toWidget(m_profile.lookCenter);
    const double lookRx = m_profile.lookRadius * width();
    const double lookRy = m_profile.lookRadius * height();
    QColor info = Theme::color(QStringLiteral("state-info"));
    QColor infoStroke = info;
    infoStroke.setAlpha(170);
    QColor infoFill = info;
    infoFill.setAlpha(24);
    painter.setPen(QPen(infoStroke, 1, Qt::DashLine));
    painter.setBrush(infoFill);
    painter.drawEllipse(lookCenter, lookRx, lookRy);
    painter.setPen(infoStroke);
    painter.drawText(QPointF(lookCenter.x() - 26, lookCenter.y() - lookRy - 6),
                     QStringLiteral("视角触发区"));

    // 摇杆
    const QPointF joystickCenter = toWidget(m_profile.joystickCenter);
    const double joystickRadius = pixelRadius(m_profile.joystickRadius);
    QColor joystickStroke = Theme::color(QStringLiteral("border-strong"));
    painter.setPen(QPen(joystickStroke, 1, Qt::DashLine));
    QColor joystickFill = Theme::brandPrimary();
    joystickFill.setAlpha(18);
    painter.setBrush(joystickFill);
    painter.drawEllipse(joystickCenter, joystickRadius, joystickRadius);
    painter.setPen(QPen(Theme::brandPrimary(), 1.5));
    painter.drawLine(QPointF(joystickCenter.x() - 8, joystickCenter.y()),
                     QPointF(joystickCenter.x() + 8, joystickCenter.y()));
    painter.drawLine(QPointF(joystickCenter.x(), joystickCenter.y() - 8),
                     QPointF(joystickCenter.x(), joystickCenter.y() + 8));
    painter.setPen(Theme::textSecondary());
    painter.drawText(QPointF(joystickCenter.x() - 18, joystickCenter.y() + joystickRadius + 16),
                     QStringLiteral("摇杆"));

    // 键位
    const QFont keyFont = Theme::uiFont(11, QFont::Medium);
    painter.setFont(keyFont);
    for (int i = 0; i < m_profile.bindings.size(); ++i) {
        const KeyBinding &binding = m_profile.bindings.at(i);
        const QPointF center = toWidget(binding.center);
        const double radius = pixelRadius(binding.radius);

        bool overlapping = false;
        for (const QPair<int, int> &pair : m_overlaps) {
            if (pair.first == i || pair.second == i) {
                overlapping = true;
                break;
            }
        }

        QColor stroke = Theme::brandPrimary();
        stroke.setAlpha(200);
        if (overlapping) {
            stroke = Theme::stateWarning();
        }
        if (i == m_selected) {
            stroke = Theme::brandPrimary();
        }
        QColor fill = Theme::brandPrimary();
        fill.setAlpha(i == m_selected ? 44 : 24);

        painter.setBrush(fill);
        painter.setPen(QPen(stroke, i == m_selected ? 2.5 : 2.0,
                            overlapping ? Qt::DashLine : Qt::SolidLine));
        painter.drawEllipse(center, radius, radius);

        const QRectF textRect(center.x() - radius, center.y() - radius, radius * 2, radius * 2);
        const QString label = vkName(binding.vk);
        painter.setPen(QColor(0, 0, 0, 180));
        for (int dx = -1; dx <= 1; dx += 2) {
            for (int dy = -1; dy <= 1; dy += 2) {
                painter.drawText(textRect.translated(dx, dy), Qt::AlignCenter, label);
            }
        }
        painter.setPen(Theme::textPrimary());
        painter.drawText(textRect, Qt::AlignCenter, label);

        if (i == m_selected) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(Theme::brandPrimary());
            for (int dx = -1; dx <= 1; dx += 2) {
                for (int dy = -1; dy <= 1; dy += 2) {
                    painter.drawRect(QRectF(center.x() + dx * radius - 4,
                                            center.y() + dy * radius - 4, 8, 8));
                }
            }
        }
    }

    // 无帧占位提示
    if (!m_emptyHint.isEmpty()) {
        const QFont hintFont = Theme::uiFont(12);
        painter.setFont(hintFont);
        const QFontMetrics metrics(hintFont);
        const QRectF hintRect((width() - metrics.horizontalAdvance(m_emptyHint) - 20) / 2.0, 16,
                              metrics.horizontalAdvance(m_emptyHint) + 20,
                              metrics.height() + 8);
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(8, 10, 13, 160));
        painter.drawRoundedRect(hintRect, 4, 4);
        painter.setPen(Theme::textSecondary());
        painter.drawText(hintRect, Qt::AlignCenter, m_emptyHint);
    }

    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(Theme::color(QStringLiteral("border-subtle")), 1));
    painter.drawRect(rect().adjusted(0, 0, -1, -1));
}

void KeymapOverlay::mousePressEvent(QMouseEvent *event) {
    if (event->button() != Qt::LeftButton) {
        return;
    }
    setFocus(Qt::MouseFocusReason);
    const int index = hitTest(event->pos());
    m_selected = index;
    m_dragging = index >= 0;
    if (m_dragging) {
        m_dragOffset = toWidget(m_profile.bindings.at(index).center) - event->pos();
    }
    update();
    emit selectedChanged(index);
}

void KeymapOverlay::mouseMoveEvent(QMouseEvent *event) {
    if (!m_dragging || m_selected < 0 || m_selected >= m_profile.bindings.size()) {
        return;
    }
    const QPoint target = event->pos() + m_dragOffset.toPoint();
    m_profile.bindings[m_selected].center = toNormalized(target);
    update();
    emit bindingMoved(m_selected);
}

void KeymapOverlay::mouseReleaseEvent(QMouseEvent *event) {
    if (event->button() != Qt::LeftButton || !m_dragging) {
        m_dragging = false;
        return;
    }
    m_dragging = false;
    if (m_selected < 0 || m_selected >= m_profile.bindings.size()) {
        return;
    }
    KeyBinding &binding = m_profile.bindings[m_selected];
    if (m_snap) {
        binding.center.setX(std::round(binding.center.x() * 32.0) / 32.0);
        binding.center.setY(std::round(binding.center.y() * 32.0) / 32.0);
    }
    binding.center.setX(std::clamp(binding.center.x(), 0.0, 1.0));
    binding.center.setY(std::clamp(binding.center.y(), 0.0, 1.0));
    update();
    emit bindingMoved(m_selected);
    emit bindingEdited();
}

void KeymapOverlay::wheelEvent(QWheelEvent *event) {
    if (m_selected < 0 || m_selected >= m_profile.bindings.size()) {
        event->ignore();
        return;
    }
    const double steps = event->angleDelta().y() / 120.0;
    KeyBinding &binding = m_profile.bindings[m_selected];
    binding.radius = std::clamp(binding.radius + steps * 0.005, 0.02, 0.16);
    update();
    emit bindingResized(m_selected);
    emit bindingEdited();
    event->accept();
}

void KeymapOverlay::keyPressEvent(QKeyEvent *event) {
    if (event->key() == Qt::Key_Delete) {
        emit deleteRequested();
        return;
    }
    if (event->key() == Qt::Key_Escape) {
        emit exitRequested();
        return;
    }
    if ((event->modifiers() & Qt::ControlModifier) && event->key() == Qt::Key_S) {
        emit saveRequested();
        return;
    }
    QWidget::keyPressEvent(event);
}

} // namespace ks::ui
