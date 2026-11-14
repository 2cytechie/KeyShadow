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
    m_profile.lookRadius = std::clamp(m_profile.lookRadius, 0.05, 0.45);
    m_profile.lookCenter = clampNormalizedRegionCenter(m_profile.lookCenter, m_profile.lookRadius);
    update();
}

void KeymapOverlay::setSelected(int index) {
    m_selected = index;
    update();
}

void KeymapOverlay::setSelectedRegion(Region region) {
    m_selectedRegion = region;
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

int KeymapOverlay::hitTestRegion(const QPoint &pos) const {
    // 轮盘按疾跑大圆命中（大圆包住小圆）。
    const QPointF joystick = toWidget(m_profile.joystickCenter);
    const double joystickOuter = std::max(12.0, pixelRadius(m_profile.joystickSprintRadius));
    if (QLineF(joystick, pos).length() <= joystickOuter) {
        return static_cast<int>(Region::Joystick);
    }
    const QPointF look = toWidget(m_profile.lookCenter);
    const double rx = m_profile.lookRadius * width();
    const double ry = m_profile.lookRadius * height();
    if (rx > 0.0 && ry > 0.0) {
        const double dx = (pos.x() - look.x()) / rx;
        const double dy = (pos.y() - look.y()) / ry;
        if (dx * dx + dy * dy <= 1.0) {
            return static_cast<int>(Region::Look);
        }
    }
    return static_cast<int>(Region::None);
}

QPointF KeymapOverlay::regionCenter(int region) const {
    return region == static_cast<int>(Region::Look) ? m_profile.lookCenter
                                                    : m_profile.joystickCenter;
}

double KeymapOverlay::regionRadius(int region) const {
    return region == static_cast<int>(Region::Look) ? m_profile.lookRadius
                                                    : m_profile.joystickRadius;
}

void KeymapOverlay::setRegionCenter(int region, const QPointF &normalized) {
    if (region == static_cast<int>(Region::Look)) {
        m_profile.lookCenter = clampNormalizedRegionCenter(normalized, m_profile.lookRadius);
    } else if (region == static_cast<int>(Region::Joystick)) {
        m_profile.joystickCenter = QPointF(std::clamp(normalized.x(), 0.0, 1.0),
                                           std::clamp(normalized.y(), 0.0, 1.0));
    }
}

void KeymapOverlay::setRegionRadius(int region, double radius, bool sprintOuter) {
    if (region == static_cast<int>(Region::Look)) {
        m_profile.lookRadius = std::clamp(radius, 0.05, 0.45);
    } else if (region == static_cast<int>(Region::Joystick)) {
        if (sprintOuter) {
            // 大圆不小于小圆
            m_profile.joystickSprintRadius =
                std::clamp(radius, m_profile.joystickRadius, 0.30);
        } else {
            // 小圆不超过大圆
            m_profile.joystickRadius =
                std::clamp(radius, 0.02, m_profile.joystickSprintRadius);
        }
    }
}

void KeymapOverlay::paintEvent(QPaintEvent *event) {
    (void)event;
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);

    // 压暗画面，保证键位与文字可读
    painter.fillRect(rect(), QColor(0, 0, 0, static_cast<int>(kDimAlpha * 255)));

    // 视角触发区
    const QPointF lookCenter = toWidget(m_profile.lookCenter);
    const double lookRx = m_profile.lookRadius * width();
    const double lookRy = m_profile.lookRadius * height();
    const bool lookSelected = m_selectedRegion == Region::Look;
    QColor info = Theme::color(QStringLiteral("state-info"));
    QColor infoStroke = info;
    infoStroke.setAlpha(lookSelected ? 255 : 170);
    QColor infoFill = info;
    infoFill.setAlpha(lookSelected ? 40 : 24);
    painter.setPen(QPen(infoStroke, lookSelected ? 2.5 : 1,
                        lookSelected ? Qt::SolidLine : Qt::DashLine));
    painter.setBrush(infoFill);
    painter.drawEllipse(lookCenter, lookRx, lookRy);
    painter.setPen(infoStroke);
    painter.drawText(QPointF(lookCenter.x() - 26, lookCenter.y() - lookRy - 6),
                     QStringLiteral("视角触发区"));

    // 摇杆：外圆=疾跑（虚线），内圆=普通滑动上限（实线+十字）
    const QPointF joystickCenter = toWidget(m_profile.joystickCenter);
    const double joystickOuter = pixelRadius(m_profile.joystickSprintRadius);
    const double joystickRadius = pixelRadius(m_profile.joystickRadius);
    const bool joystickSelected = m_selectedRegion == Region::Joystick;
    QColor joystickStroke =
        joystickSelected ? Theme::brandPrimary() : Theme::color(QStringLiteral("border-strong"));
    QColor joystickFill = Theme::brandPrimary();
    joystickFill.setAlpha(joystickSelected ? 26 : 10);
    painter.setPen(QPen(joystickStroke, joystickSelected ? 2.0 : 1, Qt::DashLine));
    painter.setBrush(joystickFill);
    painter.drawEllipse(joystickCenter, joystickOuter, joystickOuter);
    joystickFill.setAlpha(joystickSelected ? 40 : 18);
    painter.setPen(QPen(joystickStroke, joystickSelected ? 2.5 : 1,
                        joystickSelected ? Qt::SolidLine : Qt::DashLine));
    painter.setBrush(joystickFill);
    painter.drawEllipse(joystickCenter, joystickRadius, joystickRadius);
    painter.setPen(QPen(Theme::brandPrimary(), 1.5));
    painter.drawLine(QPointF(joystickCenter.x() - 8, joystickCenter.y()),
                     QPointF(joystickCenter.x() + 8, joystickCenter.y()));
    painter.drawLine(QPointF(joystickCenter.x(), joystickCenter.y() - 8),
                     QPointF(joystickCenter.x(), joystickCenter.y() + 8));
    painter.setPen(Theme::textSecondary());
    painter.drawText(QPointF(joystickCenter.x() - 18, joystickCenter.y() + joystickOuter + 16),
                     QStringLiteral("摇杆"));

    // 键位
    const QFont keyFont = Theme::uiFont(11, QFont::Medium);
    painter.setFont(keyFont);
    for (int i = 0; i < m_profile.bindings.size(); ++i) {
        const KeyBinding &binding = m_profile.bindings.at(i);
        const QPointF center = toWidget(binding.center);
        const double radius = pixelRadius(binding.radius);

        QColor stroke = Theme::brandPrimary();
        stroke.setAlpha(200);
        QColor fill = Theme::brandPrimary();
        fill.setAlpha(i == m_selected ? 44 : 24);

        painter.setBrush(fill);
        painter.setPen(QPen(stroke, i == m_selected ? 2.5 : 2.0, Qt::SolidLine));
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
    if (index >= 0) {
        m_selected = index;
        m_selectedRegion = Region::None;
        m_draggingRegion = false;
        m_dragging = true;
        m_dragOffset = toWidget(m_profile.bindings.at(index).center) - event->pos();
        update();
        emit selectedChanged(index);
        return;
    }

    const int region = hitTestRegion(event->pos());
    m_selected = -1;
    m_dragging = false;
    m_selectedRegion = static_cast<Region>(region);
    m_draggingRegion = region != static_cast<int>(Region::None);
    if (m_draggingRegion) {
        m_regionDragOffset = regionCenter(region) - toNormalized(event->pos());
    }
    update();
    emit selectedChanged(-1);
    emit regionSelected(region);
}

void KeymapOverlay::mouseMoveEvent(QMouseEvent *event) {
    if (m_dragging) {
        if (m_selected < 0 || m_selected >= m_profile.bindings.size()) {
            return;
        }
        const QPoint target = event->pos() + m_dragOffset.toPoint();
        m_profile.bindings[m_selected].center = toNormalized(target);
        update();
        emit bindingMoved(m_selected);
        return;
    }
    if (!m_draggingRegion || m_selectedRegion == Region::None) {
        return;
    }
    const int region = static_cast<int>(m_selectedRegion);
    QPointF center = toNormalized(event->pos()) + m_regionDragOffset;
    setRegionCenter(region, center);
    update();
    emit regionMoved(region);
}

void KeymapOverlay::mouseReleaseEvent(QMouseEvent *event) {
    if (event->button() != Qt::LeftButton) {
        return;
    }
    if (m_draggingRegion && m_selectedRegion != Region::None) {
        m_draggingRegion = false;
        const int region = static_cast<int>(m_selectedRegion);
        QPointF center = regionCenter(region);
        setRegionCenter(region, center);
        update();
        emit regionMoved(region);
        emit regionEdited();
        return;
    }
    if (!m_dragging) {
        return;
    }
    m_dragging = false;
    if (m_selected < 0 || m_selected >= m_profile.bindings.size()) {
        return;
    }
    KeyBinding &binding = m_profile.bindings[m_selected];
    binding.center.setX(std::clamp(binding.center.x(), 0.0, 1.0));
    binding.center.setY(std::clamp(binding.center.y(), 0.0, 1.0));
    update();
    emit bindingMoved(m_selected);
    emit bindingEdited();
}

void KeymapOverlay::wheelEvent(QWheelEvent *event) {
    if (m_selectedRegion != Region::None) {
        const int region = static_cast<int>(m_selectedRegion);
        const double steps = event->angleDelta().y() / 120.0;
        const bool joystick = region == static_cast<int>(Region::Joystick);
        // Ctrl+滚轮调疾跑大圆，普通滚轮调小圆/视角区域。
        const bool outer = joystick && (event->modifiers() & Qt::ControlModifier);
        double current = outer ? m_profile.joystickSprintRadius
                               : (joystick ? m_profile.joystickRadius : m_profile.lookRadius);
        setRegionRadius(region, current + steps * 0.005, outer);
        update();
        emit regionResized(region);
        emit regionEdited();
        event->accept();
        return;
    }
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
