#include "core/profile/ProfileStore.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>

#include <algorithm>

namespace ks {
namespace {

QJsonObject pointToJson(const QPointF &point) {
    QJsonObject obj;
    obj.insert(QStringLiteral("x"), point.x());
    obj.insert(QStringLiteral("y"), point.y());
    return obj;
}

QPointF pointFromJson(const QJsonObject &obj) {
    return QPointF(obj.value(QStringLiteral("x")).toDouble(0.5),
                   obj.value(QStringLiteral("y")).toDouble(0.5));
}

QJsonObject profileToJson(const Profile &profile) {
    QJsonObject obj;
    obj.insert(QStringLiteral("name"), profile.name);

    QJsonObject joystick;
    joystick.insert(QStringLiteral("center"), pointToJson(profile.joystickCenter));
    joystick.insert(QStringLiteral("radius"), profile.joystickRadius);
    joystick.insert(QStringLiteral("sprintRadius"), profile.joystickSprintRadius);
    joystick.insert(QStringLiteral("deadzone"), profile.joystickDeadzone);
    joystick.insert(QStringLiteral("curve"), profile.joystickCurve);
    obj.insert(QStringLiteral("joystick"), joystick);

    QJsonObject look;
    look.insert(QStringLiteral("center"), pointToJson(profile.lookCenter));
    look.insert(QStringLiteral("radius"), profile.lookRadius);
    look.insert(QStringLiteral("sensitivity"), profile.lookSensitivity);
    look.insert(QStringLiteral("countsPer180"), profile.lookCountsPer180);
    obj.insert(QStringLiteral("look"), look);

    QJsonObject enhance;
    enhance.insert(QStringLiteral("brightness"), profile.brightness);
    enhance.insert(QStringLiteral("contrast"), profile.contrast);
    enhance.insert(QStringLiteral("saturation"), profile.saturation);
    enhance.insert(QStringLiteral("shadow"), profile.shadow);
    enhance.insert(QStringLiteral("sharpness"), profile.sharpness);
    obj.insert(QStringLiteral("enhance"), enhance);

    QJsonObject video;
    video.insert(QStringLiteral("bitRate"), profile.videoBitRate);
    video.insert(QStringLiteral("maxFps"), profile.maxFps);
    video.insert(QStringLiteral("shortEdge"), profile.videoShortEdge);
    video.insert(QStringLiteral("audio"), profile.audioEnabled);
    video.insert(QStringLiteral("screenOff"), profile.screenOff);
    obj.insert(QStringLiteral("video"), video);

    QJsonArray bindings;
    for (const KeyBinding &binding : profile.bindings) {
        QJsonObject item;
        item.insert(QStringLiteral("vk"), binding.vk);
        item.insert(QStringLiteral("center"), pointToJson(binding.center));
        item.insert(QStringLiteral("radius"), binding.radius);
        bindings.append(item);
    }
    obj.insert(QStringLiteral("bindings"), bindings);

    return obj;
}

Profile profileFromJson(const QJsonObject &obj) {
    Profile profile;
    profile.name = obj.value(QStringLiteral("name")).toString(QStringLiteral("默认方案"));

    const QJsonObject joystick = obj.value(QStringLiteral("joystick")).toObject();
    profile.joystickCenter = pointFromJson(joystick.value(QStringLiteral("center")).toObject());
    profile.joystickRadius = std::clamp(joystick.value(QStringLiteral("radius")).toDouble(0.085),
                                        0.02, 0.30);
    // 疾跑大圆不小于小圆：旧文件/手改文件越界时钳制。
    profile.joystickSprintRadius =
        std::clamp(joystick.value(QStringLiteral("sprintRadius")).toDouble(0.13),
                   profile.joystickRadius, 0.30);
    profile.joystickDeadzone = joystick.value(QStringLiteral("deadzone")).toDouble(0.12);
    profile.joystickCurve = joystick.value(QStringLiteral("curve")).toDouble(1.0);

    const QJsonObject look = obj.value(QStringLiteral("look")).toObject();
    profile.lookRadius = std::clamp(look.value(QStringLiteral("radius")).toDouble(0.23), 0.05, 0.45);
    profile.lookCenter = clampNormalizedRegionCenter(
        pointFromJson(look.value(QStringLiteral("center")).toObject()), profile.lookRadius);
    profile.lookSensitivity = look.value(QStringLiteral("sensitivity")).toDouble(1.0);
    profile.lookCountsPer180 = look.value(QStringLiteral("countsPer180")).toDouble(3000.0);

    const QJsonObject enhance = obj.value(QStringLiteral("enhance")).toObject();
    profile.brightness = enhance.value(QStringLiteral("brightness")).toDouble(0.0);
    profile.contrast = enhance.value(QStringLiteral("contrast")).toDouble(0.0);
    profile.saturation = enhance.value(QStringLiteral("saturation")).toDouble(0.0);
    profile.shadow = enhance.value(QStringLiteral("shadow")).toDouble(0.0);
    profile.sharpness = enhance.value(QStringLiteral("sharpness")).toDouble(0.0);

    const QJsonObject video = obj.value(QStringLiteral("video")).toObject();
    profile.videoBitRate = video.value(QStringLiteral("bitRate")).toInt(8000000);
    profile.maxFps = video.value(QStringLiteral("maxFps")).toInt(60);
    profile.videoShortEdge = video.value(QStringLiteral("shortEdge")).toInt(0);
    profile.audioEnabled = video.value(QStringLiteral("audio")).toBool(true);
    profile.screenOff = video.value(QStringLiteral("screenOff")).toBool(false);

    const QJsonArray bindings = obj.value(QStringLiteral("bindings")).toArray();
    // 旧版 action/slideTo 字段直接忽略（键位已全部改为直通语义，不做旧版兼容）。
    for (const QJsonValue &value : bindings) {
        const QJsonObject item = value.toObject();
        KeyBinding binding;
        binding.vk = item.value(QStringLiteral("vk")).toInt();
        binding.center = pointFromJson(item.value(QStringLiteral("center")).toObject());
        binding.radius = item.value(QStringLiteral("radius")).toDouble(0.05);
        profile.bindings.append(binding);
    }

    return profile;
}

} // namespace

ProfileStore::ProfileStore(const QString &directory) {
    const QString base = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    m_directory = directory.isEmpty() ? base + QStringLiteral("/profiles") : directory;
    QDir().mkpath(m_directory);
}

QString ProfileStore::sanitizeFileName(const QString &name) {
    QString safe = name;
    static const QString invalid = QStringLiteral("\\/:*?\"<>|");
    for (const QChar &ch : invalid) {
        safe.replace(ch, QLatin1Char('_'));
    }
    return safe.isEmpty() ? QStringLiteral("profile") : safe;
}

QVector<Profile> ProfileStore::loadAll() const {
    QVector<Profile> profiles;
    QDir dir(m_directory);
    const QStringList files = dir.entryList({QStringLiteral("*.json")}, QDir::Files);
    for (const QString &file : files) {
        QFile handle(dir.filePath(file));
        if (!handle.open(QIODevice::ReadOnly)) {
            continue;
        }
        QJsonParseError parseError{};
        const QJsonDocument doc = QJsonDocument::fromJson(handle.readAll(), &parseError);
        if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
            continue;
        }
        profiles.append(profileFromJson(doc.object()));
    }

