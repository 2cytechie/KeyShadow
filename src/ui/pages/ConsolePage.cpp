#include "ui/pages/ConsolePage.h"

#include "app/Session.h"
#include "core/scrcpy/Protocol.h"
#include "license/LicenseManager.h"
#include "ui/IconFactory.h"
#include "ui/KeyNames.h"
#include "ui/KeymapOverlay.h"
#include "ui/TabStrip.h"
#include "ui/Theme.h"
#include "ui/VideoWidget.h"

#include <QApplication>
#include <QButtonGroup>
#include <QCursor>
#include <QDebug>
#include <QComboBox>
#include <QDialog>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPushButton>
#include <QScrollArea>
#include <QSettings>
#include <QSignalBlocker>
#include <QSlider>
#include <QSpinBox>
#include <QStackedWidget>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

namespace ks::ui {
namespace {

QLabel *makeMonoLabel(QWidget *parent, const QString &text, int widthChars) {
    auto *label = new QLabel(text, parent);
    label->setProperty("variant", "mono");
    label->setFont(Theme::monoFont(12));
    label->setMinimumWidth(widthChars * 8);
    label->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    return label;
}

QString formatQualityValue(const QString &key, int value, const QString &suffix) {
    if (key == QStringLiteral("gamma")) {
        return QString::number(value / 100.0, 'f', 2);
    }
    if (key == QStringLiteral("brightness") || key == QStringLiteral("contrast") ||
        key == QStringLiteral("saturation")) {
        return value > 0 ? QStringLiteral("+%1").arg(value) : QString::number(value);
    }
    return QStringLiteral("%1%2").arg(value).arg(suffix);
}

void normalizeLookProfile(Profile &profile) {
    profile.lookRadius = std::clamp(profile.lookRadius, 0.05, 0.45);
    profile.lookCenter = clampNormalizedRegionCenter(profile.lookCenter, profile.lookRadius);
}

QPushButton *makeToolButton(QWidget *parent, const QString &text, const char *variant = nullptr) {
    auto *button = new QPushButton(text, parent);
    if (variant) {
        button->setProperty("variant", variant);
    }
    return button;
}

} // namespace

ConsolePage::ConsolePage(Session *session, QWidget *parent) : QWidget(parent), m_session(session) {
    setObjectName(QStringLiteral("ConsolePage"));

    QSettings settings;
    m_summonKey = settings.value(QStringLiteral("input/summonKey"), 0x12).toInt();
    m_summonHoldMs = settings.value(QStringLiteral("input/summonHoldMs"), 300).toInt();
    m_summonMode = settings.value(QStringLiteral("input/summonMode"), QStringLiteral("longPress"))
                           .toString() == QStringLiteral("hold")
                       ? SummonMode::Hold
                       : SummonMode::LongPress;
    m_statsOverlayEnabled = settings.value(QStringLiteral("display/statsOverlay"), true).toBool();

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    buildToolbar(root);

    auto *center = new QHBoxLayout();
    center->setContentsMargins(0, 0, 0, 0);
    center->setSpacing(0);

    buildVideoArea(center);
    buildInspector(center);
    root->addLayout(center, 1);

    buildStatusBar(root);

    m_hudTimer = new QTimer(this);
    m_hudTimer->setSingleShot(true);
    connect(m_hudTimer, &QTimer::timeout, this, [this] { m_hud->hide(); });

    m_summonHintTimer = new QTimer(this);
    m_summonHintTimer->setSingleShot(true);
    connect(m_summonHintTimer, &QTimer::timeout, this, [this] { m_summonHint->hide(); });

    m_summonHoldTimer = new QTimer(this);
    m_summonHoldTimer->setSingleShot(true);
    m_summonHoldTimer->setInterval(m_summonHoldMs);
    connect(m_summonHoldTimer, &QTimer::timeout, this, [this] {
        if (m_summonHeld) {
            enterPointerMode();
        }
    });

    m_keyCaptureTimer = new QTimer(this);
    m_keyCaptureTimer->setSingleShot(true);
    m_keyCaptureTimer->setInterval(5000);
    connect(m_keyCaptureTimer, &QTimer::timeout, this, [this] {
        stopKeyCapture();
        showHud(QStringLiteral("按键监听已超时"));
    });

    m_qualityPanel->installEventFilter(this);

    if (m_session) {
        connect(m_session, &Session::frameReady, this,
                [this](const VideoFrameConstPtr &frame) {
                    m_video->setFrame(frame);
                    if (m_mode == InputMode::Edit) {
                        m_overlay->setEmptyHint(QString());
                    }
                });
        connect(m_session, &Session::statsChanged, this, [this] { updateStatsUi(); });
        connect(m_session, &Session::stateChanged, this,
                [this](Session::State state, const QString &message) {
                    const bool streaming = state == Session::State::Streaming;
                    m_deviceCapsule->setText(streaming ? QStringLiteral("●  %1")
                                                              .arg(m_session->connectedDeviceName())
                                                        : QStringLiteral("○  %1").arg(message));
                    m_deviceCapsule->setProperty("state",
                                                 streaming ? "success"
                                                 : state == Session::State::Error ? "danger"
                                                                                  : "secondary");
                    m_deviceCapsule->style()->unpolish(m_deviceCapsule);
                    m_deviceCapsule->style()->polish(m_deviceCapsule);

                    if (streaming) {
                        if (m_active && m_mode != InputMode::Edit) {
                            // 默认模式：全屏=游戏映射，窗口=鼠标指针（S5）。
                            setInputMode(m_gameMode ? InputMode::Mapping
                                                    : InputMode::Pointer);
                        }
                        if (!m_summonHintShown) {
                            showSummonHint();
                        }
                        if (m_mode == InputMode::Edit && m_video->hasFrame()) {
                            m_overlay->setEmptyHint(QString());
                        }
                        if (m_keymapDirty) {
                            showHud(QStringLiteral("有未保存的键位修改，可继续编辑后保存"));
                        }
                    } else {
                        m_video->clearFrame();
                        const QSize fallback = deviceSize();
                        m_video->setFallbackAspect(fallback.width() /
                                                   static_cast<double>(fallback.height()));
                        if (m_mode == InputMode::Edit) {
                            // 保留未保存键位：同步叠加层 → 内存方案，退出编辑但不清理 dirty，
                            // 重新连接/再次进入编辑后可继续保存（R2/04 §5.4）。
                            m_keymapProfile = m_overlay->profile();
                            m_keymapSelected = -1;
                            m_overlay->setSelected(-1);
                            setKeymapEditUi(false);
                            showHud(QStringLiteral("设备已断开，键位修改已保留，重新连接后可继续保存"));
                        }
                        setInputMode(InputMode::Idle);
                        m_summonHeld = false;
                        m_summonHoldTimer->stop();
                    }
                    updateStatsOverlay();
                });
        connect(m_session, &Session::profileChanged, this,
                [this](const Profile &) { refreshProfileToUi(); });
    }

    m_keymapProfile = m_session->profile();
    reloadKeymapProfiles();
    refreshProfileToUi();
    refreshKeymapSummary();
    refreshKeymapProperties();
    updateSummonText();
    setKeymapEditUi(false);
}

ConsolePage::~ConsolePage() {
    if (m_capturingKey) {
        qApp->removeEventFilter(this);
    }
}

// ---------------------------------------------------------------------------
// 构建：工具栏
// ---------------------------------------------------------------------------
void ConsolePage::buildToolbar(QVBoxLayout *rootLayout) {
    m_toolbar = new QFrame(this);
    m_toolbar->setObjectName(QStringLiteral("Toolbar"));
    m_toolbar->setFixedHeight(48);

    auto *layout = new QHBoxLayout(m_toolbar);
    layout->setContentsMargins(12, 8, 12, 8);
    layout->setSpacing(10);

    m_deviceCapsule = new QLabel(QStringLiteral("○  未连接"), m_toolbar);
    m_deviceCapsule->setObjectName(QStringLiteral("DeviceCapsule"));
    layout->addWidget(m_deviceCapsule);

    auto makeCombo = [this](const QStringList &items, int current) {
        auto *combo = new QComboBox(m_toolbar);
        combo->addItems(items);
        combo->setCurrentIndex(current);
        combo->setFixedWidth(96);
        return combo;
    };

    m_resolutionCombo = makeCombo({QStringLiteral("原生"), QStringLiteral("1080p"),
                                   QStringLiteral("720p"), QStringLiteral("480p")},
                                  0);
    m_fpsCombo = makeCombo({QStringLiteral("30 fps"), QStringLiteral("60 fps"),
                            QStringLiteral("90 fps · PRO"), QStringLiteral("120 fps · PRO")},
                           1);
    m_bitrateCombo = makeCombo({QStringLiteral("4 Mbps"), QStringLiteral("8 Mbps"),
                                QStringLiteral("12 Mbps"), QStringLiteral("16 Mbps"),
                                QStringLiteral("24 Mbps"), QStringLiteral("32 Mbps")},
                               1);

    layout->addWidget(m_resolutionCombo);
    layout->addWidget(m_fpsCombo);
    layout->addWidget(m_bitrateCombo);
    layout->addStretch(1);

    layout->addWidget(new QLabel(QStringLiteral("FPS"), m_toolbar));
    m_fpsLabel = makeMonoLabel(m_toolbar, QStringLiteral("--"), 4);
    layout->addWidget(m_fpsLabel);

    layout->addSpacing(8);
    layout->addWidget(new QLabel(QStringLiteral("延迟"), m_toolbar));
    m_latencyLabel = makeMonoLabel(m_toolbar, QStringLiteral("-- ms"), 7);
    layout->addWidget(m_latencyLabel);

    m_toolbarFullscreenButton = new QPushButton(m_toolbar);
    m_toolbarFullscreenButton->setIcon(
        IconFactory::icon(QStringLiteral("maximize"), Theme::textSecondary()));
    m_toolbarFullscreenButton->setProperty("variant", "ghost");
    m_toolbarFullscreenButton->setToolTip(QStringLiteral("全屏游戏模式（F11）"));
    m_toolbarFullscreenButton->setFixedSize(32, 28);
    layout->addWidget(m_toolbarFullscreenButton);
    connect(m_toolbarFullscreenButton, &QPushButton::clicked, this, [this] {
        if (m_mode == InputMode::Edit) {
            showHud(QStringLiteral("请先退出键位编辑再进入全屏"));
            return;
        }
        emit toggleGameMode();
    });

    // 鼠标模式切换：入工具栏，不再悬浮画布（避免挡住键位）。
    m_mouseButton = new QToolButton(m_toolbar);
    m_mouseButton->setObjectName(QStringLiteral("MouseModeButton"));
    m_mouseButton->setIcon(IconFactory::icon(QStringLiteral("mouse"), Theme::textSecondary(), 16));
    m_mouseButton->setIconSize(QSize(16, 16));
    m_mouseButton->setFixedSize(28, 28);
    m_mouseButton->setCheckable(true);
    m_mouseButton->setToolTip(QStringLiteral("鼠标指针模式"));
    m_mouseButton->setCursor(Qt::PointingHandCursor);
    layout->addWidget(m_mouseButton);
    connect(m_mouseButton, &QToolButton::clicked, this, [this](bool checked) {
        if (checked) {
            enterPointerMode();
            if (m_mode != InputMode::Pointer) {
                QSignalBlocker blocker(m_mouseButton);
                m_mouseButton->setChecked(false);
            }
        } else if (m_mode == InputMode::Pointer) {
            setInputMode(InputMode::Mapping);
        }
    });

    connect(m_resolutionCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this](int) { applyVideoOptions(); });
    connect(m_fpsCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this](int index) {
                if (index >= 2 && !LicenseManager::instance().isPro()) {
                    showHud(QStringLiteral("90/120fps 为付费功能，激活后解锁"));
                    QSignalBlocker blocker(m_fpsCombo);
                    m_fpsCombo->setCurrentIndex(1);
                    return;
                }
                applyVideoOptions();
            });
    connect(m_bitrateCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this](int) { applyVideoOptions(); });

    rootLayout->addWidget(m_toolbar);
}

