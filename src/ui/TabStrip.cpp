#include "ui/TabStrip.h"

#include "ui/IconFactory.h"
#include "ui/Theme.h"

#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QToolButton>
#include <QVariantAnimation>
#include <QWheelEvent>

#include <algorithm>

namespace ks::ui {
namespace {

constexpr int kStripHeight = 40;
constexpr int kArrowSlot = 56;
constexpr int kArrowButton = 28;
constexpr int kMinTabWidth = 72;
constexpr int kScrollStep = 96;

} // namespace

TabStrip::TabStrip(QWidget *parent) : QWidget(parent) {
    setFixedHeight(kStripHeight);
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);

    auto makeArrow = [this](const QString &icon, const QString &tip) {
        auto *button = new QToolButton(this);
        button->setIcon(IconFactory::icon(icon, Theme::textSecondary(), 16));
        button->setIconSize(QSize(16, 16));
        button->setFixedSize(kArrowButton, kArrowButton);
        button->setToolTip(tip);
        button->setCursor(Qt::PointingHandCursor);
        button->hide();
        return button;
    };
    m_left = makeArrow(QStringLiteral("chevron-left"), QStringLiteral("向前滚动"));
    m_right = makeArrow(QStringLiteral("chevron-right"), QStringLiteral("向后滚动"));

    m_animation = new QVariantAnimation(this);
    m_animation->setDuration(120);
    m_animation->setEasingCurve(QEasingCurve::OutCubic);
    connect(m_animation, &QVariantAnimation::valueChanged, this,
            [this](const QVariant &value) {
                m_offset = value.toInt();
                update();
            });

    connect(m_left, &QToolButton::clicked, this, [this] { scrollBy(-kScrollStep); });
    connect(m_right, &QToolButton::clicked, this, [this] { scrollBy(kScrollStep); });
}

void TabStrip::addTab(const QString &text) {
    m_tabs.append(text);
    if (m_current < 0) {
        m_current = 0;
    }
    updateArrows();
    update();
}

void TabStrip::setCurrentIndex(int index) {
    if (index < 0 || index >= m_tabs.size() || index == m_current) {
        return;
    }
    m_current = index;
    ensureVisible(index);
    update();
    emit currentChanged(index);
}

void TabStrip::resizeEvent(QResizeEvent *event) {
    QWidget::resizeEvent(event);
    const int y = (kStripHeight - kArrowButton) / 2;
    m_left->move(width() - kArrowSlot, y);
    m_right->move(width() - kArrowSlot + kArrowButton, y);
    updateArrows();
}

int TabStrip::arrowWidth() const { return overflowing() ? kArrowSlot : 0; }

int TabStrip::tabAreaWidth() const { return std::max(0, width() - arrowWidth()); }

int TabStrip::tabWidth() const {
    if (m_tabs.isEmpty()) {
        return 0;
    }
    if (overflowing()) {
        return kMinTabWidth;
    }
    return tabAreaWidth() / m_tabs.size();
}

int TabStrip::totalTabsWidth() const { return m_tabs.size() * kMinTabWidth; }

bool TabStrip::overflowing() const {
    if (m_tabs.isEmpty()) {
        return false;
    }
    return totalTabsWidth() > width();
}

int TabStrip::maxOffset() const {
    return std::max(0, totalTabsWidth() - tabAreaWidth());
}

QRect TabStrip::tabRect(int index) const {
    const int w = tabWidth();
    return QRect(index * w - m_offset, 0, w, kStripHeight);
}

int TabStrip::tabAt(const QPoint &pos) const {
    if (m_tabs.isEmpty() || pos.x() >= tabAreaWidth()) {
        return -1;
    }
    const int w = tabWidth();
    if (w <= 0) {
        return -1;
    }
    const int index = (pos.x() + m_offset) / w;
    return index >= 0 && index < m_tabs.size() ? index : -1;
}

void TabStrip::ensureVisible(int index) {
    if (!overflowing()) {
        m_offset = 0;
        return;
    }
    const int w = tabWidth();
    const int left = index * w;
    const int right = left + w;
    if (left < m_offset) {
        animateTo(left);
    } else if (right > m_offset + tabAreaWidth()) {
        animateTo(right - tabAreaWidth());
    }
}

void TabStrip::scrollBy(int delta) {
    animateTo(std::clamp(m_offset + delta, 0, maxOffset()));
}

