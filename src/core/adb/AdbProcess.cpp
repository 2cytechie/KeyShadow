#include "core/adb/AdbProcess.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QStandardPaths>

namespace ks {
namespace {

constexpr int kDefaultTimeoutMs = 15000;

struct ProcessResult {
    int exitCode = -1;
    QByteArray stdoutData;
    QByteArray stderrData;
    bool timedOut = false;
};

ProcessResult runProcess(const QString &program, const QStringList &args, int timeoutMs) {
    ProcessResult result;
    QProcess process;
    process.setProgram(program);
    process.setArguments(args);
    process.setProcessChannelMode(QProcess::SeparateChannels);
    process.start();
    if (!process.waitForStarted(timeoutMs)) {
        result.stderrData = process.errorString().toUtf8();
        return result;
    }
    if (!process.waitForFinished(timeoutMs)) {
        process.kill();
        process.waitForFinished(2000);
        result.timedOut = true;
        result.stderrData = "adb command timed out";
        return result;
    }
    result.exitCode = process.exitCode();
    result.stdoutData = process.readAllStandardOutput();
    result.stderrData = process.readAllStandardError();
    return result;
}

QString errorText(const ProcessResult &result) {
    QString text = QString::fromUtf8(result.stderrData).trimmed();
    if (text.isEmpty()) {
        text = QString::fromUtf8(result.stdoutData).trimmed();
    }
    if (text.isEmpty()) {
        text = result.timedOut ? QStringLiteral("adb 执行超时") : QStringLiteral("adb 执行失败");
    }
    return text;
}

} // namespace

AdbProcess::AdbProcess() : m_adbPath(resolveAdbPath()) {}

QString AdbProcess::resolveAdbPath() {
    const QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    const QString override = env.value(QStringLiteral("KEYSHADOW_ADB"));
    if (!override.isEmpty() && QFileInfo::exists(override)) {
        return override;
    }

    const QString appDir = QCoreApplication::applicationDirPath();
    const QStringList candidates = {
        appDir + QStringLiteral("/adb.exe"),
        appDir + QStringLiteral("/platform-tools/adb.exe"),
    };
    for (const QString &candidate : candidates) {
        if (QFileInfo::exists(candidate)) {
            return candidate;
        }
    }

    const QString sdkRoot = env.value(QStringLiteral("ANDROID_HOME"));
    if (!sdkRoot.isEmpty()) {
        const QString adb = sdkRoot + QStringLiteral("/platform-tools/adb.exe");
        if (QFileInfo::exists(adb)) {
            return adb;
        }
    }

    const QString localSdk =
        QDir::homePath() + QStringLiteral("/AppData/Local/Android/sdk/platform-tools/adb.exe");
    if (QFileInfo::exists(localSdk)) {
        return localSdk;
    }

    return QStringLiteral("adb");
}

QList<AdbDevice> AdbProcess::listDevices(QString *error) {
    QList<AdbDevice> devices;
    const ProcessResult result = runProcess(m_adbPath, {QStringLiteral("devices"), QStringLiteral("-l")},
                                            kDefaultTimeoutMs);
    if (result.exitCode != 0) {
        if (error) {
            *error = errorText(result);
        }
        return devices;
    }

    const QString text = QString::fromUtf8(result.stdoutData);
    const QStringList lines = text.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    for (const QString &rawLine : lines) {
        const QString line = rawLine.trimmed();
        if (line.isEmpty() || line.startsWith(QStringLiteral("List of devices")) ||
            line.startsWith(QLatin1Char('*'))) {
            continue;
        }
        // 形如：serial   device usb:1-1 product:xxx model:MI_14 device:xxx transport_id:3
        const QStringList parts = line.split(QRegularExpression(QStringLiteral("\\s+")),
                                             Qt::SkipEmptyParts);
        if (parts.size() < 2) {
            continue;
        }
        AdbDevice device;
        device.serial = parts.at(0);
        device.state = parts.at(1);
        for (int i = 2; i < parts.size(); ++i) {
            const QString &token = parts.at(i);
            const int colon = token.indexOf(QLatin1Char(':'));
            if (colon <= 0) {
                continue;
            }
            const QString key = token.left(colon);
            const QString value = token.mid(colon + 1);
            if (key == QStringLiteral("model")) {
                device.model = value;
            } else if (key == QStringLiteral("product")) {
                device.product = value;
            } else if (key == QStringLiteral("transport_id")) {
                device.transport = QStringLiteral("usb");
            }
        }
        if (device.state == QStringLiteral("device")) {
            devices.append(device);
        }
    }

    if (error) {
        error->clear();
    }
    return devices;
}

bool AdbProcess::shell(const QString &serial, const QString &command, QByteArray *output,
                       QString *error, int timeoutMs) {
    const ProcessResult result = runProcess(
        m_adbPath, {QStringLiteral("-s"), serial, QStringLiteral("shell"), command}, timeoutMs);
    if (output) {
        *output = result.stdoutData;
    }
    if (result.exitCode != 0 || result.timedOut) {
        if (error) {
            *error = errorText(result);
        }
        return false;
    }
    if (error) {
        error->clear();
    }
    return true;
}

QString AdbProcess::getProp(const QString &serial, const QString &prop) {
    QByteArray out;
    if (!shell(serial, QStringLiteral("getprop ") + prop, &out)) {
        return {};
    }
    return QString::fromUtf8(out).trimmed();
}

bool AdbProcess::queryScreenSize(const QString &serial, int *width, int *height) {
    QByteArray out;
    if (!shell(serial, QStringLiteral("wm size"), &out)) {
        return false;
    }
    const QString text = QString::fromUtf8(out);
    static const QRegularExpression re(QStringLiteral("(\\d+)x(\\d+)"));
    QRegularExpressionMatch best;
    auto it = re.globalMatch(text);
    while (it.hasNext()) {
        const QRegularExpressionMatch match = it.next();
        best = match; // 后者优先（Override 尺寸在 Physical 之后）
    }
    if (!best.hasMatch()) {
        return false;
    }
    if (width) {
        *width = best.captured(1).toInt();
    }
    if (height) {
        *height = best.captured(2).toInt();
    }
    return true;
}

bool AdbProcess::push(const QString &serial, const QString &localPath, const QString &remotePath,
                      QString *error, int timeoutMs) {
    const ProcessResult result = runProcess(
        m_adbPath, {QStringLiteral("-s"), serial, QStringLiteral("push"), localPath, remotePath},
        timeoutMs);
    const bool ok = result.exitCode == 0 && !result.timedOut;
    if (!ok && error) {
        *error = errorText(result);
    } else if (error) {
        error->clear();
    }
    return ok;
}

bool AdbProcess::reverse(const QString &serial, const QString &deviceSocketName, quint16 localPort,
                         QString *error) {
    const ProcessResult result = runProcess(
        m_adbPath,
        {QStringLiteral("-s"), serial, QStringLiteral("reverse"),
         QStringLiteral("localabstract:") + deviceSocketName,
         QStringLiteral("tcp:%1").arg(localPort)},
        kDefaultTimeoutMs);
    const bool ok = result.exitCode == 0 && !result.timedOut;
    if (!ok && error) {
        *error = errorText(result);
    } else if (error) {
        error->clear();
    }
    return ok;
}

bool AdbProcess::removeReverse(const QString &serial, const QString &deviceSocketName,
                               QString *error) {
    const ProcessResult result = runProcess(
        m_adbPath,
        {QStringLiteral("-s"), serial, QStringLiteral("reverse"),
         QStringLiteral("--remove"), QStringLiteral("localabstract:") + deviceSocketName},
        kDefaultTimeoutMs);
    const bool ok = result.exitCode == 0 && !result.timedOut;
    if (!ok && error) {
        *error = errorText(result);
    } else if (error) {
        error->clear();
    }
    return ok;
}

bool AdbProcess::startServerDetached(const QString &serial, const QString &command, qint64 *pidOut,
                                     QString *error) {
    qint64 pid = 0;
    const bool ok = QProcess::startDetached(
        m_adbPath, {QStringLiteral("-s"), serial, QStringLiteral("shell"), command},
        QDir::currentPath(), &pid);
    if (pidOut) {
        *pidOut = pid;
    }
    if (!ok && error) {
        *error = QStringLiteral("无法启动 scrcpy-server 进程");
    } else if (error) {
        error->clear();
    }
    return ok;
}

} // namespace ks
