#include "auditdialog.h"
#include "diskscanner.h"

#include <QApplication>
#include <QClipboard>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QFont>
#include <QFontMetrics>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMap>
#include <QProgressBar>
#include <QPushButton>
#include <QThread>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QUrl>
#include <QVBoxLayout>

AuditDialog::AuditDialog(const QString &root, QWidget *parent)
    : QDialog(parent)
    , m_root(root)
{
    setWindowTitle(QStringLiteral("空间体检"));
    resize(1000, 700);
    setModal(true);

    buildUi();

    qRegisterMetaType<AuditResult>("AuditResult");

    m_thread = new QThread(this);
    m_worker = new SpaceAuditWorker(m_root);
    m_worker->moveToThread(m_thread);

    connect(m_thread, &QThread::started, m_worker, &SpaceAuditWorker::process);
    connect(m_worker, &SpaceAuditWorker::progress, this, &AuditDialog::onProgress);
    connect(m_worker, &SpaceAuditWorker::finished, this, &AuditDialog::onFinished);
    connect(m_worker, &SpaceAuditWorker::finished, m_thread, &QThread::quit);
    connect(m_thread, &QThread::finished, m_worker, &QObject::deleteLater);

    m_thread->start();
}

AuditDialog::~AuditDialog()
{
    if (m_worker)
        m_worker->requestCancel();
    if (m_thread) {
        m_thread->quit();
        m_thread->wait(5000);
    }
}

void AuditDialog::buildUi()
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(18, 16, 18, 14);
    layout->setSpacing(10);

    auto *title = new QLabel(
        QStringLiteral("空间体检 · %1").arg(QDir::toNativeSeparators(m_root)), this);
    QFont titleFont = title->font();
    titleFont.setPointSizeF(titleFont.pointSizeF() + 4);
    titleFont.setBold(true);
    title->setFont(titleFont);
    layout->addWidget(title);

    auto *subtitle = new QLabel(
        QStringLiteral("只读扫描，不会删除或移动任何文件。"), this);
    subtitle->setStyleSheet(QStringLiteral("color:#6B7280;"));
    layout->addWidget(subtitle);

    auto *cards = new QHBoxLayout;
    cards->setSpacing(10);

    auto addCard = [this, cards](const QString &caption, const QString &color) -> QLabel * {
        auto *frame = new QFrame(this);
        frame->setStyleSheet(
            QStringLiteral("QFrame{border:1px solid palette(mid);border-radius:8px;}"));
        auto *box = new QVBoxLayout(frame);
        box->setContentsMargins(12, 8, 12, 8);
        box->setSpacing(2);

        auto *captionLabel = new QLabel(caption, frame);
        captionLabel->setStyleSheet(
            QStringLiteral("color:#6B7280;font-size:11px;border:none;"));
        auto *valueLabel = new QLabel(QStringLiteral("—"), frame);
        valueLabel->setStyleSheet(
            QStringLiteral("color:%1;font-size:18px;font-weight:600;border:none;").arg(color));

        box->addWidget(captionLabel);
        box->addWidget(valueLabel);
        cards->addWidget(frame, 1);
        return valueLabel;
    };

    m_scannedValue = addCard(QStringLiteral("扫描到"), QStringLiteral("#111827"));
    m_cleanableValue = addCard(QStringLiteral("可清理"), QStringLiteral("#16A34A"));
    m_movableValue = addCard(QStringLiteral("可迁移"), QStringLiteral("#2563EB"));
    m_cautionValue = addCard(QStringLiteral("需谨慎"), QStringLiteral("#D97706"));
    layout->addLayout(cards);

    auto *progressRow = new QHBoxLayout;
    m_progress = new QProgressBar(this);
    m_progress->setRange(0, 0);
    m_progress->setTextVisible(false);
    m_progress->setFixedHeight(8);

    m_cancelButton = new QPushButton(QStringLiteral("取消"), this);
    connect(m_cancelButton, &QPushButton::clicked, this, &AuditDialog::cancelScan);

    progressRow->addWidget(m_progress, 1);
    progressRow->addWidget(m_cancelButton);
    layout->addLayout(progressRow);

    m_status = new QLabel(QStringLiteral("准备扫描…"), this);
    layout->addWidget(m_status);

    m_current = new QLabel(this);
    m_current->setStyleSheet(QStringLiteral("color:#6B7280;"));
    m_current->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(m_current);

    m_tree = new QTreeWidget(this);
    m_tree->setColumnCount(3);
    m_tree->setHeaderLabels({QStringLiteral("项目"),
                             QStringLiteral("大小"),
                             QStringLiteral("建议")});
    m_tree->setAlternatingRowColors(true);
    m_tree->setUniformRowHeights(true);
    m_tree->setSelectionMode(QAbstractItemView::SingleSelection);
    m_tree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_tree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_tree->header()->setSectionResizeMode(2, QHeaderView::Stretch);
    m_tree->setColumnWidth(1, 110);
    layout->addWidget(m_tree, 1);

    connect(m_tree, &QTreeWidget::itemSelectionChanged, this, [this] {
        const bool has = !selectedPath().isEmpty();
        m_openButton->setEnabled(has);
        m_copyButton->setEnabled(has);
    });
    connect(m_tree, &QTreeWidget::itemDoubleClicked,
            this, &AuditDialog::openSelected);

    auto *buttons = new QHBoxLayout;
    m_openButton = new QPushButton(QStringLiteral("打开位置"), this);
    m_copyButton = new QPushButton(QStringLiteral("复制路径"), this);
    auto *closeButton = new QPushButton(QStringLiteral("关闭"), this);
    m_openButton->setEnabled(false);
    m_copyButton->setEnabled(false);

    connect(m_openButton, &QPushButton::clicked, this, &AuditDialog::openSelected);
    connect(m_copyButton, &QPushButton::clicked, this, &AuditDialog::copySelectedPath);
    connect(closeButton, &QPushButton::clicked, this, &QDialog::accept);

    buttons->addWidget(m_openButton);
    buttons->addWidget(m_copyButton);
    buttons->addStretch(1);
    buttons->addWidget(closeButton);
    layout->addLayout(buttons);

    auto *note = new QLabel(
        QStringLiteral("提示：“扫描到”只统计可读取的文件，可能小于磁盘已用；"
                       "清理/迁移动作将在下一阶段提供。"),
        this);
    note->setStyleSheet(QStringLiteral("color:#6B7280;font-size:11px;"));
    layout->addWidget(note);
}

