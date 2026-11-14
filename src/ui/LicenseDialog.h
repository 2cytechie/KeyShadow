#pragma once

#include <QDialog>

class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;

namespace ks::ui {

/// 离线授权激活对话框：展示本机指纹、导入 license.json、移除本机授权。
class LicenseDialog : public QDialog {
    Q_OBJECT

public:
    explicit LicenseDialog(QWidget *parent = nullptr);

signals:
    /// 授权状态发生变化（激活成功或移除）。
    void licenseChanged();

private:
    void refreshState();
    void setStatusText(const QString &text, const QString &state);
    void importFromFile();
    void importFromPastedText();
    void removeLicense();
    void applyLicense(const QByteArray &content);

    QLineEdit *m_fingerprintEdit = nullptr;
    QPlainTextEdit *m_pasteEdit = nullptr;
    QLabel *m_statusLabel = nullptr;
    QPushButton *m_importFileButton = nullptr;
    QPushButton *m_importPasteButton = nullptr;
    QPushButton *m_removeButton = nullptr;
};

} // namespace ks::ui
