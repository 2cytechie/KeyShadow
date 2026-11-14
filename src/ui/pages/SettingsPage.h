#pragma once

#include <QWidget>

class QCheckBox;
class QComboBox;
class QButtonGroup;
class QLabel;
class QPushButton;
class QShowEvent;
class QTimer;

namespace ks {
class Session;
}

namespace ks::ui {

/// 设置页：视频 / 音频 / 输入 / 关于，修改即时生效并自动保存。
class SettingsPage : public QWidget {
    Q_OBJECT

public:
    explicit SettingsPage(Session *session, QWidget *parent = nullptr);

signals:
    void summonKeyChanged(int vk);
    void summonModeChanged(const QString &mode);
    void statsOverlayChanged(bool enabled);
    void sprintKeyChanged(int vk);
    void sprintModeChanged(const QString &mode);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void showEvent(QShowEvent *event) override;

private:
    enum class ListenTarget { Summon, Sprint };

    QWidget *buildSection(const QString &title);
    void addRow(QWidget *section, const QString &label, QWidget *control);
    void syncFromProfile();
    void storeProfile();

    void startListen(ListenTarget target);
    void stopListen();
    void handleListenCandidate(int vk);
    void refreshListenButtons();

    Session *m_session = nullptr;
    QComboBox *m_resolutionCombo = nullptr;
    QComboBox *m_fpsCombo = nullptr;
    QComboBox *m_bitrateCombo = nullptr;
    QCheckBox *m_audioCheck = nullptr;
    QCheckBox *m_screenOffCheck = nullptr;
    QCheckBox *m_statsOverlayCheck = nullptr;
    QCheckBox *m_humanizerCheck = nullptr;
    QLabel *m_humanizerStatus = nullptr;
    QLabel *m_licenseLabel = nullptr;
    QPushButton *m_summonButton = nullptr;
    QPushButton *m_summonModeLongPress = nullptr;
    QPushButton *m_summonModeHold = nullptr;
    QButtonGroup *m_summonModeGroup = nullptr;
    QLabel *m_summonStatus = nullptr;
    QPushButton *m_sprintButton = nullptr;
    QPushButton *m_sprintModeSwitch = nullptr;
    QPushButton *m_sprintModeHold = nullptr;
    QButtonGroup *m_sprintModeGroup = nullptr;
    QTimer *m_listenTimer = nullptr;
    int m_summonKey = 0x12;
    int m_sprintKey = 0x10;
    ListenTarget m_listenTarget = ListenTarget::Summon;
    bool m_listening = false;
    bool m_captureWasEnabled = false;
    bool m_syncing = false;
};

} // namespace ks::ui