// ---------------------------------------------------------------------------
// 构建：视频区
// ---------------------------------------------------------------------------
void ConsolePage::buildVideoArea(QHBoxLayout *centerLayout) {
    m_video = new VideoWidget(this);
    centerLayout->addWidget(m_video, 1);
    m_video->installEventFilter(this);

    m_overlay = new KeymapOverlay(m_video);
    m_overlay->hide();
    m_overlay->installEventFilter(this);

    connect(m_overlay, &KeymapOverlay::selectedChanged, this, [this](int index) {
        m_keymapSelected = index;
        if (index >= 0) {
            m_regionTarget = 0;
        }
        refreshKeymapProperties();
    });
    connect(m_overlay, &KeymapOverlay::regionSelected, this, [this](int region) {
        m_keymapSelected = -1;
        m_regionTarget = region;
        refreshKeymapProperties();
    });
    connect(m_overlay, &KeymapOverlay::regionMoved, this, [this](int) {
        m_keymapProfile = m_overlay->profile();
        refreshKeymapProperties();
        markKeymapDirty();
    });
    connect(m_overlay, &KeymapOverlay::regionResized, this, [this](int) {
        m_keymapProfile = m_overlay->profile();
        refreshKeymapProperties();
        markKeymapDirty();
    });
    connect(m_overlay, &KeymapOverlay::regionEdited, this, [this] {
        m_keymapProfile = m_overlay->profile();
        markKeymapDirty();
    });
    // 叠加层内部持有 Profile 副本：每次拖拽/缩放后同步回 ConsolePage，
    // 否则属性面板、重叠检测与保存都会使用旧坐标。
    connect(m_overlay, &KeymapOverlay::bindingMoved, this, [this](int) {
        m_keymapProfile = m_overlay->profile();
        refreshKeymapProperties();
            markKeymapDirty();
    });
    connect(m_overlay, &KeymapOverlay::bindingResized, this, [this](int) {
        m_keymapProfile = m_overlay->profile();
        refreshKeymapProperties();
            markKeymapDirty();
    });
    connect(m_overlay, &KeymapOverlay::bindingEdited, this, [this] {
        m_keymapProfile = m_overlay->profile();
        markKeymapDirty();
    });
    connect(m_overlay, &KeymapOverlay::deleteRequested, this, &ConsolePage::deleteKeymapBinding);
    connect(m_overlay, &KeymapOverlay::saveRequested, this, &ConsolePage::saveKeymap);
    connect(m_overlay, &KeymapOverlay::exitRequested, this, [this] {
        if (confirmLeaveEdit()) {
            exitKeymapEdit(false);
        }
    });

    m_hud = new QLabel(m_video);
    m_hud->setStyleSheet(QStringLiteral(
        "background: rgba(8,10,13,0.62); color: #E8EDF2; padding: 4px 10px; border-radius: 4px;"));
    m_hud->setAttribute(Qt::WA_TransparentForMouseEvents);
    m_hud->hide();

    m_summonHint = new QLabel(m_video);
    m_summonHint->setStyleSheet(QStringLiteral(
        "background: rgba(8,10,13,0.55); color: #9BA7B4; padding: 4px 10px; border-radius: 4px;"));
    m_summonHint->setAttribute(Qt::WA_TransparentForMouseEvents);
    m_summonHint->hide();

    // 全屏信息悬浮（码率 / 帧率 / 延迟，顶部居中，设置统一开关）
    m_statsOverlay = new QLabel(m_video);
    m_statsOverlay->setStyleSheet(QStringLiteral(
        "background: rgba(8,10,13,0.62); color: #E8EDF2; padding: 4px 12px; border-radius: 4px;"));
    m_statsOverlay->setFont(Theme::monoFont(13));
    m_statsOverlay->setAttribute(Qt::WA_TransparentForMouseEvents);
    m_statsOverlay->setText(QStringLiteral("-- FPS · -- Mbps · -- ms"));
    m_statsOverlay->hide();

    connect(m_video, &VideoWidget::doubleClicked, this, [this] {
        // 全屏下只允许 F11 / 双击 Esc 退出；编辑与画布直操态双击属于手机/编辑操作。
        if (m_gameMode || m_mode == InputMode::Edit || canvasTouchActive()) {
            return;
        }
        emit toggleGameMode();
    });
    connect(m_video, &VideoWidget::videoRectChanged, this,
            [this](const QRect &) { layoutVideoOverlays(); });
}

// ---------------------------------------------------------------------------
// 构建：检查器（画质 / 键位）
// ---------------------------------------------------------------------------
void ConsolePage::buildInspector(QHBoxLayout *centerLayout) {
    m_inspector = new QFrame(this);
    m_inspector->setObjectName(QStringLiteral("Inspector"));
    m_inspector->setFixedWidth(320);

    auto *layout = new QVBoxLayout(m_inspector);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    m_tabs = new TabStrip(m_inspector);
    m_tabs->addTab(QStringLiteral("画质"));
    m_tabs->addTab(QStringLiteral("键位"));
    layout->addWidget(m_tabs);

    m_inspectorStack = new QStackedWidget(m_inspector);
    layout->addWidget(m_inspectorStack, 1);

    m_qualityPanel = buildQualityTab();
    auto *qualityScroll = new QScrollArea(m_inspectorStack);
    qualityScroll->setWidgetResizable(true);
    qualityScroll->setFrameShape(QFrame::NoFrame);
    qualityScroll->setWidget(m_qualityPanel);
    m_inspectorStack->addWidget(qualityScroll);

    m_keymapPanel = buildKeymapTab();
    auto *keymapScroll = new QScrollArea(m_inspectorStack);
    keymapScroll->setWidgetResizable(true);
    keymapScroll->setFrameShape(QFrame::NoFrame);
    keymapScroll->setWidget(m_keymapPanel);
    m_inspectorStack->addWidget(keymapScroll);

    m_inspectorStack->setCurrentIndex(0);

    connect(m_tabs, &TabStrip::currentChanged, this, [this](int index) {
        if (!switchInspectorTab(index)) {
            QSignalBlocker blocker(m_tabs);
            m_tabs->setCurrentIndex(m_inspectorStack->currentIndex());
        }
    });

    centerLayout->addWidget(m_inspector);
}

QWidget *ConsolePage::buildQualityTab() {
    auto *panel = new QWidget();
    auto *layout = new QVBoxLayout(panel);
    layout->setContentsMargins(16, 16, 16, 16);
    layout->setSpacing(10);

    auto *titleRow = new QHBoxLayout();
    auto *title = new QLabel(QStringLiteral("画面增强"), panel);
    title->setObjectName(QStringLiteral("SectionTitle"));
    auto *proBadge = new QLabel(QStringLiteral("PRO"), panel);
    proBadge->setProperty("badge", "pro");
    titleRow->addWidget(title);
    titleRow->addWidget(proBadge);
    titleRow->addStretch(1);

    m_qualityResetButton = new QToolButton(panel);
    m_qualityResetButton->setIcon(
        IconFactory::icon(QStringLiteral("refresh"), Theme::textSecondary(), 16));
    m_qualityResetButton->setIconSize(QSize(16, 16));
    m_qualityResetButton->setFixedSize(24, 24);
    m_qualityResetButton->setToolTip(QStringLiteral("重置为方案默认值"));
    m_qualityResetButton->setCursor(Qt::PointingHandCursor);
    titleRow->addWidget(m_qualityResetButton);
    layout->addLayout(titleRow);

    const bool pro = LicenseManager::instance().isPro();
    if (!pro) {
        m_qualityLockHint = new QLabel(
            QStringLiteral("画面增强为 Pro 功能，激活后解锁。基础版可正常投屏与游玩。"), panel);
        m_qualityLockHint->setProperty("state", "warning");
        m_qualityLockHint->setWordWrap(true);
        layout->addWidget(m_qualityLockHint);
    }

    struct SliderSpec {
        QString key;
        QString title;
        int min;
        int max;
        int initial;
        QString suffix;
    };
    const Profile &profile = m_session->profile();
    const QVector<SliderSpec> specs = {
        {QStringLiteral("brightness"), QStringLiteral("亮度"), -100, 100,
         static_cast<int>(profile.brightness), QString()},
        {QStringLiteral("contrast"), QStringLiteral("对比度"), -100, 100,
         static_cast<int>(profile.contrast), QString()},
        {QStringLiteral("saturation"), QStringLiteral("饱和度"), -100, 100,
         static_cast<int>(profile.saturation), QString()},
        {QStringLiteral("gamma"), QStringLiteral("Gamma"), 50, 200,
         static_cast<int>(profile.gamma * 100), QString()},
        {QStringLiteral("highlight"), QStringLiteral("高光压缩"), 0, 200,
         static_cast<int>(profile.highlight), QStringLiteral("%")},
        {QStringLiteral("cas"), QStringLiteral("CAS 锐化"), 0, 100,
         static_cast<int>(profile.casSharpness), QStringLiteral("%")},
    };

    for (const SliderSpec &spec : specs) {
        auto *row = new QWidget(panel);
        auto *rowLayout = new QHBoxLayout(row);
        rowLayout->setContentsMargins(0, 0, 0, 0);
        rowLayout->setSpacing(8);

        auto *label = new QLabel(spec.title, row);
        label->setMinimumWidth(64);
        label->setProperty("variant", "caption");

        auto *slider = new QSlider(Qt::Horizontal, row);
        slider->setRange(spec.min, spec.max);
        slider->setValue(spec.initial);
        slider->setEnabled(pro);

        auto *value = new QLabel(formatQualityValue(spec.key, spec.initial, spec.suffix), row);
        value->setProperty("variant", "mono");
        value->setFont(Theme::monoFont(12));
        value->setMinimumWidth(48);
        value->setAlignment(Qt::AlignRight | Qt::AlignVCenter);

        rowLayout->addWidget(label);
        rowLayout->addWidget(slider, 1);
        rowLayout->addWidget(value);
        layout->addWidget(row);

        m_qualityRows.append({spec.key, slider, value, spec.suffix});

        connect(slider, &QSlider::valueChanged, this, [this, key = spec.key,
                                                       value, suffix = spec.suffix](int v) {
            value->setText(formatQualityValue(key, v, suffix));
            if (m_updatingQuality) {
                return;
            }
            Profile updated = m_session->profile();
            if (key == QStringLiteral("brightness")) {
                updated.brightness = v;
            } else if (key == QStringLiteral("contrast")) {
                updated.contrast = v;
            } else if (key == QStringLiteral("saturation")) {
                updated.saturation = v;
            } else if (key == QStringLiteral("gamma")) {
                updated.gamma = v / 100.0;
            } else if (key == QStringLiteral("highlight")) {
                updated.highlight = v;
            } else {
                updated.casSharpness = v;
            }
            m_session->applyProfile(updated, false);
        });
        connect(slider, &QSlider::sliderReleased, this,
                [this] { m_session->applyProfile(m_session->profile(), true); });
    }

    m_compareButton = new QPushButton(QStringLiteral("长按对比原图"), panel);
    m_compareButton->setEnabled(pro);
    layout->addWidget(m_compareButton);
    connect(m_compareButton, &QPushButton::pressed, this, [this] { setBypassEnhance(true); });
    connect(m_compareButton, &QPushButton::released, this, [this] { setBypassEnhance(false); });

    m_qualityInfo = new QLabel(QStringLiteral("--"), panel);
    m_qualityInfo->setProperty("variant", "micro");
    layout->addWidget(m_qualityInfo);

    layout->addStretch(1);

    connect(m_qualityResetButton, &QToolButton::clicked, this, &ConsolePage::resetQuality);
    return panel;
}

