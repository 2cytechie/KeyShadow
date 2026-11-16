#pragma once

#include "license/LicenseVerifier.h"

#include <QString>

namespace ks {

/// 离线授权（1 码 1 机）：内置公钥验签 license.json，不信任任何本地布尔值。
class LicenseManager {
public:
    static LicenseManager &instance();

    bool isPro() const;
    QString statusText() const;

    /// 32 hex，4 位一组（供用户复制给发码方）。
    QString machineFingerprint() const;
    /// 32 hex 无分隔（用于绑定比对）。
    QString machineFingerprintRaw() const { return m_fingerprint; }

    QString licenseFilePath() const;
    QString licenseId() const { return m_valid ? m_payload.licenseId : QString(); }
    QString issuedAtText() const;
    QString lastError() const { return m_lastError; }
    LicenseError lastErrorCode() const { return m_lastErrorCode; }

    /// 从文件或粘贴内容激活；成功后写入本机授权文件。
    bool activateFromFile(const QString &path, QString *error = nullptr);
    bool activateFromText(const QByteArray &licenseJson, QString *error = nullptr);
    /// 重新读取并验签（启动、移除后刷新等）。
    bool refresh();
    /// 移除本机授权（删除 license.json 并清理历史残留）。
    void deactivate();

private:
    LicenseManager();

    void recomputeFingerprint();
    qint64 lastSeenUtc() const;
    void storeLastSeen(qint64 seconds) const;

    bool m_valid = false;
    LicensePayload m_payload;
    QString m_fingerprint;
    QString m_lastError;
    LicenseError m_lastErrorCode = LicenseError::FileMissing;
};

} // namespace ks
