#include "ui/pages/SettingsPage.h"

#include "app/Session.h"
#include "license/LicenseManager.h"
#include "ui/KeyNames.h"
#include "ui/Theme.h"

#include <QApplication>
#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPushButton>
#include <QScrollArea>
#include <QSettings>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QTimer>
#include <QVBoxLayout>

namespace ks::ui {

SettingsPage::SettingsPage(Session *session, QWidget *parent) : QWidget(parent), m_session(session) {
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);

    auto *scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    root->addWidget(scroll);

    auto *host = new QWidget(scroll);
    auto *layout = new QVBoxLayout(host);
    layout->setContentsMargins(24, 20, 24, 24);
    layout->setSpacing(16);

    auto *title = new QLabel(QStringLiteral("设置"), host);
    title->setObjectName(QStringLiteral("PageTitle"));
    layout->addWidget(title);

    // 视频
    QWidget *videoSection = buildSection(QStringLiteral("视频"));
    m_resolutionCombo = new QComboBox(videoSection);
    m_resolutionCombo->addItems({QStringLiteral("原生分辨率"), QStringLiteral("1080p"),
                                 QStringLiteral("720p"), QStringLiteral("480p")});
    addRow(videoSection, QStringLiteral("默认分辨率"), m_resolutionCombo);

    m_fpsCombo = new QComboBox(videoSection);
    m_fpsCombo->addItems({QStringLiteral("30 fps"), QStringLiteral("60 fps"),
                          QStringLiteral("90 fps（Pro）"), QStringLiteral("120 fps（Pro）")});
    addRow(videoSection, QStringLiteral("默认帧率"), m_fpsCombo);

    m_bitrateCombo = new QComboBox(videoSection);
    m_bitrateCombo->addItems({QStringLiteral("4 Mbps"), QStringLiteral("8 Mbps"),
                              QStringLiteral("12 Mbps"), QStringLiteral("16 Mbps"),
                              QStringLiteral("20 Mbps")});
    addRow(videoSection, QStringLiteral("码率上限"), m_bitrateCombo);

    m_screenOffCheck = new QCheckBox(QStringLiteral("开启息屏投屏（Pro）"), videoSection);
    addRow(videoSection, QStringLiteral("息屏投屏"), m_screenOffCheck);

    m_statsOverlayCheck =
        new QCheckBox(QStringLiteral("全屏时在画面中上方显示码率 / 帧率 / 延迟"), videoSection);
    addRow(videoSection, QStringLiteral("全屏信息"), m_statsOverlayCheck);
    layout->addWidget(videoSection);

    // 音频
    QWidget *audioSection = buildSection(QStringLiteral("音频"));
    m_audioCheck = new QCheckBox(QStringLiteral("转发设备声音（Android 10+）"), audioSection);
    addRow(audioSection, QStringLiteral("音频转发"), m_audioCheck);
    auto *audioHint = new QLabel(
        QStringLiteral("音频通道不可用的机型会自动隐藏该能力，不影响视频投屏。"), audioSection);
    audioHint->setProperty("variant", "micro");
    audioHint->setWordWrap(true);
    addRow(audioSection, QString(), audioHint);
    layout->addWidget(audioSection);

    // 输入
    QWidget *inputSection = buildSection(QStringLiteral("输入"));

    // 呼出鼠标：改绑按钮 + 呼出方式两个方框按钮（同行右侧，不再单独一行"呼出方式"）
    auto *summonRow = new QWidget(inputSection);
    auto *summonLayout = new QHBoxLayout(summonRow);
    summonLayout->setContentsMargins(0, 0, 0, 0);
    summonLayout->setSpacing(8);

    m_summonButton = new QPushButton(summonRow);
    m_summonButton->setProperty("role", "summonKey");
    m_summonButton->setFixedSize(120, 34);
    m_summonButton->setFont(Theme::monoFont(12));
    m_summonButton->setCursor(Qt::PointingHandCursor);
    summonLayout->addWidget(m_summonButton);

    m_summonModeLongPress = new QPushButton(QStringLiteral("长按 300ms 呼出"), summonRow);
    m_summonModeHold = new QPushButton(QStringLiteral("按下呼出 · 抬起返回"), summonRow);
    m_summonModeGroup = new QButtonGroup(this);
    m_summonModeGroup->setExclusive(true);
    for (QPushButton *button : {m_summonModeLongPress, m_summonModeHold}) {
        button->setCheckable(true);
        button->setProperty("role", "option");
        button->setFixedHeight(34);
        button->setCursor(Qt::PointingHandCursor);
        m_summonModeGroup->addButton(button);
        summonLayout->addWidget(button);
    }
    summonLayout->addStretch(1);
    addRow(inputSection, QStringLiteral("呼出鼠标"), summonRow);