QWidget *ConsolePage::buildKeymapTab() {
    auto *panel = new QWidget();
    auto *layout = new QVBoxLayout(panel);
    layout->setContentsMargins(16, 16, 16, 16);
    layout->setSpacing(10);

    auto *profileRow = new QHBoxLayout();
    auto *profileLabel = new QLabel(QStringLiteral("方案"), panel);
    profileLabel->setProperty("variant", "caption");
    m_profileCombo = new QComboBox(panel);
    profileRow->addWidget(profileLabel);
    profileRow->addWidget(m_profileCombo, 1);
    layout->addLayout(profileRow);

    auto *profileActions = new QHBoxLayout();
    profileActions->setSpacing(8);
    m_newProfileButton = makeToolButton(panel, QStringLiteral("新建方案"), "outline");
    m_importProfileButton = makeToolButton(panel, QStringLiteral("导入"), "outline");
    m_exportProfileButton = makeToolButton(panel, QStringLiteral("导出"), "outline");
    m_newProfileButton->setFixedHeight(28);
    m_importProfileButton->setFixedHeight(28);
    m_exportProfileButton->setFixedHeight(28);
    m_newProfileButton->setToolTip(QStringLiteral("复制当前方案并另存为新方案"));
    m_importProfileButton->setToolTip(QStringLiteral("从 JSON 文件导入键位方案"));
    m_exportProfileButton->setToolTip(QStringLiteral("将当前方案导出为 JSON 文件"));
    profileActions->addWidget(m_newProfileButton, 1);
    profileActions->addWidget(m_importProfileButton, 1);
    profileActions->addWidget(m_exportProfileButton, 1);
    layout->addLayout(profileActions);

    // 浏览态
    m_browseBox = new QWidget(panel);
    auto *browse = new QVBoxLayout(m_browseBox);
    browse->setContentsMargins(0, 0, 0, 0);
    browse->setSpacing(10);

    m_keymapSummary = new QLabel(m_browseBox);
    m_keymapSummary->setProperty("variant", "caption");
    m_keymapSummary->setWordWrap(true);
    browse->addWidget(m_keymapSummary);

    m_editKeymapButton = makeToolButton(m_browseBox, QStringLiteral("编辑键位"), "primary");
    browse->addWidget(m_editKeymapButton);

    auto *browseHint = new QLabel(QStringLiteral("F9 也可进入编辑"), m_browseBox);
    browseHint->setProperty("variant", "micro");
    browse->addWidget(browseHint);

    layout->addWidget(m_browseBox);

    // 编辑态
    m_editBox = new QWidget(panel);
    auto *edit = new QVBoxLayout(m_editBox);
    edit->setContentsMargins(0, 0, 0, 0);
    edit->setSpacing(10);

    auto *toolRow = new QHBoxLayout();
    m_addBindingButton = makeToolButton(m_editBox, QStringLiteral("添加键位"), "ghost");
    m_saveKeymapButton = makeToolButton(m_editBox, QStringLiteral("保存"), "primary");
    m_exitEditButton = makeToolButton(m_editBox, QStringLiteral("退出编辑"), "ghost");
    toolRow->addWidget(m_addBindingButton);
    toolRow->addWidget(m_saveKeymapButton, 1);
    toolRow->addWidget(m_exitEditButton);
    edit->addLayout(toolRow);

    auto *divider = new QFrame(m_editBox);
    divider->setObjectName(QStringLiteral("Divider"));
    edit->addWidget(divider);

    // 鼠标灵敏度：滑杆 + 数值输入
    auto *sensitivityRow = new QHBoxLayout();
    sensitivityRow->addWidget(new QLabel(QStringLiteral("鼠标灵敏度"), m_editBox));
    m_sensitivitySlider = new QSlider(Qt::Horizontal, m_editBox);
    m_sensitivitySlider->setRange(20, 300);
    m_sensitivitySlider->setValue(100);
    sensitivityRow->addWidget(m_sensitivitySlider, 1);
    m_sensitivitySpin = new QDoubleSpinBox(m_editBox);
    m_sensitivitySpin->setRange(0.20, 3.00);
    m_sensitivitySpin->setSingleStep(0.05);
    m_sensitivitySpin->setDecimals(2);
    m_sensitivitySpin->setSuffix(QStringLiteral(" ×"));
    m_sensitivitySpin->setFixedWidth(84);
    sensitivityRow->addWidget(m_sensitivitySpin);
    edit->addLayout(sensitivityRow);

    auto *selectedTitle = new QLabel(QStringLiteral("选中键位"), m_editBox);
    selectedTitle->setObjectName(QStringLiteral("SectionTitle"));
    edit->addWidget(selectedTitle);

    auto *keyRow = new QHBoxLayout();
    keyRow->addWidget(new QLabel(QStringLiteral("按键"), m_editBox));
    m_keyCaptureButton = new QPushButton(QStringLiteral("--"), m_editBox);
    keyRow->addWidget(m_keyCaptureButton, 1);
    m_cancelKeyCaptureButton = new QPushButton(QStringLiteral("取消监听"), m_editBox);
    m_cancelKeyCaptureButton->setVisible(false);
    keyRow->addWidget(m_cancelKeyCaptureButton);
    edit->addLayout(keyRow);

    auto *positionRow = new QHBoxLayout();
    positionRow->addWidget(new QLabel(QStringLiteral("X"), m_editBox));
    m_xSpin = new QSpinBox(m_editBox);
    m_xSpin->setRange(0, 1000);
    positionRow->addWidget(m_xSpin);
    positionRow->addWidget(new QLabel(QStringLiteral("Y"), m_editBox));
    m_ySpin = new QSpinBox(m_editBox);
    m_ySpin->setRange(0, 1000);
    positionRow->addWidget(m_ySpin);
    edit->addLayout(positionRow);

    auto *sizeRow = new QHBoxLayout();
    m_sizeLabel = new QLabel(QStringLiteral("大小"), m_editBox);
    sizeRow->addWidget(m_sizeLabel);
    m_sizeSlider = new QSlider(Qt::Horizontal, m_editBox);
    m_sizeSlider->setRange(20, 160);
    sizeRow->addWidget(m_sizeSlider, 1);
    edit->addLayout(sizeRow);

    // 疾跑大圆：仅轮盘区域选中时显示（小圆 ≤ 大圆）
    m_sprintSizeRow = new QWidget(m_editBox);
    auto *sprintSizeLayout = new QHBoxLayout(m_sprintSizeRow);
    sprintSizeLayout->setContentsMargins(0, 0, 0, 0);
    sprintSizeLayout->addWidget(new QLabel(QStringLiteral("大圆"), m_sprintSizeRow));
    m_sprintSizeSlider = new QSlider(Qt::Horizontal, m_sprintSizeRow);
    m_sprintSizeSlider->setRange(20, 300);
    sprintSizeLayout->addWidget(m_sprintSizeSlider, 1);
    edit->addWidget(m_sprintSizeRow);
    m_sprintSizeRow->setVisible(false);

    m_deleteBindingButton = makeToolButton(m_editBox, QStringLiteral("删除此键位"), "danger");
    edit->addWidget(m_deleteBindingButton);

    auto *overlapHint = new QLabel(
        QStringLiteral("键位可重叠：实际触点固定在圆心，圆圈大小只影响编辑边界与命中范围。"),
        m_editBox);
    overlapHint->setProperty("variant", "micro");
    overlapHint->setWordWrap(true);
    edit->addWidget(overlapHint);

    auto *editHint = new QLabel(
        QStringLiteral("拖拽放置 · 滚轮缩放 · Ctrl+滚轮调疾跑大圆 · Delete 删除 · Ctrl+S 保存\n"
                       "滚轮上下也可绑定：选中按键框后滚动滚轮"),
        m_editBox);
    editHint->setProperty("variant", "micro");
    editHint->setWordWrap(true);
    edit->addWidget(editHint);

    layout->addWidget(m_editBox);
    layout->addStretch(1);

    // 交互
    connect(m_profileCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this](int index) {
                if (index >= 0) {
                    loadKeymapProfile(m_profileCombo->itemText(index));
                }
            });
    connect(m_editKeymapButton, &QPushButton::clicked, this, &ConsolePage::beginKeymapEdit);
    connect(m_newProfileButton, &QPushButton::clicked, this, &ConsolePage::createKeymapProfile);
    connect(m_importProfileButton, &QPushButton::clicked, this,
            &ConsolePage::importKeymapProfile);
    connect(m_exportProfileButton, &QPushButton::clicked, this,
            &ConsolePage::exportKeymapProfile);
    connect(m_addBindingButton, &QPushButton::clicked, this, &ConsolePage::addKeymapBinding);
    connect(m_saveKeymapButton, &QPushButton::clicked, this, &ConsolePage::saveKeymap);
    connect(m_exitEditButton, &QPushButton::clicked, this, [this] {
        if (confirmLeaveEdit()) {
            exitKeymapEdit(false);
        }
    });
    connect(m_deleteBindingButton, &QPushButton::clicked, this,
            &ConsolePage::deleteKeymapBinding);
    connect(m_keyCaptureButton, &QPushButton::clicked, this, &ConsolePage::startKeyCapture);
    connect(m_cancelKeyCaptureButton, &QPushButton::clicked, this,
            &ConsolePage::stopKeyCapture);
    connect(m_xSpin, QOverload<int>::of(&QSpinBox::valueChanged), this,
            [this](int) { applyBindingFromUi(); });
    connect(m_ySpin, QOverload<int>::of(&QSpinBox::valueChanged), this,
            [this](int) { applyBindingFromUi(); });
    connect(m_sizeSlider, &QSlider::valueChanged, this, [this](int) { applyBindingFromUi(); });
    connect(m_sprintSizeSlider, &QSlider::valueChanged, this,
            [this](int) { applyBindingFromUi(); });
    connect(m_sensitivitySlider, &QSlider::valueChanged, this, [this](int value) {
        if (m_sensitivitySpin &&
            std::abs(m_sensitivitySpin->value() - value / 100.0) > 0.0001) {
            QSignalBlocker blocker(m_sensitivitySpin);
            m_sensitivitySpin->setValue(value / 100.0);
        }
        m_keymapProfile.lookSensitivity = value / 100.0;
        markKeymapDirty();
    });
    connect(m_sensitivitySpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this,
            [this](double value) {
                const int sliderValue = static_cast<int>(std::lround(value * 100));
                if (m_sensitivitySlider && m_sensitivitySlider->value() != sliderValue) {
                    QSignalBlocker blocker(m_sensitivitySlider);
                    m_sensitivitySlider->setValue(sliderValue);
                }
                m_keymapProfile.lookSensitivity = value;
                markKeymapDirty();
            });

    return panel;
}

