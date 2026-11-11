#include "ui/pages/DevicePage.h"

#include "app/Session.h"
#include "ui/IconFactory.h"
#include "ui/Theme.h"

#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QStyle>
#include <QTimer>
#include <QVBoxLayout>

namespace ks::ui {
namespace {

constexpr int kDeviceStateRole = Qt::UserRole + 1;

QLabel *makeStep(int number, const QString &text) {
    auto *label = new QLabel(QStringLiteral("①  %1").arg(text));
    if (number == 2) {
        label->setText(QStringLiteral("②  %1").arg(text));
    } else if (number == 3) {
        label->setText(QStringLiteral("③  %1").arg(text));
    } else if (number == 4) {
        label->setText(QStringLiteral("④  %1").arg(text));
    }
    label->setProperty("variant", "caption");
    label->setWordWrap(true);
    return label;
}

} // namespace

DevicePage::DevicePage(Session *session, QWidget *parent) : QWidget(parent), m_session(session) {
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(40, 32, 40, 32);
    outer->addStretch(1);

    auto *card = new QFrame(this);
    card->setObjectName(QStringLiteral("Card"));
    card->setMaximumWidth(720);
    card->setMinimumHeight(460);

    auto *cardLayout = new QVBoxLayout(card);
    cardLayout->setContentsMargins(32, 28, 32, 28);
    cardLayout->setSpacing(16);

    // 品牌行
    auto *brandRow = new QHBoxLayout();
    auto *mark = new QLabel(card);
    mark->setPixmap(IconFactory::brandMark(Theme::brandPrimary(), 26));
    auto *brand = new QLabel(QStringLiteral("键影 KeyShadow"), card);
    brand->setObjectName(QStringLiteral("BrandName"));
    auto *version = new QLabel(QStringLiteral("v0.1.0"), card);
    version->setProperty("variant", "micro");
    brandRow->addWidget(mark);
    brandRow->addSpacing(6);
    brandRow->addWidget(brand);
    brandRow->addStretch(1);
    brandRow->addWidget(version);
    cardLayout->addLayout(brandRow);

    auto *slogan = new QLabel(QStringLiteral("用键鼠，玩手机 FPS"), card);
    slogan->setProperty("variant", "caption");
    cardLayout->addWidget(slogan);

    // 四步引导
    auto *stepsRow = new QHBoxLayout();
    stepsRow->setSpacing(8);
    stepsRow->addWidget(makeStep(1, QStringLiteral("开启开发者选项")));
    stepsRow->addWidget(makeStep(2, QStringLiteral("打开 USB 调试")));
    stepsRow->addWidget(makeStep(3, QStringLiteral("数据线连接电脑")));
    stepsRow->addWidget(makeStep(4, QStringLiteral("手机上点「允许」")));
    cardLayout->addLayout(stepsRow);

    // 设备列表
    auto *listTitle = new QLabel(QStringLiteral("已检测到的设备"), card);
    listTitle->setProperty("variant", "caption");
    cardLayout->addWidget(listTitle);

    m_deviceList = new QListWidget(card);
    m_deviceList->setMinimumHeight(96);
    m_deviceList->setMaximumHeight(150);
    cardLayout->addWidget(m_deviceList);

    m_emptyHint = new QLabel(
        QStringLiteral("未检测到设备。请确认数据线支持传输、手机已开启 USB 调试。"), card);
    m_emptyHint->setProperty("variant", "caption");
    m_emptyHint->setWordWrap(true);
    cardLayout->addWidget(m_emptyHint);

    // 操作行
    auto *actionRow = new QHBoxLayout();
    m_statusLabel = new QLabel(card);
    m_statusLabel->setProperty("variant", "caption");
    auto *refreshButton = new QPushButton(QStringLiteral("刷新"), card);
    refreshButton->setProperty("variant", "ghost");
    m_connectButton = new QPushButton(QStringLiteral("开始连接"), card);
    m_connectButton->setProperty("variant", "primary");
    m_connectButton->setMinimumWidth(200);
    m_connectButton->setEnabled(false);

    actionRow->addWidget(m_statusLabel, 1);
    actionRow->addWidget(refreshButton);
    actionRow->addWidget(m_connectButton);
    cardLayout->addLayout(actionRow);

    auto *hint = new QLabel(
        QStringLiteral("连接后会向设备推送 scrcpy-server（免 root）。首次连接需在手机上允许 USB 调试。"),
        card);
    hint->setProperty("variant", "micro");
    hint->setWordWrap(true);
    cardLayout->addWidget(hint);

    auto *cardRow = new QHBoxLayout();
    cardRow->addStretch(1);
    cardRow->addWidget(card);
    cardRow->addStretch(1);
    outer->addLayout(cardRow);
    outer->addStretch(1);

    connect(refreshButton, &QPushButton::clicked, this, &DevicePage::refresh);
    connect(m_connectButton, &QPushButton::clicked, this, [this] {
        const QListWidgetItem *item = m_deviceList->currentItem();
        if (!item) {
            return;
        }
        if (item->data(kDeviceStateRole).toString() != QStringLiteral("device")) {
            m_statusLabel->setText(QStringLiteral("请在手机上点击「允许」完成 USB 调试授权后重试"));
            return;
        }
        emit connectRequested(item->data(Qt::UserRole).toString());
    });
    connect(m_deviceList, &QListWidget::itemSelectionChanged, this,
            [this] { updateConnectAvailability(); });

    if (m_session) {
        connect(m_session, &Session::devicesChanged, this,
                [this](const QList<AdbDevice> &) { rebuildDeviceList(); });
        connect(m_session, &Session::stateChanged, this,
                [this](Session::State state, const QString &message) {
                    m_statusLabel->setText(message);
                    m_statusLabel->setProperty(
                        "state", state == Session::State::Streaming ? "success"
                                 : state == Session::State::Error ? "danger" : "secondary");
                    m_statusLabel->style()->unpolish(m_statusLabel);
                    m_statusLabel->style()->polish(m_statusLabel);
                });
    }

    refresh();
    // adb server 冷启动时首次枚举可能为空，延迟再查一次
    QTimer::singleShot(600, this, [this] { refresh(); });
    QTimer::singleShot(2000, this, [this] { refresh(); });
}

void DevicePage::refresh() {
    if (m_session) {
        m_session->refreshDevices();
    }
}

void DevicePage::rebuildDeviceList() {
    m_deviceList->clear();
    if (!m_session) {
        return;
    }
    const QList<AdbDevice> devices = m_session->devices();
    QListWidgetItem *firstReady = nullptr;
    for (const AdbDevice &device : devices) {
        QString title = device.model.isEmpty() ? device.serial : device.model;
        title.replace(QLatin1Char('_'), QLatin1Char(' '));
        QString stateText = device.state;
        if (stateText == QStringLiteral("unauthorized")) {
            stateText = QStringLiteral("等待授权");
        } else if (stateText == QStringLiteral("offline")) {
            stateText = QStringLiteral("离线");
        }
        auto *item = new QListWidgetItem(
            QStringLiteral("●  %1    %2    %3").arg(title, device.serial, stateText));
        item->setData(Qt::UserRole, device.serial);
        item->setData(kDeviceStateRole, device.state);
        m_deviceList->addItem(item);
        if (device.state == QStringLiteral("device") && !firstReady) {
            firstReady = item;
        }
    }
    const bool empty = devices.isEmpty();
    m_emptyHint->setVisible(empty);
    m_deviceList->setVisible(!empty);
    if (!empty) {
        m_deviceList->setCurrentItem(firstReady ? firstReady : m_deviceList->item(0));
        m_statusLabel->setText(
            firstReady ? QStringLiteral("已检测到 %1 台设备").arg(devices.size())
                       : QStringLiteral("设备已连接，请在手机上点击「允许」完成 USB 调试授权"));
    } else {
        m_statusLabel->setText(QStringLiteral("等待设备接入…"));
    }
}

void DevicePage::updateConnectAvailability() {
    const QListWidgetItem *item = m_deviceList->currentItem();
    const bool ready = item && item->data(kDeviceStateRole).toString() == QStringLiteral("device");
    m_connectButton->setEnabled(ready);
    if (!item) {
        return;
    }
    if (!ready) {
        m_statusLabel->setText(QStringLiteral("请在手机上点击「允许」完成 USB 调试授权后重试"));
    } else if (m_session && m_session->state() != Session::State::Streaming) {
        m_statusLabel->setText(QStringLiteral("已选择 %1，点击「开始连接」")
                                   .arg(item->data(Qt::UserRole).toString()));
    }
}

} // namespace ks::ui