    m_summonStatus = new QLabel(inputSection);
    m_summonStatus->setProperty("state", "warning");
    m_summonStatus->setWordWrap(true);
    addRow(inputSection, QString(), m_summonStatus);

    // 疾跑键：改绑按钮 + 触发方式（切换/按住），布局与呼出鼠标一致
    auto *sprintRow = new QWidget(inputSection);
    auto *sprintLayout = new QHBoxLayout(sprintRow);
    sprintLayout->setContentsMargins(0, 0, 0, 0);
    sprintLayout->setSpacing(8);

    m_sprintButton = new QPushButton(sprintRow);
    m_sprintButton->setProperty("role", "summonKey");
    m_sprintButton->setFixedSize(120, 34);
    m_sprintButton->setFont(Theme::monoFont(12));
    m_sprintButton->setCursor(Qt::PointingHandCursor);
    sprintLayout->addWidget(m_sprintButton);

    m_sprintModeSwitch = new QPushButton(QStringLiteral("切换"), sprintRow);
    m_sprintModeHold = new QPushButton(QStringLiteral("按住"), sprintRow);
    m_sprintModeGroup = new QButtonGroup(this);
    m_sprintModeGroup->setExclusive(true);
    for (QPushButton *button : {m_sprintModeSwitch, m_sprintModeHold}) {
        button->setCheckable(true);
        button->setProperty("role", "option");
        button->setFixedHeight(34);
        button->setCursor(Qt::PointingHandCursor);
        m_sprintModeGroup->addButton(button);
        sprintLayout->addWidget(button);
    }
    sprintLayout->addStretch(1);
    addRow(inputSection, QStringLiteral("疾跑键"), sprintRow);

    auto *sprintHint = new QLabel(
        QStringLiteral("疾跑时轮盘可滑到外圈（大圆）；疾跑键不参与键位映射，"
                       "如需把 Shift 用作技能键，请先在此修改疾跑键。"),
        inputSection);
    sprintHint->setProperty("variant", "micro");
    sprintHint->setWordWrap(true);
    addRow(inputSection, QString(), sprintHint);

    m_humanizerStatus = new QLabel(inputSection);
    m_humanizerStatus->setProperty("variant", "micro");
    m_humanizerStatus->setWordWrap(true);
    addRow(inputSection, QStringLiteral("拟人化"), m_humanizerStatus);

    auto *warning = new QLabel(
        QStringLiteral("风险提示：使用第三方触控工具可能被游戏安全系统识别。拟人化仅降低行为特征风险，"
                       "不能保证规避所有反作弊机制，请自行判断并承担相应风险。"),
        inputSection);
    warning->setProperty("state", "warning");
    warning->setWordWrap(true);
    addRow(inputSection, QString(), warning);
    layout->addWidget(inputSection);

    // 关于
    QWidget *aboutSection = buildSection(QStringLiteral("关于"));
    auto *version = new QLabel(QStringLiteral("键影 KeyShadow v0.1.0"), aboutSection);
    addRow(aboutSection, QStringLiteral("版本"), version);

    auto *server = new QLabel(
        QStringLiteral("<a href=\"https://github.com/Genymobile/scrcpy\">scrcpy-server (GPLv3)</a>"
                       " · 免 root 触控注入基于 scrcpy 协议"),
        aboutSection);
    server->setOpenExternalLinks(true);
    server->setTextFormat(Qt::RichText);
    addRow(aboutSection, QStringLiteral("开源许可"), server);

    auto *adbPath = new QLabel(m_session->profileStore().directory(), aboutSection);
    adbPath->setProperty("variant", "micro");
    addRow(aboutSection, QStringLiteral("配置目录"), adbPath);

    m_licenseLabel = new QLabel(aboutSection);
    addRow(aboutSection, QStringLiteral("授权状态"), m_licenseLabel);

    auto *fingerprint = new QLabel(LicenseManager::instance().machineFingerprint(), aboutSection);
    fingerprint->setFont(Theme::monoFont(12));
    addRow(aboutSection, QStringLiteral("本机指纹"), fingerprint);

    auto *activateButton = new QPushButton(QStringLiteral("激活 Pro"), aboutSection);
    addRow(aboutSection, QString(), activateButton);
    layout->addWidget(aboutSection);