void TabStrip::animateTo(int offset) {
    const int target = std::clamp(offset, 0, maxOffset());
    if (target == m_offset) {
        return;
    }
    m_animation->stop();
    m_animation->setStartValue(m_offset);
    m_animation->setEndValue(target);
    m_animation->start();
}

void TabStrip::updateArrows() {
    const bool show = overflowing();
    m_left->setVisible(show);
    m_right->setVisible(show);
    if (!show) {
        m_offset = 0;
        return;
    }
    m_left->setEnabled(m_offset > 0);
    m_right->setEnabled(m_offset < maxOffset());
    m_left->setIcon(IconFactory::icon(
        QStringLiteral("chevron-left"),
        m_offset > 0 ? Theme::textSecondary() : Theme::color(QStringLiteral("text-disabled")), 16));
    m_right->setIcon(IconFactory::icon(
        QStringLiteral("chevron-right"),
        m_offset < maxOffset() ? Theme::textSecondary()
                               : Theme::color(QStringLiteral("text-disabled")),
        16));
}

void TabStrip::paintEvent(QPaintEvent *event) {
    (void)event;
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.fillRect(rect(), Theme::bgSurface());

    // 标签区分隔线与箭头组分隔线
    painter.setPen(QPen(Theme::color(QStringLiteral("border-subtle")), 1));
    const int baseY = height() - 1;
    if (overflowing()) {
        painter.drawLine(QPointF(tabAreaWidth(), 0), QPointF(tabAreaWidth(), height()));
    }
    painter.drawLine(QPointF(0, baseY), QPointF(width(), baseY));

    for (int i = 0; i < m_tabs.size(); ++i) {
        const QRect r = tabRect(i);
        if (r.right() < 0 || r.left() > tabAreaWidth()) {
            continue;
        }
        const bool selected = i == m_current;
        const bool hovered = i == m_hover;

        if (selected || hovered) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(selected ? Theme::color(QStringLiteral("bg-hover"))
                                      : Theme::color(QStringLiteral("bg-hover")));
            painter.drawRoundedRect(QRectF(r).adjusted(2, 6, -2, -7), 4, 4);
        }

        QFont font = Theme::uiFont(13, selected ? QFont::DemiBold : QFont::Normal);
        painter.setFont(font);
        painter.setPen(selected ? Theme::textPrimary() : Theme::textSecondary());
        painter.drawText(r, Qt::AlignCenter, m_tabs.at(i));
    }

    if (hasFocus() && m_current >= 0) {
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(Theme::brandPrimary(), 1));
        painter.drawRoundedRect(
            QRectF(tabRect(m_current)).adjusted(2, 5, -2, -6), 4, 4);
    }
}

void TabStrip::mousePressEvent(QMouseEvent *event) {
    if (event->button() != Qt::LeftButton) {
        return;
    }
    setFocus(Qt::MouseFocusReason);
    m_press = tabAt(event->pos());
}

void TabStrip::mouseMoveEvent(QMouseEvent *event) {
    const int hover = tabAt(event->pos());
    if (hover != m_hover) {
        m_hover = hover;
        setCursor(hover >= 0 ? Qt::PointingHandCursor : Qt::ArrowCursor);
        update();
    }
}

void TabStrip::leaveEvent(QEvent *event) {
    QWidget::leaveEvent(event);
    if (m_hover != -1) {
        m_hover = -1;
        update();
    }
}

void TabStrip::wheelEvent(QWheelEvent *event) {
    if (!overflowing()) {
        event->ignore();
        return;
    }
    scrollBy(event->angleDelta().y() > 0 ? -kScrollStep : kScrollStep);
    event->accept();
}

void TabStrip::keyPressEvent(QKeyEvent *event) {
    if (event->key() == Qt::Key_Left && m_current > 0) {
        setCurrentIndex(m_current - 1);
        return;
    }
    if (event->key() == Qt::Key_Right && m_current + 1 < m_tabs.size()) {
        setCurrentIndex(m_current + 1);
        return;
    }
    if (event->key() == Qt::Key_Enter || event->key() == Qt::Key_Return) {
        emit currentChanged(m_current);
        return;
    }
    QWidget::keyPressEvent(event);
}

void TabStrip::mouseReleaseEvent(QMouseEvent *event) {
    if (event->button() == Qt::LeftButton && m_press >= 0 && m_press == tabAt(event->pos())) {
        setCurrentIndex(m_press);
    }
    m_press = -1;
}

} // namespace ks::ui
