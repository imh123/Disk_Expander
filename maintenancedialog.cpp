#include "maintenancedialog.h"

#include <QCheckBox>
#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QFont>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QThread>
#include <QUrl>
#include <QVBoxLayout>

MaintenanceDialog::MaintenanceDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(QStringLiteral("释放 C: 空间"));
    resize(820, 620);
    setModal(true);
    buildUi();
}

MaintenanceDialog::~MaintenanceDialog()
{
    if (m_worker)
        m_worker->requestCancel();
    if (m_thread) {
        m_thread->quit();
        m_thread->wait(5000);
    }
}

void MaintenanceDialog::buildUi()
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(18, 16, 18, 14);
    layout->setSpacing(10);

    auto *title = new QLabel(QStringLiteral("释放 C: 空间"), this);
    QFont titleFont = title->font();
    titleFont.setPointSizeF(titleFont.pointSizeF() + 4);
    titleFont.setBold(true);
    title->setFont(titleFont);
    layout->addWidget(title);

    auto *subtitle = new QLabel(
        QStringLiteral("迁移会先复制到目标盘，校验通过后建 junction，最后删除 C: 上的原目录；"
                       "任一步失败会自动回滚。"),
        this);
    subtitle->setWordWrap(true);
    subtitle->setStyleSheet(QStringLiteral("color:#6B7280;"));
    layout->addWidget(subtitle);

    auto *targetRow = new QHBoxLayout;
    targetRow->addWidget(new QLabel(QStringLiteral("迁移目标："), this));
    m_target = new QLineEdit(QStringLiteral("D:\\DiskExpander\\Data"), this);
    m_target->setPlaceholderText(QStringLiteral("例如 D:\\DiskExpander\\Data"));
    auto *browse = new QPushButton(QStringLiteral("浏览…"), this);
    connect(browse, &QPushButton::clicked, this, &MaintenanceDialog::browseTarget);
    targetRow->addWidget(m_target, 1);
    targetRow->addWidget(browse);
    layout->addLayout(targetRow);

    m_hibernate = new QCheckBox(
        QStringLiteral("关闭休眠（释放 hiberfil.sys，通常 4–8 GB，可随时用 powercfg /h on 恢复）"), this);
    m_cleanup = new QCheckBox(
        QStringLiteral("清理临时文件 / Windows 更新缓存 / 回收站（约 0.5–2 GB）"), this);
    m_downloads = new QCheckBox(
        QStringLiteral("迁移 Downloads 到目标盘（当前约 4.8 GB）"), this);
    m_tencent = new QCheckBox(
        QStringLiteral("迁移 Tencent Files（微信/QQ 文件，当前约 5.6 GB）"), this);

    m_hibernate->setChecked(true);
    m_cleanup->setChecked(true);
    m_downloads->setChecked(true);
    m_tencent->setChecked(true);

    layout->addWidget(m_hibernate);
    layout->addWidget(m_cleanup);
    layout->addWidget(m_downloads);
    layout->addWidget(m_tencent);

    auto *warning = new QLabel(
        QStringLiteral("⚠ 迁移前请关闭微信/QQ、下载工具等正在占用目录的程序；迁移期间请勿关机。"
                       "失败会自动回滚，不会丢数据。"),
        this);
    warning->setWordWrap(true);
    warning->setStyleSheet(QStringLiteral("color:#D97706;"));
    layout->addWidget(warning);

    m_progress = new QProgressBar(this);
    m_progress->setRange(0, 1);
    m_progress->setValue(0);
    m_progress->setTextVisible(false);
    m_progress->setFixedHeight(8);
    layout->addWidget(m_progress);

    m_status = new QLabel(QStringLiteral("准备就绪"), this);
    layout->addWidget(m_status);

    m_log = new QPlainTextEdit(this);
    m_log->setReadOnly(true);
    m_log->setMaximumBlockCount(3000);
    QFont logFont(QStringLiteral("Consolas"));
    logFont.setPointSize(9);
    m_log->setFont(logFont);
    layout->addWidget(m_log, 1);

    auto *buttons = new QHBoxLayout;
    m_startButton = new QPushButton(QStringLiteral("开始执行"), this);
    m_openButton = new QPushButton(QStringLiteral("打开目标文件夹"), this);
    m_closeButton = new QPushButton(QStringLiteral("关闭"), this);
    m_openButton->setVisible(false);

    connect(m_startButton, &QPushButton::clicked, this, &MaintenanceDialog::start);
    connect(m_openButton, &QPushButton::clicked, this, &MaintenanceDialog::openTarget);
    connect(m_closeButton, &QPushButton::clicked, this, &QDialog::accept);

    buttons->addWidget(m_startButton);
    buttons->addWidget(m_openButton);
    buttons->addStretch(1);
    buttons->addWidget(m_closeButton);
    layout->addLayout(buttons);
}