    layout->addStretch(1);
    scroll->setWidget(host);

    // 交互
    connect(m_resolutionCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this](int) { storeProfile(); });
    connect(m_fpsCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this](int index) {
                if (m_syncing) {
                    return;
                }
                if (index >= 2 && !LicenseManager::instance().isPro()) {
                    QSignalBlocker blocker(m_fpsCombo);
                    m_fpsCombo->setCurrentIndex(1);
                    QMessageBox::information(this, QStringLiteral("付费功能"),
                                             QStringLiteral("90/120 fps 为付费功能，激活后解锁。"));
                    return;
                }
                storeProfile();
            });
    connect(m_bitrateCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this](int) { storeProfile(); });
    connect(m_audioCheck, &QCheckBox::toggled, this, [this](bool) { storeProfile(); });
    connect(m_screenOffCheck, &QCheckBox::toggled, this, [this](bool checked) {
        if (m_syncing) {
            return;
        }
        if (checked && !LicenseManager::instance().isPro()) {
            m_syncing = true;
            m_screenOffCheck->setChecked(false);
            m_syncing = false;
            QMessageBox::information(this, QStringLiteral("Pro 功能"),
                                     QStringLiteral("息屏投屏为 Pro 功能，激活后解锁。"));
            return;
        }
        m_session->setScreenOff(checked);
        storeProfile();
    });
    connect(m_statsOverlayCheck, &QCheckBox::toggled, this, [this](bool checked) {
        if (m_syncing) {
            return;
        }
        QSettings().setValue(QStringLiteral("display/statsOverlay"), checked);
        emit statsOverlayChanged(checked);
    });
    connect(activateButton, &QPushButton::clicked, this, [this] {
        bool ok = false;
        const QString code = QInputDialog::getText(
            this, QStringLiteral("激活 键影 Pro"), QStringLiteral("激活码（KS-XXXX-XXXX-XXXX）"),
            QLineEdit::Normal, QString(), &ok);
        if (!ok) {
            return;
        }
        QString error;
        if (!LicenseManager::instance().activate(code, &error)) {
            QMessageBox::warning(this, QStringLiteral("激活失败"), error);
            return;
        }
        syncFromProfile();
        if (m_session) {
            // 激活后按新授权重算注入侧的拟人化开关，无需用户再操作。
            m_session->applyProfile(m_session->profile(), false);
        }
    });

    QSettings settings;
    m_summonKey = settings.value(QStringLiteral("input/summonKey"), 0x12).toInt();
    const bool holdMode =
        settings.value(QStringLiteral("input/summonMode"), QStringLiteral("longPress"))
            .toString() == QStringLiteral("hold");
    (holdMode ? m_summonModeHold : m_summonModeLongPress)->setChecked(true);
    connect(m_summonModeLongPress, &QPushButton::toggled, this, [this](bool checked) {
        if (!checked || m_syncing) {
            return;
        }
        QSettings().setValue(QStringLiteral("input/summonMode"), QStringLiteral("longPress"));
        emit summonModeChanged(QStringLiteral("longPress"));
    });
    connect(m_summonModeHold, &QPushButton::toggled, this, [this](bool checked) {
        if (!checked || m_syncing) {
            return;
        }
        QSettings().setValue(QStringLiteral("input/summonMode"), QStringLiteral("hold"));
        emit summonModeChanged(QStringLiteral("hold"));
    });

    m_sprintKey = settings.value(QStringLiteral("input/sprintKey"), 0x10).toInt();
    const bool sprintSwitch =
        settings.value(QStringLiteral("input/sprintMode"), QStringLiteral("hold")).toString() ==
        QStringLiteral("switch");
    (sprintSwitch ? m_sprintModeSwitch : m_sprintModeHold)->setChecked(true);
    connect(m_sprintModeSwitch, &QPushButton::toggled, this, [this](bool checked) {
        if (!checked || m_syncing) {
            return;
        }
        QSettings().setValue(QStringLiteral("input/sprintMode"), QStringLiteral("switch"));
        emit sprintModeChanged(QStringLiteral("switch"));
    });
    connect(m_sprintModeHold, &QPushButton::toggled, this, [this](bool checked) {
        if (!checked || m_syncing) {
            return;
        }
        QSettings().setValue(QStringLiteral("input/sprintMode"), QStringLiteral("hold"));
        emit sprintModeChanged(QStringLiteral("hold"));
    });

    m_listenTimer = new QTimer(this);
    m_listenTimer->setSingleShot(true);
    m_listenTimer->setInterval(5000);
    connect(m_listenTimer, &QTimer::timeout, this, [this] {
        stopListen();
        m_summonStatus->setText(QStringLiteral("已取消改绑（5 秒无输入）"));
    });
    connect(m_summonButton, &QPushButton::clicked, this,
            [this] { startListen(ListenTarget::Summon); });
    connect(m_sprintButton, &QPushButton::clicked, this,
            [this] { startListen(ListenTarget::Sprint); });
    refreshListenButtons();

    syncFromProfile();
}

