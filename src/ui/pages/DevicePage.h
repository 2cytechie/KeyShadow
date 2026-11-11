#pragma once

#include <QWidget>

class QLabel;
class QListWidget;
class QPushButton;

namespace ks {
class Session;
}

namespace ks::ui {

/// 启动与设备连接页：设备发现、四步引导、连接入口。
class DevicePage : public QWidget {
    Q_OBJECT

public:
    explicit DevicePage(Session *session, QWidget *parent = nullptr);

signals:
    void connectRequested(const QString &serial);

public slots:
    void refresh();

private:
    void rebuildDeviceList();
    void updateConnectAvailability();

    Session *m_session = nullptr;
    QListWidget *m_deviceList = nullptr;
    QLabel *m_emptyHint = nullptr;
    QLabel *m_statusLabel = nullptr;
    QPushButton *m_connectButton = nullptr;
};

} // namespace ks::ui