    if (profiles.isEmpty()) {
        profiles.append(makeDefaultProfile());
    }
    return profiles;
}

Profile ProfileStore::load(const QString &name, bool *ok) const {
    QFile handle(m_directory + QLatin1Char('/') + sanitizeFileName(name) + QStringLiteral(".json"));
    if (!handle.open(QIODevice::ReadOnly)) {
        if (ok) {
            *ok = false;
        }
        return makeDefaultProfile();
    }
    const QJsonDocument doc = QJsonDocument::fromJson(handle.readAll());
    if (ok) {
        *ok = doc.isObject();
    }
    return doc.isObject() ? profileFromJson(doc.object()) : makeDefaultProfile();
}

bool ProfileStore::save(const Profile &profile, QString *error) {
    QDir().mkpath(m_directory);
    const QString path =
        m_directory + QLatin1Char('/') + sanitizeFileName(profile.name) + QStringLiteral(".json");
    QFile handle(path);
    if (!handle.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (error) {
            *error = QStringLiteral("无法写入方案文件：%1").arg(path);
        }
        return false;
    }
    handle.write(QJsonDocument(profileToJson(profile)).toJson(QJsonDocument::Indented));
    handle.close();
    if (error) {
        error->clear();
    }
    return true;
}

bool ProfileStore::remove(const QString &name) {
    QFile::remove(m_directory + QLatin1Char('/') + sanitizeFileName(name) + QStringLiteral(".json"));
    return true;
}

bool ProfileStore::exportToFile(const Profile &profile, const QString &path,
                                QString *error) const {
    QFile handle(path);
    if (!handle.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (error) {
            *error = QStringLiteral("无法写入文件：%1").arg(path);
        }
        return false;
    }
    handle.write(QJsonDocument(profileToJson(profile)).toJson(QJsonDocument::Indented));
    handle.close();
    if (error) {
        error->clear();
    }
    return true;
}

bool ProfileStore::importFromFile(const QString &path, Profile *profile, QString *error) const {
    QFile handle(path);
    if (!handle.open(QIODevice::ReadOnly)) {
        if (error) {
            *error = QStringLiteral("无法读取文件：%1").arg(path);
        }
        return false;
    }
    QJsonParseError parseError{};
    const QJsonDocument doc = QJsonDocument::fromJson(handle.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        if (error) {
            *error = QStringLiteral("方案文件格式无效：%1").arg(parseError.errorString());
        }
        return false;
    }
    Profile imported = profileFromJson(doc.object());
    if (imported.name.trimmed().isEmpty()) {
        imported.name = QStringLiteral("导入方案");
    }
    if (profile) {
        *profile = imported;
    }
    if (error) {
        error->clear();
    }
    return true;
}

Profile ProfileStore::makeDefaultProfile() {
    Profile profile;
    profile.name = defaultProfileName();

    auto add = [&profile](int vk, double x, double y, double radius) {
        KeyBinding binding;
        binding.vk = vk;
        binding.center = QPointF(x, y);
        binding.radius = radius;
        profile.bindings.append(binding);
    };

    // 默认键位基线（坐标为归一化，可在键位编辑器中拖拽微调）
    add(0x01, 0.855, 0.690, 0.055);  // 左键：开火
    add(0x02, 0.930, 0.420, 0.048); // 右键：开镜
    add(0x20, 0.700, 0.905, 0.045);  // 空格：跳跃
    add(0x43, 0.600, 0.860, 0.038);  // C：蹲下
    add(0x52, 0.520, 0.860, 0.038);  // R：换弹
    add(0x51, 0.440, 0.860, 0.038);  // Q：切枪
    add(0x46, 0.320, 0.930, 0.042);  // F：拾取
    add(0x54, 0.210, 0.930, 0.042);  // T：语音
    add(0x09, 0.060, 0.100, 0.038);  // Tab：背包
    add(0x4D, 0.170, 0.100, 0.038);  // M：地图

    return profile;
}

} // namespace ks
