#pragma once

#include <QStringList>
#include <QWidget>

class QToolButton;
class QVariantAnimation;

namespace ks::ui {

/// 检查器标签栏：水平分段标签 + 溢出横向滚动 + 右侧箭头组（无溢出时隐藏）。
class TabStrip : public QWidget {
    Q_OBJECT

public:
    explicit TabStrip(QWidget *parent = nullptr);

    void addTab(const QString &text);
    int count() const { return m_tabs.size(); }
    int currentIndex() const { return m_current; }
    void setCurrentIndex(int index);

signals:
    void currentChanged(int index);

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void leaveEvent(QEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private:
    int arrowWidth() const;
    int tabAreaWidth() const;
    int tabWidth() const;
    int totalTabsWidth() const;
    bool overflowing() const;
    int maxOffset() const;
    int tabAt(const QPoint &pos) const;
    QRect tabRect(int index) const;
    void ensureVisible(int index);
    void scrollBy(int delta);
    void animateTo(int offset);
    void updateArrows();

    QStringList m_tabs;
    int m_current = -1;
    int m_hover = -1;
    int m_press = -1;
    int m_offset = 0;
    QToolButton *m_left = nullptr;
    QToolButton *m_right = nullptr;
    QVariantAnimation *m_animation = nullptr;
};

} // namespace ks::ui
