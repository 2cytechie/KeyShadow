#include "ui/pages/SettingsPage.h"

#include "app/Session.h"
#include "license/LicenseManager.h"
#include "ui/KeyNames.h"
#include "ui/Theme.h"

#include <QApplication>
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
                          QStringLiteral("120 fps（Pro）")});
    addRow(videoSection, QStringLiteral("默认帧率"), m_fpsCombo);

    m_bitrateCombo = new QComboBox(videoSection);
    m_bitrateCombo->addItems({QStringLiteral("4 Mbps"), QStringLiteral("8 Mbps"),
                              QStringLiteral("12 Mbps"), QStringLiteral("16 Mbps"),
                              QStringLiteral("20 Mbps")});
    addRow(videoSection, QStringLiteral("码率上限"), m_bitrateCombo);

    m_screenOffCheck = new QCheckBox(QStringLiteral("开启息屏投屏（Pro）"), videoSection);
    addRow(videoSection, QStringLiteral("息屏投屏"), m_screenOffCheck);
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

    auto *summonRow = new QWidget(inputSection);
    auto *summonLayout = new QHBoxLayout(summonRow);
    summonLayout->setContentsMargins(0, 0, 0, 0);
    summonLayout->setSpacing(10);

    m_summonButton = new QPushButton(summonRow);
    m_summonButton->setProperty("role", "summonKey");
    m_summonButton->setFixedSize(180, 34);
    m_summonButton->setFont(Theme::monoFont(12));
    m_summonButton->setCursor(Qt::PointingHandCursor);

    auto *summonHint = new QLabel(summonRow);
    summonHint->setProperty("variant", "micro");
    m_summonModeHint = summonHint;
    summonLayout->addWidget(m_summonButton);
    summonLayout->addWidget(summonHint);
    summonLayout->addStretch(1);
    addRow(inputSection, QStringLiteral("呼出鼠标"), summonRow);

    m_summonModeCombo = new QComboBox(inputSection);
    m_summonModeCombo->addItem(QStringLiteral("长按 300ms 呼出"), QStringLiteral("longPress"));
    m_summonModeCombo->addItem(QStringLiteral("按下呼出，抬起返回"), QStringLiteral("hold"));
    addRow(inputSection, QStringLiteral("呼出方式"), m_summonModeCombo);

    m_summonStatus = new QLabel(inputSection);
    m_summonStatus->setProperty("state", "warning");
    m_summonStatus->setWordWrap(true);
    addRow(inputSection, QString(), m_summonStatus);

    m_humanizerCheck = new QCheckBox(QStringLiteral("开启触控拟人化"), inputSection);
    addRow(inputSection, QStringLiteral("拟人化"), m_humanizerCheck);

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
            [this](int) { storeProfile(); });
    connect(m_bitrateCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this](int) { storeProfile(); });
    connect(m_audioCheck, &QCheckBox::toggled, this, [this](bool) { storeProfile(); });
    connect(m_screenOffCheck, &QCheckBox::toggled, this, [this](bool checked) {
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
    connect(m_humanizerCheck, &QCheckBox::toggled, this, [this](bool checked) {
        Profile profile = m_session->profile();
        profile.humanizerEnabled = checked;
        m_session->applyProfile(profile, true);
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
    });

    QSettings settings;
    m_summonKey = settings.value(QStringLiteral("input/summonKey"), 0x12).toInt();
    m_summonModeCombo->setCurrentIndex(
        settings.value(QStringLiteral("input/summonMode"), QStringLiteral("longPress"))
                    .toString() == QStringLiteral("hold")
            ? 1
            : 0);
    updateSummonModeHint();
    connect(m_summonModeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this](int) {
                const QString mode = m_summonModeCombo->currentData().toString();
                QSettings().setValue(QStringLiteral("input/summonMode"), mode);
                updateSummonModeHint();
                emit summonModeChanged(mode);
            });
    m_summonTimer = new QTimer(this);
    m_summonTimer->setSingleShot(true);
    m_summonTimer->setInterval(5000);
    connect(m_summonTimer, &QTimer::timeout, this, [this] {
        stopSummonListen();
        m_summonStatus->setText(QStringLiteral("已取消改绑（5 秒无输入）"));
    });
    connect(m_summonButton, &QPushButton::clicked, this, &SettingsPage::startSummonListen);
    refreshSummonButton();

    syncFromProfile();
}