void AuditDialog::onProgress(const QString &path, qint64 bytes, int files)
{
    if (m_finished)
        return;

    m_status->setText(QStringLiteral("正在扫描：%1 个文件 · 已扫描 %2")
                          .arg(files)
                          .arg(DiskScanner::formatBytes(bytes)));

    const int available = qMax(200, m_current->width());
    const QFontMetrics metrics(m_current->font());
    m_current->setText(metrics.elidedText(QDir::toNativeSeparators(path),
                                          Qt::ElideMiddle,
                                          available));
}

void AuditDialog::onFinished(const AuditResult &result)
{
    m_finished = true;

    if (m_thread) {
        m_thread->quit();
        m_thread->wait(3000);
    }
    m_worker = nullptr;

    m_progress->setRange(0, 1);
    m_progress->setValue(1);
    m_cancelButton->setEnabled(false);

    if (result.cancelled) {
        m_status->setText(QStringLiteral("扫描已取消，以下结果不完整。"));
    } else {
        m_status->setText(QStringLiteral("扫描完成：%1 个文件，共 %2")
                              .arg(result.files)
                              .arg(DiskScanner::formatBytes(result.scannedBytes)));
    }

    populate(result);
}

void AuditDialog::populate(const AuditResult &result)
{
    m_scannedValue->setText(DiskScanner::formatBytes(result.scannedBytes));
    m_cleanableValue->setText(DiskScanner::formatBytes(result.cleanableBytes));
    m_movableValue->setText(DiskScanner::formatBytes(result.movableBytes));
    m_cautionValue->setText(DiskScanner::formatBytes(result.cautionBytes));

    m_tree->clear();

    QMap<QString, QVector<AuditItem>> groups;
    for (const AuditItem &item : result.items)
        groups[item.category].append(item);

    const QStringList order = {
        QStringLiteral("系统可清理"),
        QStringLiteral("组件存储（需 DISM）"),
        QStringLiteral("用户缓存"),
        QStringLiteral("用户数据（可迁移）"),
        QStringLiteral("应用缓存（谨慎）"),
        QStringLiteral("程序（不建议直接搬）"),
        QStringLiteral("程序数据（谨慎）"),
        QStringLiteral("其他目录（可迁移）"),
    };

    QStringList categories = order;
    for (auto it = groups.cbegin(); it != groups.cend(); ++it) {
        if (!categories.contains(it.key()))
            categories.append(it.key());
    }

    for (const QString &category : categories) {
        const auto it = groups.constFind(category);
        if (it == groups.constEnd() || it->isEmpty())
            continue;

        auto *parent = new QTreeWidgetItem(m_tree);
        parent->setText(0, QStringLiteral("%1（%2 项）").arg(category).arg(it->size()));
        parent->setFirstColumnSpanned(true);
        QFont bold = parent->font(0);
        bold.setBold(true);
        parent->setFont(0, bold);
        parent->setExpanded(true);
        parent->setFlags(parent->flags() & ~Qt::ItemIsSelectable);

        for (const AuditItem &item : *it) {
            auto *child = new QTreeWidgetItem(parent);
            child->setText(0, item.name);
            child->setText(1, DiskScanner::formatBytes(item.bytes));
            child->setText(2, item.advice);
            child->setTextAlignment(1, Qt::AlignRight | Qt::AlignVCenter);
            child->setData(0, Qt::UserRole, item.path);
            child->setToolTip(0, item.path);
            child->setToolTip(2, item.advice);
        }
    }
}

QString AuditDialog::selectedPath() const
{
    const QTreeWidgetItem *item = m_tree->currentItem();
    if (!item)
        return {};
    return item->data(0, Qt::UserRole).toString();
}

void AuditDialog::cancelScan()
{
    if (m_worker)
        m_worker->requestCancel();
    m_cancelButton->setEnabled(false);
    m_status->setText(QStringLiteral("正在取消…"));
}

void AuditDialog::openSelected()
{
    const QString path = selectedPath();
    if (path.isEmpty())
        return;

    const QFileInfo info(path);
    const QString target = info.isDir() ? path : info.absolutePath();
    QDesktopServices::openUrl(QUrl::fromLocalFile(target));
}

void AuditDialog::copySelectedPath()
{
    const QString path = selectedPath();
    if (path.isEmpty())
        return;
    QApplication::clipboard()->setText(path);
}
