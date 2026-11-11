#pragma once

#include <QVector>
#include <QWidget>

#include "core/engine/Engines.h"

namespace ks::ui {

/// 键位编辑叠加层：覆盖在视频画面矩形上，绘制摇杆/视角/键位并支持拖拽、缩放。
class KeymapOverlay : public QWidget {
    Q_OBJECT

public:
    explicit KeymapOverlay(QWidget *parent = nullptr);

    void setProfile(const Profile &profile);
    const Profile &profile() const { return m_profile; }
    void setSelected(int index);
    int selected() const { return m_selected; }
    void setGridEnabled(bool enabled);
    void setSnapEnabled(bool enabled);
    void setOverlaps(const QVector<QPair<int, int>> &pairs);
    void setEmptyHint(const QString &text);

signals:
    void selectedChanged(int index);
    void bindingMoved(int index);
    void bindingResized(int index);
    void bindingEdited();
    void deleteRequested();
    void saveRequested();
    void exitRequested();

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;

private:
    QPointF toWidget(const QPointF &normalized) const;
    QPointF toNormalized(const QPoint &pos) const;
    double pixelRadius(double normalizedRadius) const;
    int hitTest(const QPoint &pos) const;

    Profile m_profile;
    int m_selected = -1;
    bool m_grid = true;
    bool m_snap = true;
    bool m_dragging = false;
    QPointF m_dragOffset;
    QVector<QPair<int, int>> m_overlaps;
    QString m_emptyHint;
};

} // namespace ks::ui
