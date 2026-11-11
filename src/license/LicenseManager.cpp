#include "license/LicenseManager.h"

#include <QCryptographicHash>
#include <QRegularExpression>
#include <QSettings>
#include <QSysInfo>

#include <qt_windows.h>

namespace ks {
namespace {

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
    const QString source = volumeSerial() + QLatin1Char('|') + cpuIdentifier() + QLatin1Char('|') +
                           QSysInfo::machineUniqueId();
    const QByteArray digest = QCryptographicHash::hash(source.toUtf8(), QCryptographicHash::Sha256);

    QString fingerprint;
    for (int i = 0; i < 8; ++i) {
        if (i) {
            fingerprint += QLatin1Char(':');
        }
        fingerprint += QStringLiteral("%1").arg(static_cast<quint8>(digest.at(i)), 2, 16,
                                                QLatin1Char('0')).toUpper();
    }
    m_fingerprint = fingerprint;

    load();
}

void LicenseManager::load() {
    QSettings settings;
    m_pro = settings.value(QStringLiteral("license/pro"), false).toBool();
    m_code = settings.value(QStringLiteral("license/code")).toString();
}

void LicenseManager::persist() const {
    QSettings settings;
    settings.setValue(QStringLiteral("license/pro"), m_pro);
    settings.setValue(QStringLiteral("license/code"), m_code);
}

bool LicenseManager::isPro() const {
#ifdef QT_DEBUG
    if (m_devUnlocked) {
        return true;
    }
#endif
    return m_pro;
}

QString LicenseManager::statusText() const {
    return isPro() ? QStringLiteral("PRO") : QStringLiteral("基础版");
}

QString LicenseManager::machineFingerprint() const { return m_fingerprint; }

bool LicenseManager::activate(const QString &code, QString *error) {
    const QString normalized = code.trimmed().toUpper().remove(QLatin1Char(' '));
    static const QRegularExpression pattern(
        QStringLiteral("^KS-[0-9A-F]{4}-[0-9A-F]{4}-[0-9A-F]{4}$"));
    if (!pattern.match(normalized).hasMatch()) {
        if (error) {
            *error = QStringLiteral("激活码格式不正确");
        }
        return false;
    }

    m_pro = true;
    m_code = normalized;
    persist();
    if (error) {
        error->clear();
    }
    return true;
}

void LicenseManager::deactivate() {
    m_pro = false;
    m_code.clear();
    persist();
}

void LicenseManager::setDevUnlocked(bool unlocked) { m_devUnlocked = unlocked; }

} // namespace ks