void ConsolePage::buildStatusBar(QVBoxLayout *rootLayout) {
    m_statusBar = new QFrame(this);
    m_statusBar->setObjectName(QStringLiteral("StatusBar"));
    m_statusBar->setFixedHeight(28);

    auto *layout = new QHBoxLayout(m_statusBar);
    layout->setContentsMargins(12, 0, 12, 0);
    layout->setSpacing(16);

    m_statusAdb = new QLabel(QStringLiteral("adb 未连接"), m_statusBar);
    m_statusAdb->setProperty("variant", "micro");
    m_statusBitrate = new QLabel(QStringLiteral("0.0 Mbps"), m_statusBar);
    m_statusBitrate->setProperty("variant", "micro");
    m_statusBitrate->setFont(Theme::monoFont(11));
    m_statusLatency = new QLabel(QStringLiteral("输入延迟 --"), m_statusBar);
    m_statusLatency->setProperty("variant", "micro");
    m_statusLatency->setFont(Theme::monoFont(11));
    m_statusCodec = new QLabel(QStringLiteral("--"), m_statusBar);
    m_statusCodec->setProperty("variant", "micro");

    layout->addWidget(m_statusAdb);
    layout->addWidget(m_statusBitrate);
    layout->addWidget(m_statusLatency);
    layout->addStretch(1);
    layout->addWidget(m_statusCodec);

    rootLayout->addWidget(m_statusBar);
}

void ConsolePage::layoutVideoOverlays() {
    if (!m_video) {
        return;
    }
    if (m_overlay) {
        m_overlay->setGeometry(m_video->videoRect());
    }
    const QRect videoRect = m_video->videoRect();
    const QRect anchor = videoRect.isEmpty() ? m_video->rect() : videoRect;
    if (m_hud) {
        m_hud->move(anchor.left() + 12, anchor.bottom() - m_hud->height() - 12);
    }
    if (m_summonHint) {
        m_summonHint->move(anchor.right() - m_summonHint->width() - 12,
                           anchor.bottom() - m_summonHint->height() - 12);
    }
    if (m_statsOverlay) {
        m_statsOverlay->adjustSize();
        m_statsOverlay->move(anchor.left() + (anchor.width() - m_statsOverlay->width()) / 2,
                             anchor.top() + 12);
    }
}

// ---------------------------------------------------------------------------
// 画质 / 视频参数
// ---------------------------------------------------------------------------
void ConsolePage::refreshProfileToUi() {
    if (!m_session) {
        return;
    }
    const Profile &profile = m_session->profile();

    QSignalBlocker blockResolution(m_resolutionCombo);
    QSignalBlocker blockFps(m_fpsCombo);
    QSignalBlocker blockBitrate(m_bitrateCombo);

    m_resolutionCombo->setCurrentIndex(profile.maxSize >= 1080 ? 1
                                   : profile.maxSize >= 720 ? 2
                                   : profile.maxSize >= 480 ? 3
                                                            : 0);
    m_fpsCombo->setCurrentIndex(profile.maxFps >= 120 ? 3
                              : profile.maxFps >= 90 ? 2
                              : profile.maxFps >= 60 ? 1
                                                     : 0);
    const int mbps = std::max(1, profile.videoBitRate / 1'000'000);
    const int bitrateIndex = mbps <= 4 ? 0 : mbps <= 8 ? 1 : mbps <= 12 ? 2
                                                              : mbps <= 16 ? 3
                                                              : mbps <= 24 ? 4
                                                                           : 5;
    m_bitrateCombo->setCurrentIndex(bitrateIndex);

    VideoWidget::EnhanceParams params;
    params.brightness = static_cast<float>(profile.brightness);
    params.contrast = static_cast<float>(profile.contrast);
    params.saturation = static_cast<float>(profile.saturation);
    params.gamma = static_cast<float>(profile.gamma);
    params.highlight = static_cast<float>(profile.highlight);
    params.casSharpness = static_cast<float>(profile.casSharpness);
    if (!LicenseManager::instance().isPro()) {
        // 未激活时不应用方案/导入文件里的增强参数，保持原始画面。
        params = VideoWidget::EnhanceParams{};
    }
    m_video->setEnhanceParams(params);

    refreshQualityToUi();

    if (m_mode != InputMode::Edit && !m_keymapDirty) {
        m_keymapProfile = profile;
        normalizeLookProfile(m_keymapProfile);
        m_overlay->setProfile(m_keymapProfile);
        refreshKeymapSummary();
        refreshKeymapProperties();
        }
}

void ConsolePage::refreshQualityAvailability() {
    const bool pro = LicenseManager::instance().isPro();
    for (const QualityRow &row : m_qualityRows) {
        row.slider->setEnabled(pro);
    }
    if (m_compareButton) {
        m_compareButton->setEnabled(pro);
    }
    if (m_qualityResetButton) {
        m_qualityResetButton->setEnabled(pro);
    }
    if (m_qualityLockHint) {
        m_qualityLockHint->setVisible(!pro);
    }
}

void ConsolePage::onLicenseChanged() {
    refreshProfileToUi();
    refreshQualityAvailability();
}

void ConsolePage::refreshQualityToUi() {
    const Profile &profile = m_session->profile();
    m_updatingQuality = true;
    for (const QualityRow &row : m_qualityRows) {
        int value = 0;
        if (row.key == QStringLiteral("brightness")) {
            value = static_cast<int>(profile.brightness);
        } else if (row.key == QStringLiteral("contrast")) {
            value = static_cast<int>(profile.contrast);
        } else if (row.key == QStringLiteral("saturation")) {
            value = static_cast<int>(profile.saturation);
        } else if (row.key == QStringLiteral("gamma")) {
            value = static_cast<int>(profile.gamma * 100);
        } else if (row.key == QStringLiteral("highlight")) {
            value = static_cast<int>(profile.highlight);
        } else {
            value = static_cast<int>(profile.casSharpness);
        }
        QSignalBlocker blocker(row.slider);
        row.slider->setValue(value);
        row.value->setText(formatQualityValue(row.key, value, row.suffix));
    }
    m_updatingQuality = false;
}

void ConsolePage::resetQuality() {
    const auto answer = QMessageBox::question(
        this, QStringLiteral("重置画面增强"),
        QStringLiteral("恢复该方案的默认画面增强参数？"), QMessageBox::Yes | QMessageBox::No,
        QMessageBox::No);
    if (answer != QMessageBox::Yes) {
        return;
    }
    Profile profile = m_session->profile();
    profile.brightness = 0;
    profile.contrast = 0;
    profile.saturation = 0;
    profile.gamma = 1.0;
    profile.highlight = 0;
    profile.casSharpness = 0;
    m_session->applyProfile(profile, true);
    refreshQualityToUi();
    showHud(QStringLiteral("画面增强已重置"));
}

void ConsolePage::applyVideoOptions() {
    if (!m_session) {
        return;
    }
    Profile profile = m_session->profile();

    static const int resolutions[] = {0, 1080, 720, 480};
    profile.maxSize = resolutions[std::clamp(m_resolutionCombo->currentIndex(), 0, 3)];

    static const int fpsValues[] = {30, 60, 90, 120};
    profile.maxFps = fpsValues[std::clamp(m_fpsCombo->currentIndex(), 0, 3)];

    static const int bitrates[] = {4, 8, 12, 16, 24, 32};
    profile.videoBitRate = bitrates[std::clamp(m_bitrateCombo->currentIndex(), 0, 5)] * 1'000'000;

    m_session->applyProfile(profile, true);

    if (m_session->state() == Session::State::Streaming && !m_session->connectedSerial().isEmpty()) {
        showHud(QStringLiteral("视频参数已更新，正在重连…"));
        m_session->connectDevice(m_session->connectedSerial());
    }
}

