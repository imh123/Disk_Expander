#include "rollbackdialog.h"
#include "diskscanner.h"

#include <QApplication>
#include <QDir>
#include <QFont>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QStorageInfo>
#include <QThread>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>

namespace {
const QString kMigrationRoot = QStringLiteral("D:\\DiskExpander\\Data");
}

RollbackDialog::RollbackDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(QStringLiteral("回滚迁移"));
    resize(880, 600);
    setModal(true);
    buildUi();
    rescan();
}

RollbackDialog::~RollbackDialog()
{
    if (m_worker)
        m_worker->requestCancel();
    if (m_thread) {
        m_thread->quit();
        m_thread->wait(5000);
    }
}

void RollbackDialog::buildUi()
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(18, 16, 18, 14);
    layout->setSpacing(10);

    auto *title = new QLabel(QStringLiteral("回滚迁移"), this);
    QFont titleFont = title->font();
    titleFont.setPointSizeF(titleFont.pointSizeF() + 4);
    titleFont.setBold(true);
    title->setFont(titleFont);
    layout->addWidget(title);

    auto *subtitle = new QLabel(
        QStringLiteral("把指向 %1 的 junction 恢复成 C: 上的普通目录，数据会从 D: 移回。")
            .arg(QDir::toNativeSeparators(kMigrationRoot)),
        this);
    subtitle->setWordWrap(true);
    subtitle->setStyleSheet(QStringLiteral("color:#6B7280;"));
    layout->addWidget(subtitle);

    auto *warning = new QLabel(
        QStringLiteral("⚠ 回滚前请关闭正在使用这些目录的程序（微信/QQ、下载工具等）。"), this);
    warning->setStyleSheet(QStringLiteral("color:#D97706;"));
    layout->addWidget(warning);

    m_tree = new QTreeWidget(this);
    m_tree->setColumnCount(3);
    m_tree->setHeaderLabels({QStringLiteral("原路径（C:）"),
                             QStringLiteral("实际位置（D:）"),
                             QStringLiteral("大小")});
    m_tree->setRootIsDecorated(false);
    m_tree->setAlternatingRowColors(true);
    m_tree->setUniformRowHeights(true);
    m_tree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_tree->header()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_tree->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    layout->addWidget(m_tree, 1);

    m_status = new QLabel(QStringLiteral("正在扫描…"), this);
    layout->addWidget(m_status);

    m_progress = new QProgressBar(this);
    m_progress->setRange(0, 1);
    m_progress->setValue(0);
    m_progress->setTextVisible(false);
    m_progress->setFixedHeight(8);
    layout->addWidget(m_progress);

    m_log = new QPlainTextEdit(this);
    m_log->setReadOnly(true);
    m_log->setMaximumBlockCount(3000);
    QFont logFont(QStringLiteral("Consolas"));
    logFont.setPointSize(9);
    m_log->setFont(logFont);
    layout->addWidget(m_log, 1);

    auto *buttons = new QHBoxLayout;
    m_rescanButton = new QPushButton(QStringLiteral("重新扫描"), this);
    m_rollbackButton = new QPushButton(QStringLiteral("回滚选中"), this);
    m_closeButton = new QPushButton(QStringLiteral("关闭"), this);

    connect(m_rescanButton, &QPushButton::clicked, this, &RollbackDialog::rescan);
    connect(m_rollbackButton, &QPushButton::clicked, this, &RollbackDialog::rollbackSelected);
    connect(m_closeButton, &QPushButton::clicked, this, &QDialog::accept);

    buttons->addWidget(m_rescanButton);
    buttons->addWidget(m_rollbackButton);
    buttons->addStretch(1);
    buttons->addWidget(m_closeButton);
    layout->addLayout(buttons);
}

void RollbackDialog::rescan()
{
    if (m_running)
        return;

    QApplication::setOverrideCursor(Qt::WaitCursor);

    QString profile = QString::fromLocal8Bit(qgetenv("USERPROFILE"));
    if (profile.isEmpty())
        profile = QStringLiteral("C:\\Users");

    m_links = findMigrationLinks(profile, kMigrationRoot);

    QApplication::restoreOverrideCursor();
    populate();
}

