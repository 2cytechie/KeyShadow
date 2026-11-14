#include "license/LicenseVerifier.h"

#include "license/LicensePublicKey.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>

#include <qt_windows.h>

#include <bcrypt.h>

namespace ks {
namespace {

/// 时钟回拨容差：系统时间比上次可信时间早超过该值即判为异常。
constexpr qint64 kClockToleranceSeconds = 24 * 60 * 60;

struct AlgorithmHandle {
    BCRYPT_ALG_HANDLE value = nullptr;
    ~AlgorithmHandle() {
        if (value) {
            BCryptCloseAlgorithmProvider(value, 0);
        }
    }
};

struct KeyHandle {
    BCRYPT_KEY_HANDLE value = nullptr;
    ~KeyHandle() {
        if (value) {
            BCryptDestroyKey(value);
        }
    }
};

struct HashHandle {
    BCRYPT_HASH_HANDLE value = nullptr;
    ~HashHandle() {
        if (value) {
            BCryptDestroyHash(value);
        }
    }
};

PUCHAR mutableBytes(const QByteArray &data) {
    return reinterpret_cast<PUCHAR>(const_cast<char *>(data.constData()));
}

QByteArray sha256(const QByteArray &data) {
    AlgorithmHandle algorithm;
    if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&algorithm.value, BCRYPT_SHA256_ALGORITHM,
                                                    nullptr, 0))) {
        return {};
    }
    HashHandle hash;
    if (!BCRYPT_SUCCESS(BCryptCreateHash(algorithm.value, &hash.value, nullptr, 0, nullptr, 0, 0))) {
        return {};
    }
    if (!BCRYPT_SUCCESS(BCryptHashData(hash.value, mutableBytes(data),
                                       static_cast<ULONG>(data.size()), 0))) {
        return {};
    }
    QByteArray digest(32, Qt::Uninitialized);
    if (!BCRYPT_SUCCESS(BCryptFinishHash(hash.value, reinterpret_cast<PUCHAR>(digest.data()),
                                         32, 0))) {
        return {};
    }
    return digest;
}

bool rsaVerify(const QByteArray &digest, const QByteArray &signature, const QByteArray &modulus,
               const QByteArray &exponent) {
    if (modulus.isEmpty() || exponent.isEmpty() || signature.isEmpty()) {
        return false;
    }
    AlgorithmHandle algorithm;
    if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&algorithm.value, BCRYPT_RSA_ALGORITHM, nullptr,
                                                    0))) {
        return false;
    }

    BCRYPT_RSAKEY_BLOB blob{};
    blob.Magic = BCRYPT_RSAPUBLIC_MAGIC;
    blob.BitLength = static_cast<ULONG>(modulus.size() * 8);
    blob.cbPublicExp = static_cast<ULONG>(exponent.size());
    blob.cbModulus = static_cast<ULONG>(modulus.size());

    QByteArray keyBlob;
    keyBlob.reserve(static_cast<int>(sizeof(blob)) + exponent.size() + modulus.size());
    keyBlob.append(reinterpret_cast<const char *>(&blob), sizeof(blob));
    keyBlob.append(exponent);
    keyBlob.append(modulus);

    KeyHandle key;
    if (!BCRYPT_SUCCESS(BCryptImportKeyPair(algorithm.value, nullptr, BCRYPT_RSAPUBLIC_BLOB,
                                            &key.value, mutableBytes(keyBlob),
                                            static_cast<ULONG>(keyBlob.size()), 0))) {
        return false;
    }

    // PKCS#1 v1.5：必须显式传入填充信息，否则 CNG 无法确定哈希算法。
    BCRYPT_PKCS1_PADDING_INFO paddingInfo{};
    paddingInfo.pszAlgId = BCRYPT_SHA256_ALGORITHM;
    return BCRYPT_SUCCESS(BCryptVerifySignature(key.value, &paddingInfo, mutableBytes(digest),
                                                static_cast<ULONG>(digest.size()),
                                                mutableBytes(signature),
                                                static_cast<ULONG>(signature.size()),
                                                BCRYPT_PAD_PKCS1));
}

LicenseCheck failed(LicenseError error, const QString &message) {
    LicenseCheck check;
    check.error = error;
    check.message = message;
    return check;
}

} // namespace

QByteArray LicenseVerifier::base64UrlDecode(const QByteArray &text) {
    QByteArray normalized = text.trimmed();
    normalized.replace('-', '+');
    normalized.replace('_', '/');
    const int remainder = normalized.size() % 4;
    if (remainder == 1) {
        return {};
    }
    if (remainder != 0) {
        normalized.append(QByteArray(4 - remainder, '='));
    }
    return QByteArray::fromBase64(normalized);
}

QString LicenseVerifier::normalizeFingerprint(const QString &text) {
    QString cleaned;
    cleaned.reserve(text.size());
    for (const QChar ch : text) {
        if (ch.isSpace() || ch == QLatin1Char('-') || ch == QLatin1Char(':')) {
            continue;
        }
        cleaned.append(ch.toUpper());
    }
    return cleaned;
}

