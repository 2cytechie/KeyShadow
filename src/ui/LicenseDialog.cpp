#include "ui/LicenseDialog.h"

#include "license/LicenseManager.h"
#include "ui/Theme.h"

#include <QApplication>
#include <QClipboard>
#include <QDesktopServices>
#include <QFile>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QStyle>
#include <QUrl>
#include <QVBoxLayout>

namespace ks::ui {

LicenseDialog::LicenseDialog(QWidget *parent) : QDialog(parent) {
    setWindowTitle(QStringLiteral("激活 键影"));
    setModal(true);
    resize(600, 460);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(20, 20, 20, 16);
    layout->setSpacing(12);

    auto *intro = new QLabel(
        QStringLiteral("离线激活：把下方「本机指纹」发给卖家，收到 license.json 后在此导入。"
                       "授权与本机绑定，换机需重新签发。"),
        this);
    intro->setWordWrap(true);
    intro->setProperty("variant", "secondary");
    layout->addWidget(intro);

    auto *fingerprintRow = new QHBoxLayout();
    fingerprintRow->addWidget(new QLabel(QStringLiteral("本机指纹"), this));
    m_fingerprintEdit = new QLineEdit(LicenseManager::instance().machineFingerprint(), this);
    m_fingerprintEdit->setReadOnly(true);
    m_fingerprintEdit->setFont(Theme::monoFont(12));
    fingerprintRow->addWidget(m_fingerprintEdit, 1);
    auto *copyButton = new QPushButton(QStringLiteral("复制"), this);
    copyButton->setFixedWidth(64);
    fingerprintRow->addWidget(copyButton);
    layout->addLayout(fingerprintRow);

    m_statusLabel = new QLabel(this);
    m_statusLabel->setWordWrap(true);
    layout->addWidget(m_statusLabel);

    auto *importRow = new QHBoxLayout();
    m_importFileButton = new QPushButton(QStringLiteral("选择许可证文件…"), this);
    importRow->addWidget(m_importFileButton);
    importRow->addStretch(1);
    m_removeButton = new QPushButton(QStringLiteral("移除本机授权"), this);
    importRow->addWidget(m_removeButton);
    layout->addLayout(importRow);

    layout->addWidget(new QLabel(QStringLiteral("或粘贴许可证内容："), this));
    m_pasteEdit = new QPlainTextEdit(this);
    m_pasteEdit->setPlaceholderText(QStringLiteral("{\n  \"payload\": \"...\",\n  \"sig\": \"...\"\n}"));
    m_pasteEdit->setFont(Theme::monoFont(11));
    layout->addWidget(m_pasteEdit, 1);

    auto *bottomRow = new QHBoxLayout();
    // 左下角：购买入口，打开系统默认浏览器。
    auto *purchaseButton = new QPushButton(QStringLiteral("购买永久会员"), this);
    purchaseButton->setProperty("variant", "primary");
    bottomRow->addWidget(purchaseButton);
    bottomRow->addStretch(1);
    m_importPasteButton = new QPushButton(QStringLiteral("导入粘贴内容"), this);
    auto *closeButton = new QPushButton(QStringLiteral("关闭"), this);
    bottomRow->addWidget(m_importPasteButton);
    bottomRow->addWidget(closeButton);
    layout->addLayout(bottomRow);

    connect(copyButton, &QPushButton::clicked, this, [this] {
        QApplication::clipboard()->setText(m_fingerprintEdit->text());
        setStatusText(QStringLiteral("已复制本机指纹，可发送给卖家"), QStringLiteral("success"));
    });
    connect(purchaseButton, &QPushButton::clicked, this, [] {
        QDesktopServices::openUrl(
            QUrl(QStringLiteral("https://www.goofish.com/item?id=1098545515862")));
    });
    connect(m_importFileButton, &QPushButton::clicked, this, &LicenseDialog::importFromFile);
    connect(m_importPasteButton, &QPushButton::clicked, this, &LicenseDialog::importFromPastedText);
    connect(m_removeButton, &QPushButton::clicked, this, &LicenseDialog::removeLicense);
    connect(closeButton, &QPushButton::clicked, this, &QDialog::reject);
    connect(m_pasteEdit, &QPlainTextEdit::textChanged, this, [this] {
        m_importPasteButton->setEnabled(!m_pasteEdit->toPlainText().trimmed().isEmpty());
    });

    refreshState();
}

void LicenseDialog::setStatusText(const QString &text, const QString &state) {
    m_statusLabel->setText(text);
    m_statusLabel->setProperty("state", state);
    m_statusLabel->style()->unpolish(m_statusLabel);
    m_statusLabel->style()->polish(m_statusLabel);
}

void LicenseDialog::refreshState() {
    LicenseManager &license = LicenseManager::instance();
    if (license.isPro()) {
        QString detail = QStringLiteral("已激活：PRO · 永久有效");
        if (!license.licenseId().isEmpty()) {
            detail += QStringLiteral(" · %1").arg(license.licenseId());
        }
        if (!license.issuedAtText().isEmpty()) {
            detail += QStringLiteral(" · 签发于 %1").arg(license.issuedAtText());
        }
        setStatusText(detail, QStringLiteral("success"));
    } else {
        const QString error = license.lastError();
        const bool showError = license.lastErrorCode() != LicenseError::FileMissing && !error.isEmpty();
        setStatusText(showError ? QStringLiteral("未激活：%1").arg(error)
                                : QStringLiteral("未激活：当前为基础版"),
                      QStringLiteral("secondary"));
    }
    m_removeButton->setVisible(license.isPro());
    m_importPasteButton->setEnabled(!m_pasteEdit->toPlainText().trimmed().isEmpty());
}

void LicenseDialog::applyLicense(const QByteArray &content) {
    LicenseManager &license = LicenseManager::instance();
    QString error;
    if (!license.activateFromText(content, &error)) {
        refreshState();
        QMessageBox::warning(this, QStringLiteral("激活失败"), error);
        return;
    }
    m_pasteEdit->clear();
    refreshState();
    emit licenseChanged();
    QMessageBox::information(this, QStringLiteral("激活成功"),
                             QStringLiteral("已解锁全部付费功能。"));
    accept();
}

void LicenseDialog::importFromFile() {
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("选择许可证文件"), QString(),
        QStringLiteral("许可证文件 (*.json);;所有文件 (*.*)"));
    if (path.isEmpty()) {
        return;
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        QMessageBox::warning(this, QStringLiteral("激活失败"),
                             QStringLiteral("无法读取文件：%1").arg(path));
        return;
    }
    const QByteArray content = file.readAll();
    file.close();
    applyLicense(content);
}

void LicenseDialog::importFromPastedText() {
    const QByteArray content = m_pasteEdit->toPlainText().toUtf8();
    if (content.trimmed().isEmpty()) {
        return;
    }
    applyLicense(content);
}

void LicenseDialog::removeLicense() {
    const QMessageBox::StandardButton answer = QMessageBox::question(
        this, QStringLiteral("移除本机授权"),
        QStringLiteral("移除后本机将回到基础版，已签发的许可证文件不受影响。确定继续？"),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (answer != QMessageBox::Yes) {
        return;
    }
    LicenseManager::instance().deactivate();
    refreshState();
    emit licenseChanged();
    setStatusText(QStringLiteral("已移除本机授权"), QStringLiteral("secondary"));
}

} // namespace ks::ui