void SettingsPage::refreshListenButtons() {
    const QString listeningText = QStringLiteral("请按下新按键…（Esc 取消）");
    if (m_summonButton) {
        const bool listening = m_listening && m_listenTarget == ListenTarget::Summon;
        m_summonButton->setText(listening ? listeningText : vkName(m_summonKey));
        m_summonButton->setProperty("listening", listening);
        m_summonButton->style()->unpolish(m_summonButton);
        m_summonButton->style()->polish(m_summonButton);
    }
    if (m_sprintButton) {
        const bool listening = m_listening && m_listenTarget == ListenTarget::Sprint;
        m_sprintButton->setText(listening ? listeningText : vkName(m_sprintKey));
        m_sprintButton->setProperty("listening", listening);
        m_sprintButton->style()->unpolish(m_sprintButton);
        m_sprintButton->style()->polish(m_sprintButton);
    }
}

void SettingsPage::startListen(ListenTarget target) {
    if (m_listening || !m_session) {
        return;
    }
    m_listening = true;
    m_listenTarget = target;
    m_captureWasEnabled = m_session->inputCaptureEnabled();
    m_session->setInputCaptureEnabled(false);
    m_summonStatus->clear();
    refreshListenButtons();
    qApp->installEventFilter(this);
    m_listenTimer->start();
}

void SettingsPage::stopListen() {
    if (!m_listening) {
        return;
    }
    m_listening = false;
    qApp->removeEventFilter(this);
    m_listenTimer->stop();
    refreshListenButtons();
    if (m_session) {
        m_session->setInputCaptureEnabled(m_captureWasEnabled);
    }
}

void SettingsPage::handleListenCandidate(int vk) {
    if (isForbiddenSummonKey(vk)) {
        if (vk == 0x01) {
            m_summonStatus->setText(QStringLiteral("鼠标左键用于开火映射，不能作为改绑按键"));
        } else {
            m_summonStatus->setText(
                QStringLiteral("该键已被占用：Esc / F9 / F10 / F11 / Win 不可改绑"));
        }
        return;
    }
    // 呼出键与疾跑键互斥（同一键会先被拦截，无法同时承担两个职责）。
    const bool conflict = m_listenTarget == ListenTarget::Sprint
                              ? vkMatchesSummon(vk, m_summonKey)
                              : vkMatchesSummon(vk, m_sprintKey);
    if (conflict) {
        m_summonStatus->setText(
            m_listenTarget == ListenTarget::Sprint
                ? QStringLiteral("该键已被呼出鼠标占用，请先更换呼出键或另选按键")
                : QStringLiteral("该键已被疾跑键占用，请先更换疾跑键或另选按键"));
        return;
    }
    if (m_listenTarget == ListenTarget::Sprint) {
        m_sprintKey = vk;
        QSettings().setValue(QStringLiteral("input/sprintKey"), vk);
        emit sprintKeyChanged(vk);
    } else {
        m_summonKey = vk;
        QSettings().setValue(QStringLiteral("input/summonKey"), vk);
        emit summonKeyChanged(vk);
    }
    stopListen();
    m_summonStatus->clear();
}

bool SettingsPage::eventFilter(QObject *watched, QEvent *event) {
    if (!m_listening) {
        return QWidget::eventFilter(watched, event);
    }
    if (event->type() == QEvent::KeyPress) {
        auto *keyEvent = static_cast<QKeyEvent *>(event);
        if (keyEvent->key() == Qt::Key_Escape) {
            stopListen();
            m_summonStatus->setText(QStringLiteral("已取消改绑"));
            return true;
        }
        if (keyEvent->isAutoRepeat()) {
            return true;
        }
        handleListenCandidate(vkFromKeyEvent(*keyEvent));
        return true;
    }
    if (event->type() == QEvent::MouseButtonPress) {
        auto *mouseEvent = static_cast<QMouseEvent *>(event);
        int vk = 0;
        if (mouseEvent->button() == Qt::LeftButton) {
            vk = 0x01;
        } else if (mouseEvent->button() == Qt::RightButton) {
            vk = 0x02;
        } else if (mouseEvent->button() == Qt::MiddleButton) {
            vk = 0x04;
        } else if (mouseEvent->button() == Qt::BackButton) {
            vk = 0x06;
        } else if (mouseEvent->button() == Qt::ForwardButton) {
            vk = 0x07;
        }
        if (vk) {
            handleListenCandidate(vk);
        }
        return true;
    }
    return QWidget::eventFilter(watched, event);
}

