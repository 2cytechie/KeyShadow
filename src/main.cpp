#include <QApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QStandardPaths>
#include <QSurfaceFormat>
#include <QTimer>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>

#include <qt_windows.h>
#include <dbghelp.h>

#include "app/Session.h"
#include "license/LicenseManager.h"
#include "ui/MainWindow.h"
#include "ui/Theme.h"

namespace {

QString logFilePath() {
    return QCoreApplication::applicationDirPath() + QStringLiteral("/keyshadow.log");
}

void writeRawLog(const char *text) {
    const QByteArray path = logFilePath().toUtf8();
    HANDLE file = CreateFileA(path.constData(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
                              OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return;
    }
    DWORD written = 0;
    ::WriteFile(file, text, static_cast<DWORD>(std::strlen(text)), &written, nullptr);
    CloseHandle(file);
}

LONG WINAPI crashHandler(EXCEPTION_POINTERS *info) {
    char buffer[512] = {0};
    std::snprintf(buffer, sizeof(buffer), "FATAL unhandled exception 0x%08lX at %p\n",
                  info && info->ExceptionRecord ? info->ExceptionRecord->ExceptionCode : 0u,
                  info && info->ExceptionRecord ? info->ExceptionRecord->ExceptionAddress : nullptr);
    writeRawLog(buffer);

    // 输出调用栈（若 PDB 可见可解析出函数名）
    const HANDLE process = GetCurrentProcess();
    const HANDLE thread = GetCurrentThread();
    SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
    if (SymInitialize(process, nullptr, TRUE) && info) {
        CONTEXT context = *info->ContextRecord;
        STACKFRAME64 frame{};
        frame.AddrPC.Offset = context.Rip;
        frame.AddrPC.Mode = AddrModeFlat;
        frame.AddrFrame.Offset = context.Rbp;
        frame.AddrFrame.Mode = AddrModeFlat;
        frame.AddrStack.Offset = context.Rsp;
        frame.AddrStack.Mode = AddrModeFlat;

        for (int depth = 0; depth < 24; ++depth) {
            if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, thread, &frame, &context, nullptr,
                             SymFunctionTableAccess64, SymGetModuleBase64, nullptr)) {
                break;
            }
            if (!frame.AddrPC.Offset) {
                break;
            }
            char symbolBuffer[sizeof(SYMBOL_INFO) + 256] = {0};
            auto *symbol = reinterpret_cast<SYMBOL_INFO *>(symbolBuffer);
            symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
            symbol->MaxNameLen = 255;
            DWORD64 displacement = 0;
            const DWORD64 address = frame.AddrPC.Offset;

            char line[512] = {0};
            if (SymFromAddr(process, address, &displacement, symbol)) {
                std::snprintf(line, sizeof(line), "  #%02d %s + 0x%llX (0x%llX)\n", depth,
                              symbol->Name, static_cast<unsigned long long>(displacement),
                              static_cast<unsigned long long>(address));
            } else {
                IMAGEHLP_MODULE64 module{};
                module.SizeOfStruct = sizeof(module);
                const char *moduleName = "?";
                if (SymGetModuleInfo64(process, address, &module)) {
                    moduleName = module.ModuleName;
                }
                std::snprintf(line, sizeof(line), "  #%02d %s + 0x%llX (0x%llX)\n", depth,
                              moduleName,
                              static_cast<unsigned long long>(address - module.BaseOfImage),
                              static_cast<unsigned long long>(address));
            }
            writeRawLog(line);
        }
    }
    return EXCEPTION_EXECUTE_HANDLER;
}

void messageHandler(QtMsgType type, const QMessageLogContext &context, const QString &message) {
    Q_UNUSED(context);
    const char *level = "INFO";
    if (type == QtWarningMsg) {
        level = "WARN";
    } else if (type == QtCriticalMsg) {
        level = "CRIT";
    } else if (type == QtFatalMsg) {
        level = "FATAL";
    }
    std::fprintf(stderr, "[%s] %s\n", level, message.toUtf8().constData());

    QFile file(logFilePath());
    if (file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        const QString line = QStringLiteral("%1 [%2] %3\n")
                                 .arg(QDateTime::currentDateTime().toString("HH:mm:ss.zzz"),
                                      QString::fromLatin1(level), message);
        file.write(line.toUtf8());
    }
}

} // namespace

int main(int argc, char *argv[]) {
    SetUnhandledExceptionFilter(crashHandler);
    std::set_terminate([] {
        writeRawLog("FATAL std::terminate called\n");
        std::abort();
    });

    qInstallMessageHandler(messageHandler);
    writeRawLog("---- KeyShadow starting ----\n");

    // 关闭垂直同步：QOpenGLWidget 的呈现默认会阻塞到下一个刷新周期（最长 ~16.7ms），
    // 而键鼠原始输入（WM_INPUT）恰好也在 GUI 线程分发，那段阻塞会直接叠加成输入延迟。
    // 代价是可能撕裂；本工具以延迟优先。必须在 QApplication 构造前设置。
    QSurfaceFormat surfaceFormat = QSurfaceFormat::defaultFormat();
    surfaceFormat.setSwapInterval(0);
    surfaceFormat.setSwapBehavior(QSurfaceFormat::DoubleBuffer);
    QSurfaceFormat::setDefaultFormat(surfaceFormat);

    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("KeyShadow"));
    QApplication::setOrganizationName(QStringLiteral("KeyShadow"));
    QApplication::setApplicationVersion(QStringLiteral("0.1.0"));

    ks::ui::Theme::apply(app);

    // 开发期便捷开关：KEYSHADOW_DEV_PRO=1 时临时解锁 Pro 能力（仅调试用）。
    if (qEnvironmentVariableIntValue("KEYSHADOW_DEV_PRO") == 1) {
        ks::LicenseManager::instance().setDevUnlocked(true);
    }

    ks::Session session;
    ks::ui::MainWindow window(&session);
    window.show();

    // 开发期联调：KEYSHADOW_AUTO_CONNECT=1（或设备序列号）启动后自动连接，便于无需手动点击验证。
    const QString autoConnect = qEnvironmentVariable("KEYSHADOW_AUTO_CONNECT");
    if (!autoConnect.isEmpty()) {
        QTimer::singleShot(1200, &window, [&session, autoConnect] {
            QString serial = autoConnect;
            if (serial == QStringLiteral("1")) {
                session.refreshDevices();
                const auto devices = session.devices();
                if (devices.isEmpty()) {
                    return;
                }
                serial = devices.first().serial;
            }
            session.connectDevice(serial);
        });
    }

    return app.exec();
}