void ConsolePage::updateStatsUi() {
    const Session::Stats stats = m_session->stats();
    const double presentMs = m_video ? m_video->presentLatencyMs() : -1.0;

    m_fpsLabel->setText(stats.fps > 0.5 ? QString::number(std::lround(stats.fps))
                                        : QStringLiteral("--"));
    if (m_statsOverlay) {
        const QString fps = stats.fps > 0.5 ? QString::number(std::lround(stats.fps))
                                            : QStringLiteral("--");
        const QString mbps = QString::number(stats.bitrateMbps, 'f', 1);
        const QString latency = stats.inputLatencyMs >= 0
                                    ? QString::number(stats.inputLatencyMs, 'f', 1)
                                    : QStringLiteral("--");
        const QString present = presentMs >= 0 ? QString::number(presentMs, 'f', 0)
                                               : QStringLiteral("--");
        m_statsOverlay->setText(QStringLiteral("%1 FPS · %2 Mbps · 输入 %3 ms · 显示 %4 ms")
                                    .arg(fps, mbps, latency, present));
        m_statsOverlay->adjustSize();
        layoutVideoOverlays();
    }
    // 这里量的是「系统采集按键 → 触控指令发出」，已包含 GUI 线程排队时间。
    const double latency = stats.inputLatencyMs;
    if (latency >= 0) {
        m_latencyLabel->setText(QStringLiteral("%1 ms").arg(latency, 0, 'f', 1));
        m_latencyLabel->setProperty("state", latency > 12 ? "danger"
                                        : latency > 6  ? "warning"
                                                       : "success");
    } else {
        m_latencyLabel->setText(QStringLiteral("-- ms"));
    }
    m_latencyLabel->style()->unpolish(m_latencyLabel);
    m_latencyLabel->style()->polish(m_latencyLabel);

    const bool streaming = m_session->state() == Session::State::Streaming;
    m_statusAdb->setText(streaming ? QStringLiteral("● adb 已连接")
                                   : QStringLiteral("○ adb 断开"));
    m_statusAdb->setProperty("state", streaming ? "success" : "secondary");
    m_statusAdb->style()->unpolish(m_statusAdb);
    m_statusAdb->style()->polish(m_statusAdb);

    m_statusBitrate->setText(QStringLiteral("%1 Mbps").arg(stats.bitrateMbps, 0, 'f', 1));
    m_statusLatency->setText(QStringLiteral("输入 %1 ms · 解码 %2 ms · 跳过 %3/s · 积压 %4 KB")
                                 .arg(latency >= 0 ? QString::number(latency, 'f', 1)
                                                   : QStringLiteral("--"))
                                 .arg(stats.decodeMs, 0, 'f', 1)
                                 .arg(std::lround(stats.skippedFps))
                                 .arg(stats.backlogKb >= 0
                                          ? QString::number(std::lround(stats.backlogKb))
                                          : QStringLiteral("--")));
    m_statusCodec->setText(
        QStringLiteral("%1 · %2×%3 · %4")
            .arg(stats.videoCodec.isEmpty() ? QStringLiteral("--") : stats.videoCodec.toUpper())
            .arg(stats.videoWidth)
            .arg(stats.videoHeight)
            .arg(stats.hardwareDecode ? QStringLiteral("硬件解码") : QStringLiteral("软件解码")));

    if (!m_video->hasFrame() && stats.videoWidth > 0 && stats.videoHeight > 0) {
        m_video->setFallbackAspect(stats.videoWidth / static_cast<double>(stats.videoHeight));
    }

    // 每秒一行链路指标，便于真机对比不同帧率下的积压趋势（也方便用户反馈问题）。
    if (m_session->state() == Session::State::Streaming) {
        qInfo().nospace() << "stats fps=" << QString::number(stats.fps, 'f', 1)
                          << " skip=" << QString::number(stats.skippedFps, 'f', 1)
                          << " decode=" << QString::number(stats.decodeMs, 'f', 1)
                          << "ms input=" << QString::number(stats.inputLatencyMs, 'f', 1)
                          << "ms present=" << QString::number(presentMs, 'f', 1)
                          << "ms pending=" << stats.pendingFrame
                          << " backlog=" << QString::number(stats.backlogKb, 'f', 0) << "KB"
                          << " target=" << m_session->profile().maxFps;
    }

    if (m_qualityInfo) {
        if (stats.videoWidth > 0) {
            const int fps = stats.fps > 0.5 ? static_cast<int>(std::lround(stats.fps))
                                            : m_session->profile().maxFps;
            m_qualityInfo->setText(QStringLiteral("%1×%2 · %3fps · %4")
                                       .arg(stats.videoWidth)
                                       .arg(stats.videoHeight)
                                       .arg(fps)
                                       .arg(stats.videoCodec.isEmpty()
                                                ? QStringLiteral("--")
                                                : stats.videoCodec.toUpper()));
        } else {
            m_qualityInfo->setText(QStringLiteral("--"));
        }
    }
}

// ---------------------------------------------------------------------------
// 输入模式
// ---------------------------------------------------------------------------
void ConsolePage::setActive(bool active) {
    if (m_active == active) {
        return;
    }
    m_active = active;
    if (active) {
        // 激活状态可能在设置页发生变化，进入投屏页时重新应用门槛与增强参数。
        refreshProfileToUi();
        refreshQualityAvailability();
        // 默认模式：全屏=游戏映射，窗口=鼠标指针（S5）。
        setInputMode(m_session && m_session->state() == Session::State::Streaming
                         ? (m_gameMode ? InputMode::Mapping : InputMode::Pointer)
                         : InputMode::Idle);
        if (!m_summonHintShown) {
            showSummonHint();
        }
    }
}

bool ConsolePage::requestDeactivate() {
    if (m_mode == InputMode::Edit && !confirmLeaveEdit()) {
        return false;
    }
    if (m_mode == InputMode::Edit) {
        exitKeymapEdit(false);
    }
    m_active = false;
    m_summonHeld = false;
    m_summonHoldTimer->stop();
    setInputMode(InputMode::Idle);
    m_hud->hide();
    m_summonHint->hide();
    updateStatsOverlay();
    return true;
}

bool ConsolePage::canvasTouchActive() const {
    if (!m_active || !m_session) {
        return false;
    }
    if (m_mode == InputMode::Pointer) {
        return true;
    }
    // 窗口态（非全屏）映射模式下，左键直接操作手机：单击=点按、拖动=滑动。
    return !m_gameMode && m_mode == InputMode::Mapping;
}

void ConsolePage::cancelTouchDrag() {
    if (!m_draggingTouch) {
        return;
    }
    m_draggingTouch = false;
    if (m_session) {
        m_session->setTouchDragActive(false);
        sendPointerTouch(static_cast<int>(proto::TouchAction::Up), m_lastTouchPos, true);
    }
}

void ConsolePage::setInputMode(InputMode mode) {
    if (m_mode == mode) {
        updateInputCapture();
        return;
    }

    const InputMode previous = m_mode;
    if (m_session) {
        m_session->releaseAllTouches();
    }
    m_mode = mode;
    m_draggingTouch = false;
    if (m_session) {
        m_session->setTouchDragActive(false);
        if (mode == InputMode::Pointer) {
            // 先关闭视角，再移动系统光标，避免光标定位产生的 Raw Input 被当成视角位移。
            m_session->setLookEnabled(false);
        }
    }

    if (mode != InputMode::Edit) {
        m_overlay->hide();
        if (m_capturingKey) {
            stopKeyCapture();
        }
    }

    switch (mode) {
    case InputMode::Pointer: {
        m_video->setFocus();
        showHud(pointerHudText(), true);
        // 仅全屏呼出鼠标时把光标定位到屏幕中央（画面中心），窗口态保持光标原位
        if (m_gameMode) {
            const QRect rect = m_video->videoRect();
            if (!rect.isEmpty()) {
                QCursor::setPos(m_video->mapToGlobal(rect.center()));
            }
        }
        if (m_mouseButton) {
            QSignalBlocker blocker(m_mouseButton);
            m_mouseButton->setChecked(true);
        }
        break;
    }
    case InputMode::Mapping:
        if (previous == InputMode::Pointer) {
            showHud(QStringLiteral("已返回游戏操作"));
        }
        if (m_mouseButton) {
            QSignalBlocker blocker(m_mouseButton);
            m_mouseButton->setChecked(false);
        }
        break;
    case InputMode::Edit:
        m_overlay->setGeometry(m_video->videoRect());
        m_overlay->show();
        m_overlay->raise();
        m_overlay->setFocus();
        break;
    case InputMode::Idle:
        if (m_mouseButton) {
            QSignalBlocker blocker(m_mouseButton);
            m_mouseButton->setChecked(false);
        }
        m_hud->hide();
        break;
    }

    updateCursor();
    updateMouseModeButton();
    updateInputCapture();
}

void ConsolePage::updateMouseModeButton() {
    if (!m_mouseButton) {
        return;
    }
    const bool active = m_mode == InputMode::Pointer;
    m_mouseButton->setIcon(IconFactory::icon(QStringLiteral("mouse"),
                                             active ? Theme::brandPrimary()
                                                    : Theme::textSecondary(),
                                             16));
}

void ConsolePage::updateInputCapture() {
    if (!m_session) {
        return;
    }
    const bool streaming = m_session->state() == Session::State::Streaming;
    // 指针模式也保持键盘/其它鼠标键映射：呼出鼠标只接管光标与视角。
    const bool interactive = m_mode == InputMode::Mapping || m_mode == InputMode::Pointer;
    const bool capture = m_active && streaming && interactive;
    m_session->setInputCaptureEnabled(capture);
    // 左键由画布直操注入（指针模式 / 窗口态映射），Raw Input 不再映射左键。
    m_session->setDirectTouchEnabled(capture &&
                                     (m_mode == InputMode::Pointer || !m_gameMode));
    // 指针模式鼠标用于移动系统光标：位移不再驱动视角。
    m_session->setLookEnabled(m_mode != InputMode::Pointer);
}

void ConsolePage::updateCursor() {
    if (!m_video) {
        return;
    }
    if (m_mode == InputMode::Pointer || m_mode == InputMode::Edit) {
        m_video->setCursor(Qt::ArrowCursor);
    } else if (m_mode == InputMode::Mapping && m_gameMode) {
        m_video->setCursor(Qt::BlankCursor);
    } else {
        m_video->unsetCursor();
    }
}

void ConsolePage::enterPointerMode() {
    if (!m_active) {
        return;
    }
    if (m_mode == InputMode::Edit) {
        showHud(QStringLiteral("键位编辑中，请先退出编辑"));
        return;
    }
    if (!m_session || m_session->state() != Session::State::Streaming) {
        showHud(QStringLiteral("未连接设备，无法呼出鼠标"));
        return;
    }
    setInputMode(InputMode::Pointer);
}

void ConsolePage::exitPointerMode() {
    if (m_mode == InputMode::Pointer) {
        setInputMode(InputMode::Mapping);
    }
}

void ConsolePage::setSummonKey(int vk) {
    m_summonKey = vk;
    updateSummonText();
    if (m_mode == InputMode::Pointer) {
        showHud(pointerHudText(), true);
    }
}

void ConsolePage::setSummonMode(const QString &mode) {
    const SummonMode next = mode == QStringLiteral("hold") ? SummonMode::Hold
                                                           : SummonMode::LongPress;
    if (m_summonMode == next) {
        return;
    }
    m_summonMode = next;
    m_summonHeld = false;
    m_summonHoldTimer->stop();
    updateSummonText();
    if (m_mode == InputMode::Pointer) {
        showHud(pointerHudText(), true);
    }
}

void ConsolePage::onSummonKeyPressed(bool down) {
    if (!m_active || m_mode == InputMode::Edit) {
        return;
    }
    if (m_summonMode == SummonMode::Hold) {
        if (down) {
            if (m_mode != InputMode::Pointer) {
                enterPointerMode();
            }
        } else if (m_mode == InputMode::Pointer) {
            setInputMode(InputMode::Mapping);
        }
        return;
    }

    if (down) {
        if (m_mode == InputMode::Pointer) {
            setInputMode(InputMode::Mapping);
            return;
        }
        m_summonHeld = true;
        m_summonHoldTimer->start(m_summonHoldMs);
    } else {
        m_summonHeld = false;
        m_summonHoldTimer->stop();
    }
}