void SettingsPage::refreshSummonButton() {
    if (!m_summonButton) {
        return;
    }
    m_summonButton->setText(m_listening ? QStringLiteral("请按下新按键…（Esc 取消）")
                                        : vkName(m_summonKey));
    m_summonButton->setProperty("listening", m_listening);
    m_summonButton->style()->unpolish(m_summonButton);
    m_summonButton->style()->polish(m_summonButton);
}

void SettingsPage::updateSummonModeHint() {
    if (!m_summonModeHint || !m_summonModeCombo) {
        return;
    }
    const bool hold = m_summonModeCombo->currentData().toString() == QStringLiteral("hold");
    m_summonModeHint->setText(hold ? QStringLiteral("按住呼出，松开返回")
                                   : QStringLiteral("长按 300ms 呼出"));
}

void SettingsPage::startSummonListen() {
    if (m_listening || !m_session) {
        return;
    }
    m_listening = true;
    m_captureWasEnabled = m_session->inputCaptureEnabled();
    m_session->setInputCaptureEnabled(false);
    m_summonStatus->clear();
    refreshSummonButton();
    qApp->installEventFilter(this);
    m_summonTimer->start();
}

void SettingsPage::stopSummonListen() {
    if (!m_listening) {
        return;
    }
    m_listening = false;
    qApp->removeEventFilter(this);
    m_summonTimer->stop();
    refreshSummonButton();
    if (m_session) {
        m_session->setInputCaptureEnabled(m_captureWasEnabled);
    }
}

void SettingsPage::handleSummonCandidate(int vk) {
    if (isForbiddenSummonKey(vk)) {
        if (vk == 0x01) {
            m_summonStatus->setText(QStringLiteral("鼠标左键用于开火映射，不能作为呼出键"));
        } else {
            m_summonStatus->setText(
                QStringLiteral("该键已被占用：Esc / F9 / F10 / F11 / Win 不可作为呼出键"));
        }
        return;
    }
    m_summonKey = vk;
    QSettings().setValue(QStringLiteral("input/summonKey"), vk);
    stopSummonListen();
    m_summonStatus->clear();
    emit summonKeyChanged(vk);
}

bool SettingsPage::eventFilter(QObject *watched, QEvent *event) {
    if (!m_listening) {
        return QWidget::eventFilter(watched, event);
    }
    if (event->type() == QEvent::KeyPress) {
        auto *keyEvent = static_cast<QKeyEvent *>(event);
        if (keyEvent->key() == Qt::Key_Escape) {
            stopSummonListen();
            m_summonStatus->setText(QStringLiteral("已取消改绑"));
            return true;
        }
        if (keyEvent->isAutoRepeat()) {
            return true;
        }
        handleSummonCandidate(vkFromKeyEvent(*keyEvent));
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
        }
        if (vk) {
            handleSummonCandidate(vk);
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

void SettingsPage::syncFromProfile() {
    m_syncing = true;
    const Profile &profile = m_session->profile();

    m_resolutionCombo->setCurrentIndex(profile.maxSize >= 1080 ? 1
                                         : profile.maxSize >= 720 ? 2
                                         : profile.maxSize >= 480 ? 3
                                                                  : 0);
    m_fpsCombo->setCurrentIndex(profile.maxFps >= 120 ? 2 : profile.maxFps >= 60 ? 1 : 0);
    const int mbps = std::max(1, profile.videoBitRate / 1'000'000);
    m_bitrateCombo->setCurrentIndex(mbps <= 4 ? 0 : mbps <= 8 ? 1 : mbps <= 12 ? 2
                                                                  : mbps <= 16 ? 3
                                                                               : 4);
    m_audioCheck->setChecked(profile.audioEnabled);
    m_screenOffCheck->setChecked(profile.screenOff);
    m_humanizerCheck->setChecked(profile.humanizerEnabled);

    const bool pro = LicenseManager::instance().isPro();
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
    static const int fpsValues[] = {30, 60, 120};
    static const int bitrates[] = {4, 8, 12, 16, 20};
    profile.maxSize = resolutions[std::clamp(m_resolutionCombo->currentIndex(), 0, 3)];
    profile.maxFps = fpsValues[std::clamp(m_fpsCombo->currentIndex(), 0, 2)];
    profile.videoBitRate = bitrates[std::clamp(m_bitrateCombo->currentIndex(), 0, 4)] * 1'000'000;
    profile.audioEnabled = m_audioCheck->isChecked();
    m_session->applyProfile(profile, true);
}

} // namespace ks::ui
