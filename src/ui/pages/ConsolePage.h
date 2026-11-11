#pragma once

#include <QVector>
#include <QWidget>

#include "core/engine/Engines.h"

class QCheckBox;
class QComboBox;
class QFrame;
class QHBoxLayout;
class QLabel;
class QPushButton;
class QSlider;
class QSpinBox;
class QStackedWidget;
class QTimer;
class QToolButton;
class QVBoxLayout;

namespace ks {
class Session;
}

namespace ks::ui {

class KeymapOverlay;
class TabStrip;
class VideoWidget;

/// 投屏工作台：工具栏 + 视频画布（键位叠加编辑）+ 检查器标签（画质/键位）+ 状态栏。
class ConsolePage : public QWidget {
    Q_OBJECT

public:
    /// 输入三态：空闲 / 映射 / 指针 / 键位编辑，互斥切换。
    enum class InputMode { Idle, Mapping, Pointer, Edit };
    /// 呼出方式：长按 300ms 呼出 / 按住呼出、松开返回。
    enum class SummonMode { LongPress, Hold };

    explicit ConsolePage(Session *session, QWidget *parent = nullptr);
    ~ConsolePage() override;

    void setGameMode(bool enabled);
    bool isGameMode() const { return m_gameMode; }
    bool isEditing() const { return m_mode == InputMode::Edit; }
    void setBypassEnhance(bool bypass);

    /// 投屏页激活/停用：停用时暂停全部输入捕获。
    void setActive(bool active);
    /// 离开投屏页前调用；键位编辑有未保存修改时可被用户取消（返回 false）。
    bool requestDeactivate();

    void setSummonKey(int vk);
    void setSummonMode(const QString &mode);
    void onSummonKeyPressed(bool down);
    void beginKeymapEdit();

signals:
    void requestExitGameMode();
    void toggleGameMode();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private:
    struct QualityRow {
        QString key;
        QSlider *slider = nullptr;
        QLabel *value = nullptr;
        QString suffix;
    };

    void buildToolbar(QVBoxLayout *rootLayout);
    void buildVideoArea(QHBoxLayout *centerLayout);
    void buildInspector(QHBoxLayout *centerLayout);
    QWidget *buildQualityTab();
    QWidget *buildKeymapTab();
    void buildStatusBar(QVBoxLayout *rootLayout);
    void buildFloatingButtons();
    void layoutVideoOverlays();

    void refreshProfileToUi();
    void refreshQualityToUi();
    void refreshQualityAvailability();
    void updateStatsUi();
    void applyVideoOptions();
    void resetQuality();

    void setInputMode(InputMode mode);
    void updateInputCapture();
    void enterPointerMode();
    void exitPointerMode();
    bool mapToDevice(const QPoint &widgetPos, QPoint *devicePos) const;
    void sendPointerTouch(int action, const QPoint &widgetPos, bool clampInside);
    QSize deviceSize() const;

    void beginKeymapEditInternal();
    void exitKeymapEdit(bool save);
    void saveKeymap();
    bool confirmLeaveEdit();
    bool switchInspectorTab(int index);
    void setKeymapEditUi(bool editing);
    void reloadKeymapProfiles();
    void loadKeymapProfile(const QString &name);
    void refreshKeymapSummary();
    void refreshKeymapProperties();
    void updateKeymapOverlaps();
    void markKeymapDirty();
    void addKeymapBinding();
    void deleteKeymapBinding();
    void createKeymapProfile();
    void importKeymapProfile();
    void exportKeymapProfile();
    void startKeyCapture();
    void stopKeyCapture();
    void applyBindingFromUi();
    int selectedBinding() const;
    void openCalibration();

    void showHud(const QString &text, bool persistent = false);
    void hideHudSoon();
    QString pointerHudText() const;
    void showSummonHint();
    void updateSummonText();
    void updateCursor();

    Session *m_session = nullptr;

    // 工作台容器
    QFrame *m_toolbar = nullptr;
    QFrame *m_statusBar = nullptr;

    // 工具栏
    QLabel *m_deviceCapsule = nullptr;
    QLabel *m_fpsLabel = nullptr;
    QLabel *m_latencyLabel = nullptr;
    QComboBox *m_resolutionCombo = nullptr;
    QComboBox *m_fpsCombo = nullptr;
    QComboBox *m_bitrateCombo = nullptr;

    // 视频区
    VideoWidget *m_video = nullptr;
    KeymapOverlay *m_overlay = nullptr;
    QLabel *m_hud = nullptr;
    QLabel *m_summonHint = nullptr;
    QWidget *m_floatingButtons = nullptr;
    QToolButton *m_mouseButton = nullptr;
    QToolButton *m_fullscreenButton = nullptr;

    // 检查器
    QFrame *m_inspector = nullptr;
    TabStrip *m_tabs = nullptr;
    QStackedWidget *m_inspectorStack = nullptr;

    QWidget *m_qualityPanel = nullptr;
    QVector<QualityRow> m_qualityRows;
    QLabel *m_qualityInfo = nullptr;
    QLabel *m_qualityLockHint = nullptr;
    QPushButton *m_compareButton = nullptr;
    QToolButton *m_qualityResetButton = nullptr;
    bool m_updatingQuality = false;

    QWidget *m_keymapPanel = nullptr;
    QWidget *m_browseBox = nullptr;
    QWidget *m_editBox = nullptr;
    QComboBox *m_profileCombo = nullptr;
    QPushButton *m_newProfileButton = nullptr;
    QPushButton *m_importProfileButton = nullptr;
    QPushButton *m_exportProfileButton = nullptr;
    QLabel *m_keymapSummary = nullptr;
    QPushButton *m_editKeymapButton = nullptr;
    QPushButton *m_calibrateButton = nullptr;
    QCheckBox *m_gridCheck = nullptr;
    QCheckBox *m_snapCheck = nullptr;
    QPushButton *m_addBindingButton = nullptr;
    QPushButton *m_saveKeymapButton = nullptr;
    QPushButton *m_exitEditButton = nullptr;
    QPushButton *m_keyCaptureButton = nullptr;
    QSpinBox *m_xSpin = nullptr;
    QSpinBox *m_ySpin = nullptr;
    QSlider *m_sizeSlider = nullptr;
    QComboBox *m_actionCombo = nullptr;
    QPushButton *m_deleteBindingButton = nullptr;
    QLabel *m_overlapLabel = nullptr;

    // 状态栏
    QLabel *m_statusAdb = nullptr;
    QLabel *m_statusBitrate = nullptr;
    QLabel *m_statusLatency = nullptr;
    QLabel *m_statusCodec = nullptr;

    QTimer *m_hudTimer = nullptr;
    QTimer *m_summonHintTimer = nullptr;
    QTimer *m_summonHoldTimer = nullptr;

    InputMode m_mode = InputMode::Idle;
    bool m_active = false;
    bool m_gameMode = false;
    bool m_draggingTouch = false;
    bool m_summonHeld = false;
    bool m_summonHintShown = false;
    SummonMode m_summonMode = SummonMode::LongPress;
    int m_summonKey = 0x12; ///< VK_MENU
    int m_summonHoldMs = 300;

    Profile m_keymapProfile;
    int m_keymapSelected = -1;
    bool m_keymapDirty = false;
    bool m_capturingKey = false;
};

} // namespace ks::ui