QSize ConsolePage::deviceSize() const {
    const Session::Stats stats = m_session->stats();
    return {stats.videoWidth > 0 ? stats.videoWidth : 1080,
            stats.videoHeight > 0 ? stats.videoHeight : 2400};
}

bool ConsolePage::mapToDevice(const QPoint &widgetPos, QPoint *devicePos) const {
    const QRect rect = m_video->videoRect();
    if (rect.isEmpty() || !rect.contains(widgetPos)) {
        return false;
    }
    const QSize size = deviceSize();
    const double nx = (widgetPos.x() - rect.x()) / static_cast<double>(rect.width());
    const double ny = (widgetPos.y() - rect.y()) / static_cast<double>(rect.height());
    *devicePos = {static_cast<int>(std::lround(nx * size.width())),
                  static_cast<int>(std::lround(ny * size.height()))};
    return true;
}

void ConsolePage::sendPointerTouch(int action, const QPoint &widgetPos, bool clampInside) {
    QPoint pos = widgetPos;
    const QRect rect = m_video->videoRect();
    if (rect.isEmpty()) {
        return;
    }
    if (clampInside) {
        pos.setX(std::clamp(pos.x(), rect.left(), rect.right()));
        pos.setY(std::clamp(pos.y(), rect.top(), rect.bottom()));
    }
    QPoint device;
    if (!mapToDevice(pos, &device)) {
        return;
    }
    const QSize size = deviceSize();
    const auto touchAction = static_cast<proto::TouchAction>(action);
    const bool up = touchAction == proto::TouchAction::Up;
    if (!up) {
        m_lastTouchPos = pos;
    }
    const qint32 actionButton = up ? proto::ButtonNone : proto::ButtonPrimary;
    const qint32 buttons = up ? proto::ButtonNone : proto::ButtonPrimary;
    // pointer 10 专用直操触点：键位引擎分配范围为 2..9，互不重叠。
    m_session->sendControlMessage(proto::touch(touchAction, 10, device.x(), device.y(),
                                               size.width(), size.height(), 1.0f, actionButton,
                                               buttons));
}

// ---------------------------------------------------------------------------
// 键位编辑
// ---------------------------------------------------------------------------
void ConsolePage::beginKeymapEdit() { beginKeymapEditInternal(); }

void ConsolePage::beginKeymapEditInternal() {
    if (!m_active || !m_session) {
        return;
    }
    if (m_mode == InputMode::Edit) {
        m_overlay->setFocus();
        return;
    }
    if (m_mode == InputMode::Pointer) {
        setInputMode(InputMode::Mapping);
    }

    // 断线时可能保留了未保存的键位修改；此时沿用内存方案，不重新加载会话方案。
    const bool restoringUnsaved = m_keymapDirty;
    if (!restoringUnsaved) {
        m_keymapProfile = m_session->profile();
    }
    m_keymapSelected = -1;
    setRegionTarget(0);

    m_overlay->setProfile(m_keymapProfile);
    m_overlay->setSelected(-1);
    m_overlay->setSelectedRegion(KeymapOverlay::Region::None);
    m_overlay->setEmptyHint(m_video->hasFrame() ? QString()
                                                : QStringLiteral("未连接投屏，坐标按比例编辑"));

    refreshKeymapSummary();
    refreshKeymapProperties();
    setKeymapEditUi(true);
    switchInspectorTab(1);
    setInputMode(InputMode::Edit);
    if (restoringUnsaved) {
        showHud(QStringLiteral("已恢复未保存的键位修改"));
    }
}

void ConsolePage::exitKeymapEdit(bool save) {
    if (m_mode != InputMode::Edit) {
        setKeymapEditUi(false);
        return;
    }
    stopKeyCapture();
    if (save) {
        saveKeymap();
    }
    m_overlay->hide();
    m_keymapSelected = -1;
    setRegionTarget(0);
    m_overlay->setSelected(-1);
    m_keymapDirty = false;
    setKeymapEditUi(false);
    setInputMode(m_active && m_session->state() == Session::State::Streaming ? InputMode::Mapping
                                                                            : InputMode::Idle);
}

bool ConsolePage::confirmLeaveEdit() {
    if (!m_keymapDirty) {
        return true;
    }
    QMessageBox box(QMessageBox::Question, QStringLiteral("未保存的键位"),
                    QStringLiteral("当前键位有未保存的修改，如何处理？"), QMessageBox::NoButton,
                    this);
    auto *saveButton = box.addButton(QStringLiteral("保存"), QMessageBox::AcceptRole);
    auto *discardButton = box.addButton(QStringLiteral("放弃"), QMessageBox::DestructiveRole);
    box.addButton(QStringLiteral("取消"), QMessageBox::RejectRole);
    box.exec();
    if (box.clickedButton() == saveButton) {
        saveKeymap();
        return true;
    }
    return box.clickedButton() == discardButton;
}

bool ConsolePage::switchInspectorTab(int index) {
    if (!m_inspectorStack || index < 0 || index >= m_inspectorStack->count()) {
        return true;
    }
    if (index == 0 && m_mode == InputMode::Edit) {
        if (!confirmLeaveEdit()) {
            return false;
        }
        exitKeymapEdit(false);
    }
    if (index == 0) {
        refreshQualityAvailability();
    }
    {
        QSignalBlocker blocker(m_tabs);
        m_tabs->setCurrentIndex(index);
    }
    m_inspectorStack->setCurrentIndex(index);
    return true;
}

void ConsolePage::setKeymapEditUi(bool editing) {
    m_browseBox->setVisible(!editing);
    m_editBox->setVisible(editing);
    m_profileCombo->setEnabled(!editing);
    m_newProfileButton->setEnabled(!editing);
    m_importProfileButton->setEnabled(!editing);
    m_exportProfileButton->setEnabled(!editing);
    if (m_toolbarFullscreenButton) {
        m_toolbarFullscreenButton->setEnabled(!editing);
    }
    if (editing) {
        refreshKeymapProperties();
    }
}

void ConsolePage::reloadKeymapProfiles() {
    if (!m_profileCombo) {
        return;
    }
    QSignalBlocker blocker(m_profileCombo);
    m_profileCombo->clear();
    const QVector<Profile> profiles = m_session->profileStore().loadAll();
    for (const Profile &profile : profiles) {
        m_profileCombo->addItem(profile.name);
    }
    const int index = m_profileCombo->findText(m_keymapProfile.name);
    if (index >= 0) {
        m_profileCombo->setCurrentIndex(index);
    }
}

void ConsolePage::loadKeymapProfile(const QString &name) {
    if (m_mode == InputMode::Edit) {
        return;
    }
    bool ok = false;
    const Profile profile = m_session->profileStore().load(name, &ok);
    if (!ok) {
        return;
    }
    m_keymapProfile = profile;
    normalizeLookProfile(m_keymapProfile);
    m_keymapSelected = -1;
    setRegionTarget(0);
    m_keymapDirty = false;
    m_overlay->setSelected(-1);
    m_overlay->setProfile(m_keymapProfile);
    refreshKeymapSummary();
    refreshKeymapProperties();
    m_session->applyProfile(m_keymapProfile, true);
}

void ConsolePage::createKeymapProfile() {
    if (!m_session || !m_active || m_mode == InputMode::Edit) {
        return;
    }
    bool ok = false;
    QString name = QInputDialog::getText(
        this, QStringLiteral("新建键位方案"),
        QStringLiteral("方案名称（将复制当前方案的键位与参数）"), QLineEdit::Normal,
        QStringLiteral("%1-副本").arg(m_keymapProfile.name), &ok);
    name = name.trimmed();
    if (!ok || name.isEmpty()) {
        return;
    }
    const QString invalid = QStringLiteral("\\/:*?\"<>|");
    for (const QChar &ch : invalid) {
        if (name.contains(ch)) {
            QMessageBox::warning(this, QStringLiteral("名称无效"),
                                 QStringLiteral("方案名称不能包含 \\ / : * ? \" < > |"));
            return;
        }
    }
    const QVector<Profile> profiles = m_session->profileStore().loadAll();
    for (const Profile &existing : profiles) {
        if (existing.name == name) {
            QMessageBox::warning(this, QStringLiteral("方案已存在"),
                                 QStringLiteral("已存在同名方案「%1」，请更换名称。").arg(name));
            return;
        }
    }

    Profile profile = m_keymapProfile;
    normalizeLookProfile(profile);
    profile.name = name;
    QString error;
    if (!m_session->profileStore().save(profile, &error)) {
        QMessageBox::warning(this, QStringLiteral("新建失败"), error);
        return;
    }
    m_keymapProfile = profile;
    m_keymapDirty = false;
    m_session->applyProfile(profile, false);
    reloadKeymapProfiles();
    m_overlay->setProfile(m_keymapProfile);
    refreshKeymapSummary();
    refreshKeymapProperties();
    showHud(QStringLiteral("已新建方案：%1").arg(name));
}

void ConsolePage::importKeymapProfile() {
    if (!m_session || !m_active || m_mode == InputMode::Edit) {
        return;
    }
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("导入键位方案"), m_session->profileStore().directory(),
        QStringLiteral("键位方案 (*.json)"));
    if (path.isEmpty()) {
        return;
    }
    Profile imported;
    QString error;
    if (!m_session->profileStore().importFromFile(path, &imported, &error)) {
        QMessageBox::warning(this, QStringLiteral("导入失败"), error);
        return;
    }
    const QVector<Profile> profiles = m_session->profileStore().loadAll();
    for (const Profile &existing : profiles) {
        if (existing.name == imported.name) {
            const auto answer = QMessageBox::question(
                this, QStringLiteral("同名方案"),
                QStringLiteral("已存在方案「%1」，是否覆盖？").arg(imported.name),
                QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
            if (answer != QMessageBox::Yes) {
                return;
            }
            break;
        }
    }
    if (!m_session->profileStore().save(imported, &error)) {
        QMessageBox::warning(this, QStringLiteral("导入失败"), error);
        return;
    }
    m_keymapProfile = imported;
    normalizeLookProfile(m_keymapProfile);
    m_keymapDirty = false;
    m_session->applyProfile(imported, false);
    reloadKeymapProfiles();
    m_overlay->setProfile(m_keymapProfile);
    refreshKeymapSummary();
    refreshKeymapProperties();
    showHud(QStringLiteral("已导入方案：%1").arg(imported.name));
}

void ConsolePage::exportKeymapProfile() {
    if (!m_session || !m_active || m_mode == InputMode::Edit) {
        return;
    }
    const QString suggested =
        QDir(m_session->profileStore().directory())
            .filePath(ProfileStore::sanitizeFileName(m_keymapProfile.name) + QStringLiteral(".json"));
    const QString path = QFileDialog::getSaveFileName(
        this, QStringLiteral("导出键位方案"), suggested, QStringLiteral("键位方案 (*.json)"));
    if (path.isEmpty()) {
        return;
    }
    QString target = path;
    if (!target.endsWith(QStringLiteral(".json"), Qt::CaseInsensitive)) {
        target += QStringLiteral(".json");
    }
    QString error;
    if (!m_session->profileStore().exportToFile(m_keymapProfile, target, &error)) {
        QMessageBox::warning(this, QStringLiteral("导出失败"), error);
        return;
    }
    showHud(QStringLiteral("已导出方案：%1").arg(QFileInfo(target).fileName()));
}