LicenseCheck LicenseVerifier::verify(const QByteArray &licenseJson, const QString &machineDigestHex,
                                     qint64 nowUtcSeconds, qint64 lastSeenUtcSeconds) {
    const QByteArray modulus = base64UrlDecode(QByteArray(license::kPublicModulusBase64Url));
    const QByteArray exponent = base64UrlDecode(QByteArray(license::kPublicExponentBase64Url));
    return verifyWithKey(licenseJson, machineDigestHex, nowUtcSeconds, lastSeenUtcSeconds, modulus,
                         exponent);
}

LicenseCheck LicenseVerifier::verifyWithKey(const QByteArray &licenseJson,
                                            const QString &machineDigestHex, qint64 nowUtcSeconds,
                                            qint64 lastSeenUtcSeconds, const QByteArray &modulus,
                                            const QByteArray &exponent) {
    if (licenseJson.trimmed().isEmpty()) {
        return failed(LicenseError::FileMissing, QStringLiteral("未找到本机授权文件"));
    }

    // 1. 结构：外层必须是含 payload / sig 的 JSON 对象。
    QJsonParseError parseError{};
    const QJsonDocument outer = QJsonDocument::fromJson(licenseJson, &parseError);
    if (parseError.error != QJsonParseError::NoError || !outer.isObject()) {
        return failed(LicenseError::Malformed, QStringLiteral("授权文件不是有效的 JSON"));
    }
    const QJsonObject root = outer.object();
    const QByteArray payloadText = root.value(QStringLiteral("payload")).toString().toLatin1();
    const QByteArray signatureText = root.value(QStringLiteral("sig")).toString().toLatin1();
    if (payloadText.isEmpty() || signatureText.isEmpty()) {
        return failed(LicenseError::Malformed, QStringLiteral("授权文件缺少 payload 或签名"));
    }
    const QByteArray payloadBlob = base64UrlDecode(payloadText);
    const QByteArray signature = base64UrlDecode(signatureText);
    if (payloadBlob.isEmpty() || signature.isEmpty()) {
        return failed(LicenseError::Malformed, QStringLiteral("授权文件内容编码损坏"));
    }

    // 2. 验签必须先做：未验签的载荷字段不可信。
    const QByteArray digest = sha256(payloadBlob);
    if (digest.isEmpty() || !rsaVerify(digest, signature, modulus, exponent)) {
        return failed(LicenseError::BadSignature,
                      QStringLiteral("签名校验失败：授权文件已被篡改或与当前版本不匹配"));
    }

    QJsonParseError payloadError{};
    const QJsonDocument payloadDoc = QJsonDocument::fromJson(payloadBlob, &payloadError);
    if (payloadError.error != QJsonParseError::NoError || !payloadDoc.isObject()) {
        return failed(LicenseError::Malformed, QStringLiteral("授权载荷解析失败"));
    }
    const QJsonObject payloadObject = payloadDoc.object();

    LicensePayload payload;
    payload.version = payloadObject.value(QStringLiteral("v")).toInt();
    payload.licenseId = payloadObject.value(QStringLiteral("lid")).toString();
    payload.fingerprint =
        normalizeFingerprint(payloadObject.value(QStringLiteral("fp")).toString());
    payload.edition = payloadObject.value(QStringLiteral("edition")).toString();
    payload.issuedAt = static_cast<qint64>(payloadObject.value(QStringLiteral("iat")).toDouble());
    payload.expiresAt = static_cast<qint64>(payloadObject.value(QStringLiteral("exp")).toDouble());
    const QJsonArray features = payloadObject.value(QStringLiteral("features")).toArray();
    for (const QJsonValue &value : features) {
        payload.features.append(value.toString());
    }

    // 3. 版本：为将来升级预留，未知版本拒绝。
    if (payload.version != 1) {
        return failed(LicenseError::UnsupportedVersion,
                      QStringLiteral("授权格式版本不受支持，请更新客户端"));
    }

    // 4. 指纹：1 码 1 机。
    if (payload.fingerprint.isEmpty() ||
        payload.fingerprint != normalizeFingerprint(machineDigestHex)) {
        return failed(LicenseError::FingerprintMismatch,
                      QStringLiteral("该授权与本机不匹配：请提供本机指纹重新签发"));
    }

    // 5. 有效期：exp = 0 表示永久。
    if (payload.expiresAt != 0 && nowUtcSeconds > payload.expiresAt) {
        return failed(LicenseError::Expired, QStringLiteral("授权已过期"));
    }

    // 6. 时钟回拨：系统时间明显早于上次可信时间。
    if (lastSeenUtcSeconds > 0 && nowUtcSeconds + kClockToleranceSeconds < lastSeenUtcSeconds) {
        return failed(LicenseError::ClockRollback,
                      QStringLiteral("检测到系统时间异常，请校准时间后重试"));
    }

    LicenseCheck check;
    check.error = LicenseError::None;
    check.payload = payload;
    return check;
}

} // namespace ks
