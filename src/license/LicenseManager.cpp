#include "license/LicenseManager.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QSettings>
#include <QStandardPaths>
#include <QSysInfo>

#include <qt_windows.h>

namespace ks {
namespace {

constexpr const char *kLastSeenKey = "license/lastSeenUtc";

QString volumeSerial() {
    DWORD serial = 0;
    if (GetVolumeInformationW(L"C:\\", nullptr, 0, &serial, nullptr, nullptr, nullptr, 0)) {
        return QString::number(serial, 16).toUpper();
    }
    return QStringLiteral("00000000");
}

QString cpuIdentifier() {
    QSettings registry(QStringLiteral("HKEY_LOCAL_MACHINE\\HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0"),
                       QSettings::NativeFormat);
    return registry.value(QStringLiteral("ProcessorNameString")).toString();
}

} // namespace

LicenseManager &LicenseManager::instance() {
    static LicenseManager manager;
    return manager;
}

LicenseManager::LicenseManager() {
    recomputeFingerprint();
    refresh();
}

void LicenseManager::recomputeFingerprint() {
    const QString source = volumeSerial() + QLatin1Char('|') + cpuIdentifier() + QLatin1Char('|') +
                           QSysInfo::machineUniqueId();
    const QByteArray digest = QCryptographicHash::hash(source.toUtf8(), QCryptographicHash::Sha256);

    // 指纹摘要取 SHA-256 前 16 字节（32 hex）：既用于展示，也用于许可证绑定比对。
    QString raw;
    raw.reserve(32);
    for (int i = 0; i < 16; ++i) {
        raw += QStringLiteral("%1")
                   .arg(static_cast<quint8>(digest.at(i)), 2, 16, QLatin1Char('0'))
                   .toUpper();
    }
    m_fingerprint = raw;
}

QString LicenseManager::licenseFilePath() const {
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) +
           QStringLiteral("/license.json");
}

qint64 LicenseManager::lastSeenUtc() const {
    return QSettings().value(QString::fromLatin1(kLastSeenKey), 0).toLongLong();
}

void LicenseManager::storeLastSeen(qint64 seconds) const {
    QSettings().setValue(QString::fromLatin1(kLastSeenKey), seconds);
}

bool LicenseManager::isPro() const {
#ifdef QT_DEBUG
    if (m_devUnlocked) {
        return true;
    }
#endif
    return m_valid;
}

QString LicenseManager::statusText() const {
    return isPro() ? QStringLiteral("PRO") : QStringLiteral("基础版");
}

QString LicenseManager::machineFingerprint() const {
    QString grouped;
    for (int i = 0; i < m_fingerprint.size(); ++i) {
        if (i > 0 && i % 4 == 0) {
            grouped += QLatin1Char('-');
        }
        grouped += m_fingerprint.at(i);
    }
    return grouped;
}

QString LicenseManager::issuedAtText() const {
    if (!m_valid || m_payload.issuedAt <= 0) {
        return {};
    }
    return QDateTime::fromSecsSinceEpoch(m_payload.issuedAt).toString(QStringLiteral("yyyy-MM-dd"));
}

bool LicenseManager::refresh() {
    QFile file(licenseFilePath());
    if (!file.open(QIODevice::ReadOnly)) {
        m_valid = false;
        m_payload = {};
        m_lastErrorCode = LicenseError::FileMissing;
        m_lastError = QStringLiteral("未找到本机授权文件");
        return false;
    }
    const QByteArray content = file.readAll();
    file.close();

    const qint64 now = QDateTime::currentSecsSinceEpoch();
    const qint64 lastSeen = lastSeenUtc();
    const LicenseCheck check = LicenseVerifier::verify(content, m_fingerprint, now, lastSeen);
    m_lastErrorCode = check.error;
    m_lastError = check.message;
    if (!check.ok()) {
        m_valid = false;
        m_payload = {};
        return false;
    }

    m_valid = true;
    m_payload = check.payload;
    if (now > lastSeen) {
        storeLastSeen(now);
    }
    return true;
}

bool LicenseManager::activateFromText(const QByteArray &licenseJson, QString *error) {
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    const LicenseCheck check =
        LicenseVerifier::verify(licenseJson, m_fingerprint, now, lastSeenUtc());
    if (!check.ok()) {
        m_lastErrorCode = check.error;
        m_lastError = check.message;
        if (error) {
            *error = check.message;
        }
        return false;
    }

    QDir().mkpath(QFileInfo(licenseFilePath()).absolutePath());
    QSaveFile file(licenseFilePath());
    if (!file.open(QIODevice::WriteOnly) || file.write(licenseJson) != licenseJson.size() ||
        !file.commit()) {
        const QString message = QStringLiteral("无法写入授权文件：%1").arg(licenseFilePath());
        m_lastErrorCode = LicenseError::Malformed;
        m_lastError = message;
        if (error) {
            *error = message;
        }
        return false;
    }

    m_valid = true;
    m_payload = check.payload;
    m_lastErrorCode = LicenseError::None;
    m_lastError.clear();
    if (now > lastSeenUtc()) {
        storeLastSeen(now);
    }
    return true;
}

bool LicenseManager::activateFromFile(const QString &path, QString *error) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        const QString message = QStringLiteral("无法读取许可证文件：%1").arg(path);
        m_lastErrorCode = LicenseError::FileMissing;
        m_lastError = message;
        if (error) {
            *error = message;
        }
        return false;
    }
    const QByteArray content = file.readAll();
    file.close();
    return activateFromText(content, error);
}

void LicenseManager::deactivate() {
    QFile::remove(licenseFilePath());
    m_valid = false;
    m_payload = {};
    m_lastErrorCode = LicenseError::FileMissing;
    m_lastError = QStringLiteral("未找到本机授权文件");

    QSettings settings;
    settings.remove(QStringLiteral("license/pro"));
    settings.remove(QStringLiteral("license/code"));
    settings.remove(QString::fromLatin1(kLastSeenKey));
}

void LicenseManager::setDevUnlocked(bool unlocked) { m_devUnlocked = unlocked; }

} // namespace ks