void ConsolePage::refreshKeymapSummary() {
    if (!m_keymapSummary) {
        return;
    }
    m_keymapSummary->setText(
        QStringLiteral("方案：%1\n轮盘 %2（疾跑 %3） · 视角半径 %4\n视角灵敏度 %5×")
            .arg(m_keymapProfile.name)
            .arg(m_keymapProfile.joystickRadius, 0, 'f', 3)
            .arg(m_keymapProfile.joystickSprintRadius, 0, 'f', 3)
            .arg(m_keymapProfile.lookRadius, 0, 'f', 3)
            .arg(m_keymapProfile.lookSensitivity, 0, 'f', 2));
}

bool ConsolePage::wheelBound(int delta) const {
    const int vk = delta > 0 ? ks::kVkWheelUp : ks::kVkWheelDown;
    for (const KeyBinding &binding : m_keymapProfile.bindings) {
        if (binding.vk == vk) {
            return true;
        }
    }
    return false;
}

int ConsolePage::selectedBinding() const {
    if (m_keymapSelected < 0 || m_keymapSelected >= m_keymapProfile.bindings.size()) {
        return -1;
    }
    return m_keymapSelected;
}

void ConsolePage::refreshKeymapProperties() {
    if (!m_keyCaptureButton) {
        return;
    }
    // 回填 UI 期间屏蔽 applyBindingFromUi：滑杆 setRange 触发的钳制 valueChanged
    // 不得把面板旧值（区域坐标/最大半径）写进当前键位（S3 根因）。
    m_syncingBindingUi = true;

    // 灵敏度始终反映当前方案
    if (m_sensitivitySlider && m_sensitivitySpin) {
        const int sliderValue =
            std::clamp(static_cast<int>(std::lround(m_keymapProfile.lookSensitivity * 100)), 20,
                       300);
        QSignalBlocker blockSlider(m_sensitivitySlider);
        QSignalBlocker blockSpin(m_sensitivitySpin);
        m_sensitivitySlider->setValue(sliderValue);
        m_sensitivitySpin->setValue(sliderValue / 100.0);
    }

    const int index = selectedBinding();
    const bool regionMode = m_regionTarget != 0;
    const bool valid = index >= 0;

    m_xSpin->setEnabled(valid || regionMode);
    m_ySpin->setEnabled(valid || regionMode);
    m_sizeSlider->setEnabled(valid || regionMode);
    m_keyCaptureButton->setEnabled(valid);
    m_deleteBindingButton->setEnabled(valid);

    if (regionMode) {
        const bool joystick = m_regionTarget == 1;
        m_keyCaptureButton->setText(joystick ? QStringLiteral("轮盘")
                                             : QStringLiteral("视角触发区"));
        QSignalBlocker blockX(m_xSpin);
        QSignalBlocker blockY(m_ySpin);
        QSignalBlocker blockSize(m_sizeSlider);
        QSignalBlocker blockSprint(m_sprintSizeSlider);
        if (joystick) {
            m_sizeLabel->setText(QStringLiteral("小圆"));
            m_sprintSizeRow->setVisible(true);
            m_sizeSlider->setRange(20, 300);
            m_sizeSlider->setValue(static_cast<int>(m_keymapProfile.joystickRadius * 1000));
            m_sprintSizeSlider->setValue(
                static_cast<int>(m_keymapProfile.joystickSprintRadius * 1000));
        } else {
            m_sizeLabel->setText(QStringLiteral("大小"));
            m_sprintSizeRow->setVisible(false);
            m_sizeSlider->setRange(50, 450);
            m_sizeSlider->setValue(static_cast<int>(m_keymapProfile.lookRadius * 1000));
        }
        const QPointF center = joystick ? m_keymapProfile.joystickCenter
                                        : m_keymapProfile.lookCenter;
        m_xSpin->setValue(static_cast<int>(center.x() * 1000));
        m_ySpin->setValue(static_cast<int>(center.y() * 1000));
        m_syncingBindingUi = false;
        return;
    }

    m_sizeLabel->setText(QStringLiteral("大小"));
    m_sprintSizeRow->setVisible(false);
    {
        QSignalBlocker blockSizeRange(m_sizeSlider);
        m_sizeSlider->setRange(20, 160);
    }
    if (!valid) {
        m_keyCaptureButton->setText(QStringLiteral("--"));
        m_syncingBindingUi = false;
        return;
    }
    const KeyBinding &binding = m_keymapProfile.bindings.at(index);
    m_keyCaptureButton->setText(m_capturingKey ? QStringLiteral("请按键…")
                                               : vkName(binding.vk));
    QSignalBlocker blockX(m_xSpin);
    QSignalBlocker blockY(m_ySpin);
    QSignalBlocker blockSize(m_sizeSlider);
    m_xSpin->setValue(static_cast<int>(binding.center.x() * 1000));
    m_ySpin->setValue(static_cast<int>(binding.center.y() * 1000));
    m_sizeSlider->setValue(static_cast<int>(binding.radius * 1000));
    m_syncingBindingUi = false;
}

void ConsolePage::setRegionTarget(int region) {
    m_regionTarget = region;
    if (region == 0) {
        m_overlay->setSelectedRegion(KeymapOverlay::Region::None);
    } else {
        m_keymapSelected = -1;
        m_overlay->setSelected(-1);
        m_overlay->setSelectedRegion(
            region == 1 ? KeymapOverlay::Region::Joystick : KeymapOverlay::Region::Look);
    }
    refreshKeymapProperties();
}

void ConsolePage::markKeymapDirty() { m_keymapDirty = true; }

void ConsolePage::saveKeymap() {
    if (!m_session) {
        return;
    }
    normalizeLookProfile(m_keymapProfile);
    m_session->applyProfile(m_keymapProfile, true);
    m_keymapDirty = false;
    refreshKeymapSummary();
    reloadKeymapProfiles();
    showHud(QStringLiteral("键位方案已保存"));
}

void ConsolePage::addKeymapBinding() {
    KeyBinding binding;
    binding.vk = 0x47; // G
    binding.center = QPointF(0.5, 0.5);
    binding.radius = 0.045;
    m_keymapProfile.bindings.append(binding);
    m_keymapSelected = m_keymapProfile.bindings.size() - 1;
    setRegionTarget(0);
    m_overlay->setProfile(m_keymapProfile);
    m_overlay->setSelected(m_keymapSelected);
    refreshKeymapProperties();
    markKeymapDirty();
    startKeyCapture();
}

void ConsolePage::deleteKeymapBinding() {
    const int index = selectedBinding();
    if (index < 0) {
        return;
    }
    m_keymapProfile.bindings.removeAt(index);
    m_keymapSelected = -1;
    m_overlay->setProfile(m_keymapProfile);
    m_overlay->setSelected(-1);
    refreshKeymapProperties();
    markKeymapDirty();
}

void ConsolePage::startKeyCapture() {
    if (selectedBinding() < 0 || m_capturingKey) {
        return;
    }
    m_capturingKey = true;
    m_keyCaptureButton->setText(QStringLiteral("请按键…"));
    if (m_cancelKeyCaptureButton) {
        m_cancelKeyCaptureButton->setVisible(true);
    }
    if (m_keyCaptureTimer) {
        m_keyCaptureTimer->start();
    }
    qApp->installEventFilter(this);
}

void ConsolePage::stopKeyCapture() {
    if (!m_capturingKey) {
        return;
    }
    m_capturingKey = false;
    qApp->removeEventFilter(this);
    if (m_keyCaptureTimer) {
        m_keyCaptureTimer->stop();
    }
    if (m_cancelKeyCaptureButton) {
        m_cancelKeyCaptureButton->setVisible(false);
    }
    refreshKeymapProperties();
}

void ConsolePage::applyBindingFromUi() {
    // 面板回填期间不回写（refreshKeymapProperties 的钳制信号会误写，见 S3）。
    if (m_syncingBindingUi) {
        return;
    }
    if (m_regionTarget != 0) {
        const QPointF center(m_xSpin->value() / 1000.0, m_ySpin->value() / 1000.0);
        if (m_regionTarget == 1) {
            // 小圆 ≤ 大圆：任一侧调整时交叉钳制。
            const double inner = std::clamp(m_sizeSlider->value() / 1000.0, 0.02, 0.30);
            const double outer =
                std::clamp(m_sprintSizeSlider->value() / 1000.0, inner, 0.30);
            m_keymapProfile.joystickRadius = inner;
            m_keymapProfile.joystickSprintRadius = outer;
        } else {
            m_keymapProfile.lookRadius =
                std::clamp(m_sizeSlider->value() / 1000.0, 0.05, 0.45);
            m_keymapProfile.lookCenter =
                clampNormalizedRegionCenter(center, m_keymapProfile.lookRadius);
        }
        m_overlay->setProfile(m_keymapProfile);
        m_overlay->setSelectedRegion(
            m_regionTarget == 1 ? KeymapOverlay::Region::Joystick : KeymapOverlay::Region::Look);
        markKeymapDirty();
        return;
    }
    const int index = selectedBinding();
    if (index < 0 || m_capturingKey) {
        return;
    }
    KeyBinding &binding = m_keymapProfile.bindings[index];
    binding.center = QPointF(m_xSpin->value() / 1000.0, m_ySpin->value() / 1000.0);
    binding.radius = m_sizeSlider->value() / 1000.0;
    m_overlay->setProfile(m_keymapProfile);
    markKeymapDirty();
}

// ---------------------------------------------------------------------------
// HUD / 提示
// ---------------------------------------------------------------------------
void ConsolePage::showHud(const QString &text, bool persistent) {
    m_hud->setText(text);
    m_hud->adjustSize();
    const QRect videoRect = m_video->videoRect();
    const QRect anchor = videoRect.isEmpty() ? m_video->rect() : videoRect;
    m_hud->move(anchor.left() + 12, anchor.bottom() - m_hud->height() - 12);
    m_hud->show();
    m_hud->raise();
    if (persistent) {
        m_hudTimer->stop();
    } else {
        hideHudSoon();
    }
}

void ConsolePage::hideHudSoon() { m_hudTimer->start(3000); }

void ConsolePage::showSummonHint() {
    if (!m_summonHint || m_summonHintShown) {
        return;
    }
    m_summonHintShown = true;
    updateSummonText();
    m_summonHint->adjustSize();
    layoutVideoOverlays();
    m_summonHint->show();
    m_summonHint->raise();
    m_summonHintTimer->start(5000);
}