QWidget *SettingsPage::buildSection(const QString &title) {
    auto *card = new QFrame(this);
    card->setObjectName(QStringLiteral("Card"));
    auto *layout = new QVBoxLayout(card);
    layout->setContentsMargins(20, 16, 20, 16);
    layout->setSpacing(10);

    auto *heading = new QLabel(title, card);
    heading->setObjectName(QStringLiteral("SectionTitle"));
    layout->addWidget(heading);

    auto *form = new QFormLayout();
    form->setContentsMargins(0, 0, 0, 0);
    form->setSpacing(10);
    form->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    layout->addLayout(form);
    return card;
}

void SettingsPage::addRow(QWidget *section, const QString &label, QWidget *control) {
    auto *layout = qobject_cast<QVBoxLayout *>(section->layout());
    if (!layout) {
        return;
    }
    auto *form = qobject_cast<QFormLayout *>(layout->itemAt(1)->layout());
    if (!form) {
        return;
    }
    if (label.isEmpty()) {
        form->addRow(QString(), control);
    } else {
        auto *title = new QLabel(label, section);
        title->setMinimumWidth(120);
        form->addRow(title, control);
    }
}

void SettingsPage::showEvent(QShowEvent *event) {
    QWidget::showEvent(event);
    // 投屏页工具栏/键位方案可能已修改当前方案，进入设置页时重新同步，避免显示旧值。
    syncFromProfile();
}

void SettingsPage::syncFromProfile() {
    m_syncing = true;
    const Profile &profile = m_session->profile();

    m_resolutionCombo->setCurrentIndex(profile.maxSize >= 1080 ? 1
                                         : profile.maxSize >= 720 ? 2
                                         : profile.maxSize >= 480 ? 3
                                                                  : 0);
    m_fpsCombo->setCurrentIndex(profile.maxFps >= 120 ? 3
                              : profile.maxFps >= 90 ? 2
                              : profile.maxFps >= 60 ? 1
                                                     : 0);
    const int mbps = std::max(1, profile.videoBitRate / 1'000'000);
    m_bitrateCombo->setCurrentIndex(mbps <= 4 ? 0 : mbps <= 8 ? 1 : mbps <= 12 ? 2
                                                                  : mbps <= 16 ? 3
                                                                               : 4);
    m_audioCheck->setChecked(profile.audioEnabled);
    m_screenOffCheck->setChecked(profile.screenOff);
    m_statsOverlayCheck->setChecked(
        QSettings().value(QStringLiteral("display/statsOverlay"), true).toBool());

    const bool pro = LicenseManager::instance().isPro();
    if (m_humanizerStatus) {
        m_humanizerStatus->setText(pro ? QStringLiteral("付费版自动开启（触控抖动与随机时序）")
                                       : QStringLiteral("基础版固定时序，付费版自动开启"));
    }
    m_licenseLabel->setText(pro ? QStringLiteral("PRO · 永久有效")
                                : QStringLiteral("基础版 · 功能不受限时可正常游玩"));
    m_licenseLabel->setProperty("state", pro ? "success" : "secondary");
    m_licenseLabel->style()->unpolish(m_licenseLabel);
    m_licenseLabel->style()->polish(m_licenseLabel);

    m_syncing = false;
}

void SettingsPage::storeProfile() {
    if (m_syncing) {
        return;
    }
    Profile profile = m_session->profile();
    static const int resolutions[] = {0, 1080, 720, 480};
    static const int fpsValues[] = {30, 60, 90, 120};
    static const int bitrates[] = {4, 8, 12, 16, 20};
    profile.maxSize = resolutions[std::clamp(m_resolutionCombo->currentIndex(), 0, 3)];
    profile.maxFps = fpsValues[std::clamp(m_fpsCombo->currentIndex(), 0, 3)];
    profile.videoBitRate = bitrates[std::clamp(m_bitrateCombo->currentIndex(), 0, 4)] * 1'000'000;
    profile.audioEnabled = m_audioCheck->isChecked();
    m_session->applyProfile(profile, true);
}

} // namespace ks::ui