void RollbackDialog::populate()
{
    m_tree->clear();

    qint64 total = 0;
    for (int i = 0; i < m_links.size(); ++i) {
        const MigrationLink &link = m_links.at(i);
        total += link.bytes;

        auto *item = new QTreeWidgetItem(m_tree);
        item->setText(0, link.source);
        item->setText(1, link.target);
        item->setText(2, DiskScanner::formatBytes(link.bytes));
        item->setTextAlignment(2, Qt::AlignRight | Qt::AlignVCenter);
        item->setCheckState(0, Qt::Checked);
        item->setData(0, Qt::UserRole, i);
        item->setToolTip(0, link.source);
        item->setToolTip(1, link.target);
    }

    if (m_links.isEmpty()) {
        m_status->setText(QStringLiteral("没有发现指向 %1 的迁移。")
                              .arg(QDir::toNativeSeparators(kMigrationRoot)));
        m_rollbackButton->setEnabled(false);
    } else {
        m_status->setText(QStringLiteral("发现 %1 项迁移，共 %2。勾选后点“回滚选中”。")
                              .arg(m_links.size())
                              .arg(DiskScanner::formatBytes(total)));
        m_rollbackButton->setEnabled(true);
    }
}

QVector<MigrationLink> RollbackDialog::checkedLinks() const
{
    QVector<MigrationLink> selected;
    for (int i = 0; i < m_tree->topLevelItemCount(); ++i) {
        const QTreeWidgetItem *item = m_tree->topLevelItem(i);
        if (item->checkState(0) != Qt::Checked)
            continue;
        const int index = item->data(0, Qt::UserRole).toInt();
        if (index >= 0 && index < m_links.size())
            selected.append(m_links.at(index));
    }
    return selected;
}

void RollbackDialog::rollbackSelected()
{
    if (m_running)
        return;

    const QVector<MigrationLink> selected = checkedLinks();
    if (selected.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("提示"),
                                 QStringLiteral("请先勾选要回滚的项。"));
        return;
    }

    qint64 total = 0;
    for (const MigrationLink &link : selected)
        total += link.bytes;

    QStorageInfo storage(QStringLiteral("C:/"));
    const qint64 available = storage.bytesAvailable();

    const auto answer = QMessageBox::warning(
        this,
        QStringLiteral("确认回滚"),
        QStringLiteral("将把 %1 数据从 D: 移回 C:，并把 junction 恢复成普通目录。\n\n"
                       "C: 当前可用 %2。回滚后 C: 空间会相应减少。\n\n确定继续吗？")
            .arg(DiskScanner::formatBytes(total),
                 DiskScanner::formatBytes(available)),
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::No);
    if (answer != QMessageBox::Yes)
        return;

    m_running = true;
    m_log->clear();
    m_progress->setRange(0, qMax(1, selected.size()));
    m_progress->setValue(0);
    m_status->setText(QStringLiteral("正在回滚…"));
    m_rescanButton->setEnabled(false);
    m_rollbackButton->setEnabled(false);
    m_closeButton->setEnabled(false);

    m_thread = new QThread(this);
    m_worker = new RollbackWorker(selected);
    m_worker->moveToThread(m_thread);

    connect(m_thread, &QThread::started, m_worker, &RollbackWorker::run);
    connect(m_worker, &RollbackWorker::logMessage, this, &RollbackDialog::onLog);
    connect(m_worker, &RollbackWorker::progress, this, &RollbackDialog::onProgress);
    connect(m_worker, &RollbackWorker::finished, this, &RollbackDialog::onFinished);
    connect(m_worker, &RollbackWorker::finished, m_thread, &QThread::quit);
    connect(m_thread, &QThread::finished, m_worker, &QObject::deleteLater);

    m_thread->start();
}

void RollbackDialog::onLog(const QString &text)
{
    m_log->appendPlainText(text);
}

void RollbackDialog::onProgress(int index, int total)
{
    m_progress->setRange(0, qMax(1, total));
    m_progress->setValue(index);
}

void RollbackDialog::onFinished(int succeeded, int failed, const QString &summary)
{
    m_running = false;

    if (m_thread) {
        m_thread->quit();
        m_thread->wait(3000);
    }
    m_worker = nullptr;

    m_rescanButton->setEnabled(true);
    m_rollbackButton->setEnabled(true);
    m_closeButton->setEnabled(true);
    m_status->setText(summary);

    QMessageBox::information(this,
                             failed == 0 ? QStringLiteral("完成") : QStringLiteral("部分完成"),
                             summary);

    rescan();
}
