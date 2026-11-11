#pragma once

#include <QWidget>

class QCheckBox;
class QComboBox;
class QLabel;
class QPushButton;
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

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    QWidget *buildSection(const QString &title);
    void addRow(QWidget *section, const QString &label, QWidget *control);
    void syncFromProfile();
    void storeProfile();

    void startSummonListen();
    void stopSummonListen();
    void handleSummonCandidate(int vk);
    void refreshSummonButton();
    void updateSummonModeHint();

    Session *m_session = nullptr;
    QComboBox *m_resolutionCombo = nullptr;
    QComboBox *m_fpsCombo = nullptr;
    QComboBox *m_bitrateCombo = nullptr;
    QCheckBox *m_audioCheck = nullptr;
    QCheckBox *m_screenOffCheck = nullptr;
    QCheckBox *m_humanizerCheck = nullptr;
    QLabel *m_licenseLabel = nullptr;
    QPushButton *m_summonButton = nullptr;
    QComboBox *m_summonModeCombo = nullptr;
    QLabel *m_summonModeHint = nullptr;
    QLabel *m_summonStatus = nullptr;
    QTimer *m_summonTimer = nullptr;
    int m_summonKey = 0x12;
    bool m_listening = false;
    bool m_captureWasEnabled = false;
    bool m_syncing = false;
};

} // namespace ks::ui
