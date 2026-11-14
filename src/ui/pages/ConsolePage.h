#pragma once

#include <QVector>
#include <QWidget>

#include "core/engine/Engines.h"

class QComboBox;
class QDoubleSpinBox;
class QButtonGroup;
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

    /// 全屏信息悬浮开关（码率/帧率/延迟，设置页 → MainWindow 转发）。
    void setStatsOverlayEnabled(bool enabled);
    /// 疾跑状态变化（Session 主线程拦截疾跑键后转发）：游戏态 HUD 提示。
    void onSprintToggled(bool active);
    /// 授权状态变化：立即刷新画面增强可用性与帧率选项。
    void onLicenseChanged();

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
    void layoutVideoOverlays();
    void updateStatsOverlay();

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
    /// 画布触控是否可用：指针模式，或窗口态（非全屏）映射模式。
    bool canvasTouchActive() const;
    void cancelTouchDrag();
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
    void markKeymapDirty();
    void setRegionTarget(int region);
    void updateMouseModeButton();
    void addKeymapBinding();
    void deleteKeymapBinding();
    void createKeymapProfile();
    void importKeymapProfile();
    void exportKeymapProfile();
    void startKeyCapture();
    void stopKeyCapture();
    void applyBindingFromUi();
    int selectedBinding() const;
    /// 当前方案是否把该滚轮方向绑定为按键。
    bool wheelBound(int delta) const;

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
    QToolButton *m_mouseButton = nullptr;
    QPushButton *m_toolbarFullscreenButton = nullptr;

    // 视频区
    VideoWidget *m_video = nullptr;
    KeymapOverlay *m_overlay = nullptr;
    QLabel *m_hud = nullptr;
    QLabel *m_summonHint = nullptr;
    QLabel *m_statsOverlay = nullptr;

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
    QPushButton *m_addBindingButton = nullptr;
    QPushButton *m_saveKeymapButton = nullptr;
    QPushButton *m_exitEditButton = nullptr;
    QPushButton *m_cancelKeyCaptureButton = nullptr;
    QSlider *m_sensitivitySlider = nullptr;
    QDoubleSpinBox *m_sensitivitySpin = nullptr;
    QLabel *m_sizeLabel = nullptr;
    QSlider *m_sizeSlider = nullptr;
    QWidget *m_sprintSizeRow = nullptr;
    QSlider *m_sprintSizeSlider = nullptr;
    QPushButton *m_keyCaptureButton = nullptr;
    QSpinBox *m_xSpin = nullptr;
    QSpinBox *m_ySpin = nullptr;
    QPushButton *m_deleteBindingButton = nullptr;

    // 状态栏
    QLabel *m_statusAdb = nullptr;
    QLabel *m_statusBitrate = nullptr;
    QLabel *m_statusLatency = nullptr;
    QLabel *m_statusCodec = nullptr;

    QTimer *m_hudTimer = nullptr;
    QTimer *m_summonHintTimer = nullptr;
    QTimer *m_summonHoldTimer = nullptr;
    QTimer *m_keyCaptureTimer = nullptr;

    InputMode m_mode = InputMode::Idle;
    bool m_active = false;
    bool m_gameMode = false;
    bool m_draggingTouch = false;
    QPoint m_lastTouchPos;
    bool m_summonHeld = false;
    bool m_summonHintShown = false;
    SummonMode m_summonMode = SummonMode::LongPress;
    int m_summonKey = 0x12; ///< VK_MENU
    int m_summonHoldMs = 300;

    Profile m_keymapProfile;
    int m_keymapSelected = -1;
    int m_regionTarget = 0; ///< 0=无，1=轮盘，2=视角触发区（由画布点选）
    bool m_keymapDirty = false;
    bool m_capturingKey = false;
    bool m_syncingBindingUi = false; ///< 属性面板回填 UI 时屏蔽 applyBindingFromUi
    bool m_statsOverlayEnabled = true;
};

} // namespace ks::ui
