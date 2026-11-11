#pragma once

#include <QObject>
#include <QString>
#include <QStringList>

namespace ks {

struct AdbDevice {
    QString serial;
    QString state;
    QString model;
    QString product;
    QString transport;

    bool isUsb() const { return transport.isEmpty() || transport == QStringLiteral("usb"); }
};

/// 对 adb.exe 的同步封装（短命令）。长驻 server 进程用 startServerDetached。
class AdbProcess {
public:
    AdbProcess();

    static QString resolveAdbPath();
    QString adbPath() const { return m_adbPath; }
    void setAdbPath(const QString &path) { m_adbPath = path; }

    /// adb devices -l
    QList<AdbDevice> listDevices(QString *error = nullptr);

    /// adb -s <serial> shell <command>
    bool shell(const QString &serial, const QString &command, QByteArray *output = nullptr,
               QString *error = nullptr, int timeoutMs = 15000);

    QString getProp(const QString &serial, const QString &prop);

    /// 物理屏幕分辨率（wm size 输出中的 Override/Physical 优先）。
    bool queryScreenSize(const QString &serial, int *width, int *height);

    bool push(const QString &serial, const QString &localPath, const QString &remotePath,
              QString *error = nullptr, int timeoutMs = 60000);

    bool reverse(const QString &serial, const QString &deviceSocketName, quint16 localPort,
                 QString *error = nullptr);
    bool removeReverse(const QString &serial, const QString &deviceSocketName,
                       QString *error = nullptr);
    /// 异步移除反向隧道（不阻塞 UI 线程，用于断开/重连清理）。
    void removeReverseDetached(const QString &serial, const QString &deviceSocketName);
    /// 异步执行 `adb shell <command>`（用于清理本会话的 scrcpy-server 进程）。
    void shellDetached(const QString &serial, const QString &command);

    /// 以分离进程启动 `adb -s <serial> shell <command>`。
    bool startServerDetached(const QString &serial, const QString &command, qint64 *pidOut = nullptr,
                             QString *error = nullptr);

private:
    QString m_adbPath;
};

} // namespace ks
