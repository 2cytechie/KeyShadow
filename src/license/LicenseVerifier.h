#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>

namespace ks {

/// 许可证校验失败原因。
enum class LicenseError {
    None,
    FileMissing,
    Malformed,
    UnsupportedVersion,
    BadSignature,
    FingerprintMismatch,
    Expired,
    ClockRollback,
};

struct LicensePayload {
    int version = 0;
    QString licenseId;
    QString fingerprint; ///< 32 hex 大写，无分隔
    QString edition;
    QStringList features;
    qint64 issuedAt = 0;
    qint64 expiresAt = 0; ///< 0 表示永久
};

struct LicenseCheck {
    LicenseError error = LicenseError::FileMissing;
    LicensePayload payload;
    QString message; ///< 中文可读原因
    bool ok() const { return error == LicenseError::None; }
};

/// 离线许可证验签：RSA-2048 + SHA-256 + PKCS#1 v1.5（Windows CNG）。
/// 纯算法模块，不读写配置文件，可被单元测试直接调用。
class LicenseVerifier {
public:
    /// 使用内置公钥验签，并校验版本 / 指纹 / 有效期 / 时钟回拨。
    static LicenseCheck verify(const QByteArray &licenseJson, const QString &machineDigestHex,
                               qint64 nowUtcSeconds, qint64 lastSeenUtcSeconds);

    /// 测试入口：注入指定公钥（base64url 的 modulus / exponent）。
    static LicenseCheck verifyWithKey(const QByteArray &licenseJson,
                                      const QString &machineDigestHex, qint64 nowUtcSeconds,
                                      qint64 lastSeenUtcSeconds, const QByteArray &modulus,
                                      const QByteArray &exponent);

    /// base64url 解码（`-_` → `+/`，自动补齐 `=`）。
    static QByteArray base64UrlDecode(const QByteArray &text);

    /// 指纹归一化：去掉空白 / `-` / `:`，转大写。
    static QString normalizeFingerprint(const QString &text);
};

} // namespace ks
