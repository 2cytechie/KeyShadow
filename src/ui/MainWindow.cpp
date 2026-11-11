#include "ui/MainWindow.h"

#include "app/Session.h"
#include "license/LicenseManager.h"
#include "ui/IconFactory.h"
#include "ui/KeyNames.h"
#include "ui/Theme.h"
#include "ui/pages/ConsolePage.h"
#include "ui/pages/DevicePage.h"
#include "ui/pages/SettingsPage.h"

#include <QApplication>
#include <QButtonGroup>
#include <QCloseEvent>
#include <QEvent>
#include <QFrame>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPushButton>
#include <QScreen>
#include <QSettings>
#include <QShowEvent>
#include <QStackedWidget>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWindow>

#include <qt_windows.h>
#include <windowsx.h>

namespace ks::ui {
namespace {

QPushButton *makeNavButton(const QString &text, const QString &icon, QWidget *parent) {
    auto *button = new QPushButton(text, parent);
    button->setCheckable(true);
    button->setProperty("nav", "true");
    button->setIcon(IconFactory::icon(icon, Theme::textSecondary(), 18));
    button->setIconSize(QSize(18, 18));
    button->setCursor(Qt::PointingHandCursor);
    return button;
}

} // namespace

MainWindow::MainWindow(Session *session, QWidget *parent) : QMainWindow(parent), m_session(session) {
    setWindowTitle(QStringLiteral("键影 KeyShadow"));
    setWindowFlag(Qt::FramelessWindowHint, true);
    setMinimumSize(1024, 640);
    resize(1280, 800);

    m_central = new QWidget(this);
    m_central->setObjectName(QStringLiteral("AppRoot"));
    setCentralWidget(m_central);

    auto *root = new QVBoxLayout(m_central);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    buildTitleBar(m_central, root);

    auto *body = new QWidget(m_central);
    auto *bodyLayout = new QHBoxLayout(body);
    bodyLayout->setContentsMargins(0, 0, 0, 0);
    bodyLayout->setSpacing(0);

    buildSidebar(body, bodyLayout);
    buildPages(body, bodyLayout);
    root->addWidget(body, 1);

    // Toast
    m_toast = new QLabel(m_central);
    m_toast->setStyleSheet(QStringLiteral(
        "background: #1A1F26; color: #E8EDF2; padding: 8px 14px; border-radius: 6px;"));
    m_toast->setAttribute(Qt::WA_TransparentForMouseEvents);
    m_toast->hide();
    m_toastTimer = new QTimer(this);
    m_toastTimer->setSingleShot(true);
    m_toastTimer->setInterval(4000);
    connect(m_toastTimer, &QTimer::timeout, m_toast, &QLabel::hide);

    if (m_session) {
        connect(m_session, &Session::logMessage, this, &MainWindow::showToast);
        connect(m_session, &Session::stateChanged, this,
                [this](Session::State state, const QString &) {
                    if (state == Session::State::Streaming) {
                        navigateTo(1);
                    } else if (state == Session::State::Error) {
                        navigateTo(0);
                    }
                });

        QSettings settings;
        const QByteArray geometry = settings.value(QStringLiteral("ui/geometry")).toByteArray();
        if (!geometry.isEmpty()) {
            restoreGeometry(geometry);
        }

        // 确保窗口落在可见屏幕内（多显示器/DPI 变化后恢复几何可能越界）
        const QRect available = QGuiApplication::primaryScreen()->availableGeometry();
        if (!available.contains(frameGeometry().center()) ||
            frameGeometry().width() > available.width() ||
            frameGeometry().height() > available.height()) {
            resize(std::min(1280, available.width()), std::min(800, available.height()));
            move(available.center() -
                 QPoint(width() / 2, height() / 2));
        }
    }

    // 主窗口句柄就绪后注册 Raw Input（showEvent 中完成）
    m_titleBar->installEventFilter(this);

    setFocusPolicy(Qt::StrongFocus);
}

MainWindow::~MainWindow() {
    releaseCursorClip();
}

void MainWindow::buildTitleBar(QWidget *root, QVBoxLayout *layout) {
    m_titleBar = new QWidget(root);
    m_titleBar->setObjectName(QStringLiteral("TitleBar"));
    m_titleBar->setFixedHeight(40);

    auto *bar = new QHBoxLayout(m_titleBar);
    bar->setContentsMargins(12, 0, 0, 0);
    bar->setSpacing(8);

    auto *mark = new QLabel(m_titleBar);
    mark->setPixmap(IconFactory::brandMark(Theme::brandPrimary(), 18));
    auto *title = new QLabel(QStringLiteral("键影 KeyShadow"), m_titleBar);
    title->setObjectName(QStringLiteral("BrandName"));
    bar->addWidget(mark);
    bar->addWidget(title);
    bar->addStretch(1);

    auto makeWindowButton = [this](const QString &icon, const QString &objectName) {
        auto *button = new QToolButton(m_titleBar);
        button->setObjectName(objectName);
        button->setIcon(IconFactory::icon(icon, Theme::textSecondary(), 16));
        button->setFixedSize(46, 40);
        return button;
    };

    auto *minimize = makeWindowButton(QStringLiteral("minimize"), QStringLiteral("WindowButton"));
    auto *maximize = makeWindowButton(QStringLiteral("maximize"), QStringLiteral("WindowButton"));
    auto *closeButton = makeWindowButton(QStringLiteral("close"), QStringLiteral("WindowClose"));
    bar->addWidget(minimize);
    bar->addWidget(maximize);
    bar->addWidget(closeButton);

    connect(minimize, &QToolButton::clicked, this, &QWidget::showMinimized);
    connect(maximize, &QToolButton::clicked, this, [this] {
        isMaximized() ? showNormal() : showMaximized();
    });
    connect(closeButton, &QToolButton::clicked, this, &QWidget::close);

    layout->addWidget(m_titleBar);
}

void MainWindow::buildSidebar(QWidget *body, QHBoxLayout *layout) {
    m_sidebar = new QWidget(body);
    m_sidebar->setObjectName(QStringLiteral("Sidebar"));
    m_sidebar->setFixedWidth(220);

    auto *column = new QVBoxLayout(m_sidebar);
    column->setContentsMargins(12, 16, 12, 12);
    column->setSpacing(4);

    m_navGroup = new QButtonGroup(this);
    m_navGroup->setExclusive(true);

    const QVector<QPair<QString, QString>> items = {
        {QStringLiteral("设备"), QStringLiteral("device")},
        {QStringLiteral("投屏"), QStringLiteral("monitor")},
        {QStringLiteral("设置"), QStringLiteral("settings")},
    };

    for (int i = 0; i < items.size(); ++i) {
        auto *button = makeNavButton(items.at(i).first, items.at(i).second, m_sidebar);
        m_navGroup->addButton(button, i);
        column->addWidget(button);
    }
    column->addStretch(1);

    auto *divider = new QFrame(m_sidebar);
    divider->setObjectName(QStringLiteral("Divider"));
    column->addWidget(divider);

    auto *version = new QLabel(QStringLiteral("v0.1.0"), m_sidebar);
    version->setProperty("variant", "micro");
    column->addWidget(version);

    m_licenseBadge = new QLabel(m_sidebar);
    const bool pro = LicenseManager::instance().isPro();
    m_licenseBadge->setText(pro ? QStringLiteral("PRO") : QStringLiteral("基础版"));
    m_licenseBadge->setProperty("badge", pro ? "pro" : "basic");
    m_licenseBadge->setMaximumWidth(64);
    column->addWidget(m_licenseBadge);

    connect(m_navGroup, QOverload<int>::of(&QButtonGroup::idClicked), this,
            [this](int id) { navigateTo(id); });

    layout->addWidget(m_sidebar);
}

void MainWindow::buildPages(QWidget *body, QHBoxLayout *layout) {
    m_pages = new QStackedWidget(body);

    auto *devicePage = new DevicePage(m_session, m_pages);
    m_console = new ConsolePage(m_session, m_pages);
    auto *settingsPage = new SettingsPage(m_session, m_pages);

    m_pages->addWidget(devicePage);
    m_pages->addWidget(m_console);
    m_pages->addWidget(settingsPage);

    layout->addWidget(m_pages, 1);

    connect(devicePage, &DevicePage::connectRequested, this, [this](const QString &serial) {
        navigateTo(1);
        m_session->connectDevice(serial);
    });
    connect(m_console, &ConsolePage::toggleGameMode, this,
            [this] { setGameMode(!m_gameMode); });
    connect(settingsPage, &SettingsPage::summonKeyChanged, this, [this](int vk) {
        m_session->setSummonKey(vk);
        m_console->setSummonKey(vk);
        showToast(QStringLiteral("呼出键已改为 %1").arg(vkName(vk)));
    });
    connect(settingsPage, &SettingsPage::summonModeChanged, m_console,
            &ConsolePage::setSummonMode);
    connect(m_session, &Session::summonKeyPressed, m_console,
            &ConsolePage::onSummonKeyPressed);

    m_console->setSummonKey(m_session->summonKey());

    // 开发期联调：KEYSHADOW_AUTO_KEYMAP_EDIT=1 在自动连接后进入键位编辑，便于截图验证。
    if (qEnvironmentVariableIntValue("KEYSHADOW_AUTO_KEYMAP_EDIT") == 1) {
        auto *autoEditTimer = new QTimer(this);
        autoEditTimer->setSingleShot(true);
        autoEditTimer->setInterval(4000);
        connect(autoEditTimer, &QTimer::timeout, this, [this] {
            if (m_console && m_console->isVisible()) {
                m_console->beginKeymapEdit();
            }
        });
        connect(m_session, &Session::stateChanged, this,
                [autoEditTimer](Session::State state, const QString &) {
                    if (state == Session::State::Streaming) {
                        autoEditTimer->start();
                    }
                });
    }

    if (m_navGroup->button(0)) {
        m_navGroup->button(0)->setChecked(true);
    }
    m_pages->setCurrentIndex(0);
    m_currentPage = 0;

    // 开发期联调：KEYSHADOW_AUTO_PAGE=settings 时直接打开指定页面（便于截图验证）。
    const QString autoPage = qEnvironmentVariable("KEYSHADOW_AUTO_PAGE");
    if (autoPage == QStringLiteral("settings")) {
        navigateTo(2);
    } else if (autoPage == QStringLiteral("device")) {
        navigateTo(0);
    }
}

void MainWindow::navigateTo(int index) {
    if (!m_pages || index < 0 || index >= m_pages->count()) {
        return;
    }
    // 离开投屏页：先让键位编辑确认并暂停全部输入捕获
    if (m_currentPage == 1 && index != 1 && m_console) {
        if (!m_console->requestDeactivate()) {
            if (auto *button = m_navGroup->button(m_currentPage)) {
                button->setChecked(true);
            }
            return;
        }
    }
    if (index == 1 && m_console) {
        m_console->setActive(true);
    }
    m_pages->setCurrentIndex(index);
    m_currentPage = index;
    if (auto *button = m_navGroup->button(index)) {
        button->setChecked(true);
    }
}

void MainWindow::showToast(const QString &text) {
    if (!m_toast || text.isEmpty()) {
        return;
    }
    m_toast->setText(text);
    m_toast->adjustSize();
    m_toast->move((m_central->width() - m_toast->width()) / 2, m_central->height() - 56);
    m_toast->show();
    m_toast->raise();
    m_toastTimer->start();
}

void MainWindow::setGameMode(bool enabled) {
    if (m_gameMode == enabled) {
        return;
    }
    m_gameMode = enabled;

    m_titleBar->setVisible(!enabled);
    m_sidebar->setVisible(!enabled);
    if (m_console) {
        m_console->setGameMode(enabled);
    }

    if (enabled) {
        showFullScreen();
        // 全屏几何在下一拍才稳定，之后再约束光标。
        QTimer::singleShot(0, this, [this] { updateCursorClip(); });
    } else {
        releaseCursorClip();
        showNormal();
    }
}

void MainWindow::updateCursorClip() {
    if (!m_gameMode || !isFullScreen()) {
        return;
    }
    const QRect frame = frameGeometry();
    if (frame.isEmpty()) {
        return;
    }
    RECT rect{frame.left(), frame.top(), frame.right() + 1, frame.bottom() + 1};
    ClipCursor(&rect);
}

void MainWindow::releaseCursorClip() { ClipCursor(nullptr); }

bool MainWindow::event(QEvent *event) {
    switch (event->type()) {
    case QEvent::WindowActivate:
        if (m_gameMode) {
            updateCursorClip();
        }
        break;
    case QEvent::WindowDeactivate:
        releaseCursorClip();
        break;
    case QEvent::Resize:
        if (m_gameMode && isFullScreen()) {
            updateCursorClip();
        }
        break;
    default:
        break;
    }
    return QMainWindow::event(event);
}

void MainWindow::showEvent(QShowEvent *event) {
    QMainWindow::showEvent(event);
    if (!m_inputWindowAttached && m_session) {
        m_session->attachInputWindow(static_cast<quintptr>(winId()));
        m_inputWindowAttached = true;
    }
}

bool MainWindow::eventFilter(QObject *watched, QEvent *event) {
    if (watched == m_titleBar) {
        if (event->type() == QEvent::MouseButtonPress) {
            auto *mouseEvent = static_cast<QMouseEvent *>(event);
            if (mouseEvent->button() == Qt::LeftButton) {
                if (windowHandle()) {
                    windowHandle()->startSystemMove();
                }
                return true;
            }
        } else if (event->type() == QEvent::MouseButtonDblClick) {
            isMaximized() ? showNormal() : showMaximized();
            return true;
        }
    }
    return QMainWindow::eventFilter(watched, event);
}

void MainWindow::keyPressEvent(QKeyEvent *event) {
    if (event->key() == Qt::Key_F9) {
        if (m_console) {
            if (m_gameMode) {
                setGameMode(false);
            }
            navigateTo(1);
            m_console->beginKeymapEdit();
        }
        return;
    }
    if (event->key() == Qt::Key_F11) {
        if (m_console && m_console->isEditing()) {
            return;
        }
        setGameMode(!m_gameMode);
        return;
    }
    if (event->key() == Qt::Key_Escape && m_gameMode) {
        if (m_console && m_console->isEditing()) {
            return;
        }
        setGameMode(false);
        return;
    }
    QMainWindow::keyPressEvent(event);
}

void MainWindow::closeEvent(QCloseEvent *event) {
    QSettings settings;
    settings.setValue(QStringLiteral("ui/geometry"), saveGeometry());
    if (m_session) {
        m_session->disconnectDevice();
    }
    QMainWindow::closeEvent(event);
}

bool MainWindow::nativeEvent(const QByteArray &eventType, void *message, qintptr *result) {
    if (eventType == "windows_generic_MSG" && !m_gameMode) {
        auto *msg = static_cast<MSG *>(message);
        if (msg->message == WM_NCHITTEST) {
            const int border = 6;
            const long x = GET_X_LPARAM(msg->lParam);
            const long y = GET_Y_LPARAM(msg->lParam);
            RECT rect{};
            GetWindowRect(reinterpret_cast<HWND>(winId()), &rect);
            const bool left = x - rect.left < border;
            const bool right = rect.right - x < border;
            const bool top = y - rect.top < border;
            const bool bottom = rect.bottom - y < border;

            if (top && left) {
                *result = HTTOPLEFT;
                return true;
            }
            if (top && right) {
                *result = HTTOPRIGHT;
                return true;
            }
            if (bottom && left) {
                *result = HTBOTTOMLEFT;
                return true;
            }
            if (bottom && right) {
                *result = HTBOTTOMRIGHT;
                return true;
            }
            if (left) {
                *result = HTLEFT;
                return true;
            }
            if (right) {
                *result = HTRIGHT;
                return true;
            }
            if (top) {
                *result = HTTOP;
                return true;
            }
            if (bottom) {
                *result = HTBOTTOM;
                return true;
            }
        }
    }
    return QMainWindow::nativeEvent(eventType, message, result);
}

} // namespace ks::ui
