#pragma once

#include <QString>

namespace ks {

/// 买断制授权（1 码 1 机）。当前为本地验签骨架：正式版接入 RSA-2048 公钥验签与激活服务。
class LicenseManager {
public:
    static LicenseManager &instance();

    bool isPro() const;
    QString statusText() const;
    QString machineFingerprint() const;

    /// 激活码格式：KS-XXXX-XXXX-XXXX（开发期接受任意合法格式，便于联调）。
    bool activate(const QString &code, QString *error = nullptr);
    void deactivate();

    /// 开发调试开关（仅在 debug 构建生效）。
    void setDevUnlocked(bool unlocked);

private:
    LicenseManager();

    void load();
    void persist() const;

    mutable bool m_pro = false;
    bool m_devUnlocked = false;
    QString m_code;
    QString m_fingerprint;
};

} // namespace ks