QString ConsolePage::pointerHudText() const {
    if (m_summonMode == SummonMode::Hold) {
        return QStringLiteral("鼠标模式 · 松开 %1 返回游戏").arg(vkName(m_summonKey));
    }
    return QStringLiteral("鼠标模式 · 点击手机画面操作 · 再按 %1 返回").arg(vkName(m_summonKey));
}

void ConsolePage::updateSummonText() {
    if (!m_summonHint) {
        return;
    }
    const QString action =
        m_summonMode == SummonMode::Hold ? QStringLiteral("按住") : QStringLiteral("长按");
    m_summonHint->setText(QStringLiteral("%1 %2 呼出鼠标").arg(action, vkName(m_summonKey)));
    m_summonHint->adjustSize();
}

// ---------------------------------------------------------------------------
// 事件
// ---------------------------------------------------------------------------
bool ConsolePage::eventFilter(QObject *watched, QEvent *event) {
    // 按键捕获优先（qApp 级过滤器，watched 为任意对象）
    if (m_capturingKey) {
        if (event->type() == QEvent::KeyPress) {
            auto *keyEvent = static_cast<QKeyEvent *>(event);
            if (!keyEvent->isAutoRepeat() && selectedBinding() >= 0) {
                m_keymapProfile.bindings[m_keymapSelected].vk = vkFromKeyEvent(*keyEvent);
                stopKeyCapture();
                m_overlay->setProfile(m_keymapProfile);
                refreshKeymapProperties();
                            markKeymapDirty();
                return true;
            }
            return true;
        }
        if (event->type() == QEvent::Wheel) {
            // 滚轮上下也可绑定为键位：向上=滚轮上、向下=滚轮下
            auto *wheelEvent = static_cast<QWheelEvent *>(event);
            const int angle = wheelEvent->angleDelta().y();
            if (angle != 0 && selectedBinding() >= 0) {
                m_keymapProfile.bindings[m_keymapSelected].vk =
                    angle > 0 ? ks::kVkWheelUp : ks::kVkWheelDown;
                stopKeyCapture();
                m_overlay->setProfile(m_keymapProfile);
                refreshKeymapProperties();
                markKeymapDirty();
            }
            return true;
        }
        if (event->type() == QEvent::MouseButtonPress) {
            if (watched == m_cancelKeyCaptureButton) {
                return QWidget::eventFilter(watched, event);
            }
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
            if (vk && selectedBinding() >= 0) {
                m_keymapProfile.bindings[m_keymapSelected].vk = vk;
                stopKeyCapture();
                m_overlay->setProfile(m_keymapProfile);
                refreshKeymapProperties();
                            markKeymapDirty();
                return true;
            }
        }
    }

    if (watched == m_qualityPanel && event->type() == QEvent::MouseButtonPress &&
        !LicenseManager::instance().isPro()) {
        QMessageBox::information(
            this, QStringLiteral("Pro 功能"),
            QStringLiteral("画面增强为 Pro 功能，激活后解锁。可在「设置 → 激活 Pro」中输入激活码。"));
        return true;
    }

    if (watched == m_video) {
        if (event->type() == QEvent::Resize) {
            layoutVideoOverlays();
        } else if (event->type() == QEvent::KeyPress && m_mode == InputMode::Pointer) {
            auto *keyEvent = static_cast<QKeyEvent *>(event);
            // Esc 不再退出指针模式：留给屏幕操控（可绑定键位）；退出鼠标模式用呼出键/工具栏按钮
            const int vk = vkFromKeyEvent(*keyEvent);
            if (vk && vkMatchesSummon(vk, m_summonKey) && !keyEvent->isAutoRepeat()) {
                if (m_summonMode == SummonMode::LongPress) {
                    setInputMode(InputMode::Mapping);
                }
                return true;
            }
        } else if (event->type() == QEvent::KeyRelease && m_mode == InputMode::Pointer) {
            auto *keyEvent = static_cast<QKeyEvent *>(event);
            const int vk = vkFromKeyEvent(*keyEvent);
            if (vk && vkMatchesSummon(vk, m_summonKey) && !keyEvent->isAutoRepeat()) {
                if (m_summonMode == SummonMode::Hold) {
                    setInputMode(InputMode::Mapping);
                }
                return true;
            }
        } else if (event->type() == QEvent::Wheel) {
            auto *wheelEvent = static_cast<QWheelEvent *>(event);
            const QPoint pos = wheelEvent->position().toPoint();
            const int angle = wheelEvent->angleDelta().y();
            if (angle == 0) {
                return true;
            }
            const int delta = angle > 0 ? 1 : -1;
            const bool mappingMode =
                m_mode == InputMode::Mapping || m_mode == InputMode::Pointer;
            // 滚轮已绑定为上/下滚轮键位：作为游戏动作触发（逐格派发）。
            if (mappingMode && wheelBound(delta)) {
                const int steps = std::clamp(std::abs(angle) / 120, 1, 3);
                for (int i = 0; i < steps; ++i) {
                    InputEvent wheel;
                    wheel.type = InputEventType::Wheel;
                    wheel.dx = delta;
                    m_session->postInputEvent(wheel);
                }
                return true;
            }
            if (canvasTouchActive()) {
                // 直操模式滚轮仅在画面矩形内注入；信箱黑边与画面外忽略（R5/R6）。
                const QRect rect = m_video->videoRect();
                QPoint device;
                if (!rect.isEmpty() && rect.contains(pos) && mapToDevice(pos, &device)) {
                    const QSize size = deviceSize();
                    m_session->sendControlMessage(proto::scroll(
                        device.x(), device.y(), size.width(), size.height(), 0.0f,
                        static_cast<float>(delta)));
                }
                return true;
            }
            // 未绑定且非直操（游戏态）：不注入滚动。手机端把滚动当作多指手势，
            // 会与视角的单指拖动冲突，导致滚轮后视角不再响应。
            return true;
        } else if (canvasTouchActive() &&
                   (event->type() == QEvent::MouseButtonPress ||
                    event->type() == QEvent::MouseButtonDblClick)) {
            // 双击间隔内的第二次按下 Qt 投递为 MouseButtonDblClick 而非 Press，
            // 必须同样按「手指按下」处理，否则连点/双击丢触（S4）。
            auto *mouseEvent = static_cast<QMouseEvent *>(event);
            if (mouseEvent->button() == Qt::LeftButton) {
                const QRect rect = m_video->videoRect();
                m_draggingTouch = rect.contains(mouseEvent->pos());
                if (m_draggingTouch) {
                    // 左键按下即触点按下；拖动由 MouseMove 持续注入，模拟手指滑动。
                    m_session->setTouchDragActive(true);
                    sendPointerTouch(static_cast<int>(proto::TouchAction::Down),
                                     mouseEvent->pos(), false);
                }
                return true;
            }
            if (mouseEvent->button() == Qt::MiddleButton ||
                mouseEvent->button() == Qt::RightButton) {
                const int vk = mouseEvent->button() == Qt::MiddleButton ? 0x04 : 0x02;
                if (m_mode == InputMode::Pointer && m_summonMode == SummonMode::LongPress &&
                    vkMatchesSummon(vk, m_summonKey)) {
                    setInputMode(InputMode::Mapping);
                }
                return true;
            }
        } else if (m_draggingTouch && event->type() == QEvent::MouseMove) {
            auto *mouseEvent = static_cast<QMouseEvent *>(event);
            sendPointerTouch(static_cast<int>(proto::TouchAction::Move), mouseEvent->pos(), true);
            return true;
        } else if (event->type() == QEvent::MouseButtonRelease) {
            auto *mouseEvent = static_cast<QMouseEvent *>(event);
            if (mouseEvent->button() == Qt::LeftButton && m_draggingTouch) {
                sendPointerTouch(static_cast<int>(proto::TouchAction::Up), mouseEvent->pos(), true);
                m_draggingTouch = false;
                m_session->setTouchDragActive(false);
                return true;
            }
            if (m_mode == InputMode::Pointer && m_summonMode == SummonMode::Hold &&
                (mouseEvent->button() == Qt::MiddleButton ||
                 mouseEvent->button() == Qt::RightButton)) {
                const int vk = mouseEvent->button() == Qt::MiddleButton ? 0x04 : 0x02;
                if (vkMatchesSummon(vk, m_summonKey)) {
                    setInputMode(InputMode::Mapping);
                }
                return true;
            }
        }
    }

    return QWidget::eventFilter(watched, event);
}

void ConsolePage::resizeEvent(QResizeEvent *event) {
    QWidget::resizeEvent(event);
    if (m_inspector) {
        m_inspector->setFixedWidth(width() < 1100 ? 280 : 320);
    }
    layoutVideoOverlays();
}

void ConsolePage::setGameMode(bool enabled) {
    if (m_gameMode == enabled) {
        return;
    }
    // 切换全屏前先抬起画布触点，避免手机端手指卡在按下状态。
    cancelTouchDrag();
    m_gameMode = enabled;
    if (m_toolbar) {
        m_toolbar->setVisible(!enabled);
    }
    if (m_inspector) {
        m_inspector->setVisible(!enabled);
    }
    if (m_statusBar) {
        m_statusBar->setVisible(!enabled);
    }

    if (enabled) {
        m_video->setFocus();
        // 全屏默认游戏模式（S5）：指针态自动切回映射。
        if (m_mode == InputMode::Pointer) {
            setInputMode(InputMode::Mapping);
        }
        if (m_mode == InputMode::Mapping) {
            const QString action = m_summonMode == SummonMode::Hold ? QStringLiteral("按住")
                                                                    : QStringLiteral("长按");
            showHud(QStringLiteral("%1 %2 呼出鼠标 · F11 退出全屏")
                        .arg(action, vkName(m_summonKey)));
        }
    } else {
        // 窗口默认鼠标模式（S5）。
        if (m_session && m_session->state() == Session::State::Streaming &&
            m_mode != InputMode::Edit) {
            setInputMode(InputMode::Pointer);
        }
        m_hud->hide();
    }
    updateCursor();
    updateInputCapture();
    updateStatsOverlay();
    layoutVideoOverlays();
}

void ConsolePage::setStatsOverlayEnabled(bool enabled) {
    m_statsOverlayEnabled = enabled;
    updateStatsOverlay();
}

void ConsolePage::onSprintToggled(bool active) {
    if (m_gameMode && m_mode == InputMode::Mapping) {
        showHud(active ? QStringLiteral("疾跑已开启") : QStringLiteral("疾跑已关闭"));
    }
}

void ConsolePage::updateStatsOverlay() {
    if (!m_statsOverlay) {
        return;
    }
    const bool show = m_gameMode && m_statsOverlayEnabled && m_session &&
                      m_session->state() == Session::State::Streaming;
    m_statsOverlay->setVisible(show);
    if (show) {
        m_statsOverlay->raise();
        layoutVideoOverlays();
    }
}

void ConsolePage::setBypassEnhance(bool bypass) { m_video->setBypassEnhance(bypass); }

} // namespace ks::ui
