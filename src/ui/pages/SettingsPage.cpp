#include "ui/pages/SettingsPage.h"

#include "app/Session.h"
#include "license/LicenseManager.h"
#include "ui/KeyNames.h"
#include "ui/LicenseDialog.h"
#include "ui/Theme.h"
#include "ui/VideoOptionsUi.h"
#include "ui/VideoPresentationSettings.h"

#include <QApplication>
#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
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
    populateResolutions(m_resolutionCombo);
    addRow(videoSection, QStringLiteral("默认分辨率"), m_resolutionCombo);

    m_fpsCombo = new QComboBox(videoSection);
    m_fpsCombo->addItems({QStringLiteral("30 fps"), QStringLiteral("60 fps"),
                          QStringLiteral("90 fps（Pro）"), QStringLiteral("120 fps（Pro）")});
    addRow(videoSection, QStringLiteral("默认帧率"), m_fpsCombo);

    m_bitrateCombo = new QComboBox(videoSection);
    populateBitrates(m_bitrateCombo);
    addRow(videoSection, QStringLiteral("码率上限"), m_bitrateCombo);

    m_renderModeCombo = new QComboBox(videoSection);
    m_renderModeCombo->addItems({QStringLiteral("帧率优先"), QStringLiteral("画质优先")});
    m_renderModeCombo->setToolTip(QStringLiteral("帧率优先使用快速双三次缩放，CAS 上限 25%，超出处理预算时逐步减弱增强；画质优先保留高质量缩放和所选锐化强度。均保留分辨率和帧率目标。"));
    addRow(videoSection, QStringLiteral("投屏模式"), m_renderModeCombo);

    m_qualityCodecCombo = new QComboBox(videoSection);
    m_qualityCodecCombo->setObjectName(QStringLiteral("QualityCodecCombo"));
    m_qualityCodecCombo->addItem(QStringLiteral("H.264（默认，低延迟）"), QStringLiteral("h264"));
    m_qualityCodecCombo->addItem(QStringLiteral("H.265（试用，连接后验证）"), QStringLiteral("h265"));
    addRow(videoSection, QStringLiteral("画质优先编码"), m_qualityCodecCombo);
    auto *codecHint = new QLabel(QStringLiteral(
        "帧率优先固定使用 H.264。画质优先可试用 H.265；连接或硬解验证失败时回退 H.264，并保留分辨率、码率和帧率目标。"), videoSection);
    codecHint->setWordWrap(true);
    codecHint->setProperty("variant", "micro");
    addRow(videoSection, QString(), codecHint);
    m_codecStatus = new QLabel(videoSection);
    m_codecStatus->setWordWrap(true);
    m_codecStatus->setProperty("variant", "micro");
    addRow(videoSection, QStringLiteral("编码状态"), m_codecStatus);
    m_retryCodecButton = new QPushButton(QStringLiteral("重试 H.265"), videoSection);
    addRow(videoSection, QString(), m_retryCodecButton);

    m_verticalSyncCheck =
        new QCheckBox(QStringLiteral("开启垂直同步（重启软件后生效）"), videoSection);
    m_verticalSyncCheck->setObjectName(QStringLiteral("VerticalSyncCheck"));
    addRow(videoSection, QStringLiteral("垂直同步"), m_verticalSyncCheck);
    auto *verticalSyncHint = new QLabel(
        QStringLiteral("开启可减少画面撕裂；关闭可降低显示延迟，但可能出现撕裂。FPS 按解码帧数统计。"),
        videoSection);
    verticalSyncHint->setProperty("variant", "micro");
    verticalSyncHint->setWordWrap(true);
    addRow(videoSection, QString(), verticalSyncHint);

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

    m_humanizerCheck = new QCheckBox(QStringLiteral("开启拟人化（Pro）"), inputSection);
    addRow(inputSection, QStringLiteral("拟人化"), m_humanizerCheck);

    m_humanizerStatus = new QLabel(inputSection);
    m_humanizerStatus->setProperty("variant", "micro");
    m_humanizerStatus->setWordWrap(true);
    addRow(inputSection, QString(), m_humanizerStatus);

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
    fingerprint->setWordWrap(true);
    fingerprint->setTextInteractionFlags(Qt::TextSelectableByMouse);
    addRow(aboutSection, QStringLiteral("本机指纹"), fingerprint);

    auto *activateButton = new QPushButton(QStringLiteral("激活 / 导入许可证"), aboutSection);
    addRow(aboutSection, QString(), activateButton);
    layout->addWidget(aboutSection);

    layout->addStretch(1);
    scroll->setWidget(host);

    // 交互
    connect(m_resolutionCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this](int index) {
                if (m_syncing) {
                    return;
                }
                // 原生分辨率为付费功能：未激活时回退到方案当前值。
                if (index == 0 && !LicenseManager::instance().isPro()) {
                    syncResolution(m_resolutionCombo, m_session->profile());
                    QMessageBox::information(this, QStringLiteral("付费功能"),
                                             QStringLiteral("原生分辨率为付费功能，激活后解锁。"));
                    return;
                }
                storeProfile();
            });
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
    connect(m_renderModeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this](int index) { if (!m_syncing) m_session->setQualityPriority(index == 1); });
    connect(m_qualityCodecCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this](int) {
                if (!m_syncing) m_session->setQualityCodec(m_qualityCodecCombo->currentData().toString());
            });
    connect(m_retryCodecButton, &QPushButton::clicked, m_session, &Session::retryQualityCodec);
    connect(m_session, &Session::videoOptionsChanged, this, &SettingsPage::syncFromProfile);
    connect(m_verticalSyncCheck, &QCheckBox::toggled, this, [this](bool checked) {
        if (m_syncing) return;
        QSettings settings;
        setVerticalSyncEnabled(settings, checked);
    });
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
    });
    connect(m_audioCheck, &QCheckBox::toggled, this, [this](bool checked) {
        if (!m_syncing && m_session) {
            m_session->setAudioEnabled(checked);
        }
    });
    connect(m_humanizerCheck, &QCheckBox::toggled, this, [this](bool checked) {
        if (m_syncing || !m_session) {
            return;
        }
        if (checked && !LicenseManager::instance().isPro()) {
            m_syncing = true;
            m_humanizerCheck->setChecked(false);
            m_syncing = false;
            QMessageBox::information(this, QStringLiteral("Pro 功能"),
                                     QStringLiteral("拟人化为 Pro 功能，激活后解锁。"));
            return;
        }
        m_session->setHumanizerEnabled(checked);
        syncFromProfile();
    });
    connect(m_statsOverlayCheck, &QCheckBox::toggled, this, [this](bool checked) {
        if (m_syncing) {
            return;
        }
        QSettings().setValue(QStringLiteral("display/statsOverlay"), checked);
        emit statsOverlayChanged(checked);
    });
    if (m_session) {
        // 投屏页工具栏也有息屏开关：两处双向同步。
        connect(m_session, &Session::screenOffChanged, this, [this](bool off) {
            if (!m_screenOffCheck) {
                return;
            }
            m_syncing = true;
            m_screenOffCheck->setChecked(off);
            m_syncing = false;
        });
    }
    connect(activateButton, &QPushButton::clicked, this, [this] {
        LicenseDialog dialog(this);
        connect(&dialog, &LicenseDialog::licenseChanged, this, [this] {
            if (m_session) {
                // 激活后按新授权重算注入侧的拟人化/帧率门控，无需用户再操作。
                m_session->refreshLicense();
            }
            syncFromProfile();
            emit licenseChanged();
        });
        dialog.exec();
        syncFromProfile();
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
            vk = 0x05; // VK_XBUTTON1（上方侧键）
        } else if (mouseEvent->button() == Qt::ForwardButton) {
            vk = 0x06; // VK_XBUTTON2（下方侧键）
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

    syncResolution(m_resolutionCombo, profile);
    m_renderModeCombo->setCurrentIndex(m_session->qualityPriority() ? 1 : 0);
    m_qualityCodecCombo->setCurrentIndex(m_qualityCodecCombo->findData(m_session->qualityCodec()));
    m_qualityCodecCombo->setEnabled(m_session->qualityPriority());
    m_codecStatus->setText(m_session->videoNotice().isEmpty()
                              ? QStringLiteral("默认使用 H.264；H.265 需要在当前设备连接后验证。")
                              : m_session->videoNotice());
    m_retryCodecButton->setVisible(m_session->qualityPriority() && m_session->codecFallbackActive());
    m_verticalSyncCheck->setChecked(verticalSyncEnabled(QSettings()));
    m_fpsCombo->setCurrentIndex(profile.maxFps >= 120 ? 3
                              : profile.maxFps >= 90 ? 2
                              : profile.maxFps >= 60 ? 1
                                                     : 0);
    syncBitrate(m_bitrateCombo, profile.videoBitRate);
    m_audioCheck->setChecked(m_session->audioEnabled());
    m_screenOffCheck->setChecked(m_session->screenOff());
    m_statsOverlayCheck->setChecked(
        QSettings().value(QStringLiteral("display/statsOverlay"), true).toBool());
    m_humanizerCheck->setChecked(m_session->humanizerEnabled());

    const bool pro = LicenseManager::instance().isPro();
    if (m_humanizerStatus) {
        const bool enabled = m_session->humanizerEnabled();
        m_humanizerStatus->setText(
            enabled && pro ? QStringLiteral("已开启：触控抖动与随机时序")
                           : pro ? QStringLiteral("已关闭：可手动开启触控抖动与随机时序")
                                 : QStringLiteral("基础版不可用，激活 Pro 后可开启"));
    }
    QString licenseText = QStringLiteral("基础版 · 功能不受限时可正常游玩");
    if (pro) {
        licenseText = QStringLiteral("PRO · 永久有效");
        const QString licenseId = LicenseManager::instance().licenseId();
        if (!licenseId.isEmpty()) {
            licenseText += QStringLiteral(" · %1").arg(licenseId);
        }
    }
    m_licenseLabel->setText(licenseText);
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
    static const int fpsValues[] = {30, 60, 90, 120};
    readResolution(m_resolutionCombo, profile);
    profile.maxFps = fpsValues[std::clamp(m_fpsCombo->currentIndex(), 0, 3)];
    profile.videoBitRate = m_bitrateCombo->currentData().toInt();
    m_session->applyProfile(profile, true);
    if (m_session->state() == Session::State::Streaming && !m_session->connectedSerial().isEmpty()) {
        m_session->connectDevice(m_session->connectedSerial());
    }
}

} // namespace ks::ui
