#pragma once

#include <QMainWindow>
#include <QPoint>

class QButtonGroup;
class QHBoxLayout;
class QLabel;
class QStackedWidget;
class QTimer;
class QVBoxLayout;
class QWidget;

namespace ks {
class Session;
}

namespace ks::ui {

class ConsolePage;

/// 应用外壳：自绘标题栏、左侧导航、页面堆栈、全屏游戏模式。
class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(Session *session, QWidget *parent = nullptr);
    ~MainWindow() override;

    void setGameMode(bool enabled);

protected:
    bool nativeEvent(const QByteArray &eventType, void *message, qintptr *result) override;
    bool event(QEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void closeEvent(QCloseEvent *event) override;
    void showEvent(QShowEvent *event) override;

private:
    void buildTitleBar(QWidget *root, QVBoxLayout *layout);
    void buildSidebar(QWidget *body, QHBoxLayout *layout);
    void buildPages(QWidget *body, QHBoxLayout *layout);
    void navigateTo(int index);
    void showToast(const QString &text);
    /// 按当前授权状态刷新侧边栏徽标。
    void refreshLicenseBadge();
    /// 全屏游戏模式：把系统光标限制在窗口内，避免移动到其他屏幕。
    void updateCursorClip();
    void releaseCursorClip();

    Session *m_session = nullptr;
    QWidget *m_central = nullptr;
    QWidget *m_titleBar = nullptr;
    QWidget *m_sidebar = nullptr;
    QStackedWidget *m_pages = nullptr;
    QButtonGroup *m_navGroup = nullptr;
    ConsolePage *m_console = nullptr;
    QLabel *m_toast = nullptr;
    QTimer *m_toastTimer = nullptr;
    QLabel *m_licenseBadge = nullptr;

    bool m_gameMode = false;
    bool m_inputWindowAttached = false;
    int m_currentPage = 0;
    qint64 m_lastEscMs = 0; ///< 上次全屏内按下 Esc 的时刻（双击 Esc 退出全屏）
};

} // namespace ks::ui
