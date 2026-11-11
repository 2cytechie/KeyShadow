#pragma once

#include <QString>
#include <QVector>

#include "core/engine/Engines.h"

namespace ks {

/// 键位方案持久化（JSON）。存放于 %APPDATA%/KeyShadow/profiles。
class ProfileStore {
public:
    ProfileStore();

    QString directory() const { return m_directory; }

    QVector<Profile> loadAll() const;
    Profile load(const QString &name, bool *ok = nullptr) const;
    bool save(const Profile &profile, QString *error = nullptr);
    bool remove(const QString &name);

    /// 导出方案到任意 JSON 文件（不写入方案目录）。
    bool exportToFile(const Profile &profile, const QString &path, QString *error = nullptr) const;
    /// 从 JSON 文件读取方案；不自动保存，由调用方决定是否写入。
    bool importFromFile(const QString &path, Profile *profile, QString *error = nullptr) const;

    /// 内置默认方案（和平精英键位基线）。
    static Profile makeDefaultProfile();
    static QString defaultProfileName() { return QStringLiteral("和平精英-默认"); }
    static QString sanitizeFileName(const QString &name);

private:
    QString m_directory;
};

} // namespace ks