void MaintenanceDialog::browseTarget()
{
    const QString directory = QFileDialog::getExistingDirectory(
        this, QStringLiteral("选择迁移目标目录"), m_target->text());
    if (!directory.isEmpty())
        m_target->setText(QDir::toNativeSeparators(directory));
}

void MaintenanceDialog::start()
{
    if (m_running)
        return;

    if (!m_hibernate->isChecked() && !m_cleanup->isChecked()
        && !m_downloads->isChecked() && !m_tencent->isChecked()) {
        QMessageBox::information(this, QStringLiteral("提示"),
                                 QStringLiteral("请至少选择一项操作。"));
        return;
    }

    if ((m_downloads->isChecked() || m_tencent->isChecked())
        && m_target->text().trimmed().isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("提示"),
                             QStringLiteral("请先指定迁移目标目录。"));
        return;
    }

    const auto answer = QMessageBox::warning(
        this,
        QStringLiteral("确认执行"),
        QStringLiteral("将执行选中的操作。迁移会复制文件并删除 C: 上的原目录（失败自动回滚）。\n\n"
                       "确定继续吗？"),
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::No);
    if (answer != QMessageBox::Yes)
        return;

    MaintenanceOptions options;
    options.disableHibernate = m_hibernate->isChecked();
    options.cleanupTemp = m_cleanup->isChecked();
    options.migrateDownloads = m_downloads->isChecked();
    options.migrateTencent = m_tencent->isChecked();
    options.targetRoot = QDir::toNativeSeparators(m_target->text().trimmed());

    int total = 0;
    if (options.disableHibernate) ++total;
    if (options.cleanupTemp) ++total;
    if (options.migrateDownloads) ++total;
    if (options.migrateTencent) ++total;

    m_running = true;
    m_log->clear();
    m_progress->setRange(0, qMax(1, total));
    m_progress->setValue(0);
    m_status->setText(QStringLiteral("正在执行…"));
    m_startButton->setEnabled(false);
    m_closeButton->setEnabled(false);
    m_hibernate->setEnabled(false);
    m_cleanup->setEnabled(false);
    m_downloads->setEnabled(false);
    m_tencent->setEnabled(false);
    m_target->setEnabled(false);

    m_thread = new QThread(this);
    m_worker = new MaintenanceWorker(options);
    m_worker->moveToThread(m_thread);

    connect(m_thread, &QThread::started, m_worker, &MaintenanceWorker::run);
    connect(m_worker, &MaintenanceWorker::logMessage, this, &MaintenanceDialog::onLog);
    connect(m_worker, &MaintenanceWorker::stepStarted, this, &MaintenanceDialog::onStep);
    connect(m_worker, &MaintenanceWorker::finished, this, &MaintenanceDialog::onFinished);
    connect(m_worker, &MaintenanceWorker::finished, m_thread, &QThread::quit);
    connect(m_thread, &QThread::finished, m_worker, &QObject::deleteLater);

    m_thread->start();
}

void MaintenanceDialog::cancelRun()
{
    if (m_worker)
        m_worker->requestCancel();
    m_status->setText(QStringLiteral("正在取消（当前步骤完成后停止）…"));
}

void MaintenanceDialog::onLog(const QString &text)
{
    m_log->appendPlainText(text);
}

void MaintenanceDialog::onStep(int index, int total, const QString &title)
{
    m_progress->setRange(0, qMax(1, total));
    m_progress->setValue(index);
    m_status->setText(QStringLiteral("正在执行：%1").arg(title));
}

void MaintenanceDialog::onFinished(bool success, const QString &summary)
{
    m_running = false;

    if (m_thread) {
        m_thread->quit();
        m_thread->wait(3000);
    }
    m_worker = nullptr;

    m_progress->setValue(m_progress->maximum());
    m_status->setText(summary);
    m_startButton->setEnabled(true);
    m_closeButton->setEnabled(true);
    m_hibernate->setEnabled(true);
    m_cleanup->setEnabled(true);
    m_downloads->setEnabled(true);
    m_tencent->setEnabled(true);
    m_target->setEnabled(true);
    m_openButton->setVisible(true);

    QMessageBox::information(this,
                             success ? QStringLiteral("完成") : QStringLiteral("结束"),
                             summary);
}

void MaintenanceDialog::openTarget()
{
    const QString target = QDir::toNativeSeparators(m_target->text().trimmed());
    if (!target.isEmpty())
        QDesktopServices::openUrl(QUrl::fromLocalFile(target));
}
