#include "mainwindow.h"
#include "auditdialog.h"
#include "diskmapwidget.h"
#include "diskscanner.h"
#include "maintenancedialog.h"
#include "rollbackdialog.h"

#ifndef NOMINMAX
#  define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>

#include <QAction>
#include <QApplication>
#include <QDateTime>
#include <QDir>
#include <QDockWidget>
#include <QElapsedTimer>
#include <QFile>
#include <QFileDialog>
#include <QFont>
#include <QFrame>
#include <QHBoxLayout>
#include <QIcon>
#include <QKeySequence>
#include <QJsonDocument>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMutex>
#include <QMutexLocker>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QSplitter>
#include <QStandardPaths>
#include <QStatusBar>
#include <QStyle>
#include <QTextBrowser>
#include <QTextStream>
#include <QTimer>
#include <QToolBar>
#include <QVBoxLayout>

namespace {

MainWindow *g_window = nullptr;
QMutex g_logMutex;

QString logFilePath()
{
    const QString directory =
        QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)
        + QStringLiteral("/logs");
    QDir().mkpath(directory);
    return directory + QStringLiteral("/disk-expander.log");
}

void writeLogFile(const QString &line)
{
    QMutexLocker locker(&g_logMutex);
    QFile file(logFilePath());
    if (!file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text))
        return;
    QTextStream stream(&file);
    stream << QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz "))
           << line << '\n';
}

void messageHandler(QtMsgType type, const QMessageLogContext &, const QString &message)
{
    const char *level = "INF";
    switch (type) {
    case QtDebugMsg:    level = "DBG"; break;
    case QtInfoMsg:     level = "INF"; break;
    case QtWarningMsg:  level = "WRN"; break;
    case QtCriticalMsg: level = "ERR"; break;
    case QtFatalMsg:    level = "FTL"; break;
    }

    const QString line = QStringLiteral("[%1] %2")
                             .arg(QString::fromLatin1(level), message);
    writeLogFile(line);

    MainWindow *window = g_window;
    if (window) {
        QMetaObject::invokeMethod(window,
                                  "appendLog",
                                  Qt::QueuedConnection,
                                  Q_ARG(QString, line));
    }
}

QString mutedColor()
{
    return QStringLiteral("#6B7280");
}

QString okLine(const QString &text)
{
    return QStringLiteral("<li><span style='color:#16A34A;font-weight:600'>✔</span> %1</li>")
        .arg(text);
}

QString warnLine(const QString &text)
{
    return QStringLiteral("<li><span style='color:#D97706;font-weight:600'>⚠</span> %1</li>")
        .arg(text);
}

QString badLine(const QString &text)
{
    return QStringLiteral("<li><span style='color:#DC2626;font-weight:600'>✖</span> %1</li>")
        .arg(text);
}

QString infoLine(const QString &text)
{
    return QStringLiteral("<li><span style='color:%1'>ℹ</span> %2</li>")
        .arg(mutedColor(), text);
}

}  // namespace

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    g_window = this;
    qInstallMessageHandler(messageHandler);

    setWindowTitle(QStringLiteral("Disk Expander · 磁盘布局"));
    setWindowIcon(QIcon(QStringLiteral(":/app.ico")));
    setMinimumSize(940, 620);
    resize(1220, 800);

    buildUi();
    refresh();

    QTimer::singleShot(0, this, &MainWindow::checkPendingMigration);
}

MainWindow::~MainWindow()
{
    g_window = nullptr;
    qInstallMessageHandler(nullptr);
}

void MainWindow::buildUi()
{
    // 顶部摘要
    auto *header = new QWidget(this);
    auto *headerLayout = new QHBoxLayout(header);
    headerLayout->setContentsMargins(16, 12, 16, 10);
    headerLayout->setSpacing(14);

    m_banner = new QLabel(header);
    m_banner->setTextFormat(Qt::RichText);
    m_banner->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_banner->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);

    m_usage = new QProgressBar(header);
    m_usage->setRange(0, 100);
    m_usage->setValue(0);
    m_usage->setTextVisible(false);
    m_usage->setFixedSize(150, 8);

    m_percent = new QLabel(header);
    m_percent->setMinimumWidth(46);
    m_percent->setAlignment(Qt::AlignRight | Qt::AlignVCenter);

    headerLayout->addWidget(m_banner, 1);
    headerLayout->addWidget(m_usage, 0, Qt::AlignVCenter);
    headerLayout->addWidget(m_percent, 0, Qt::AlignVCenter);

    // 磁盘地图 + 详情
    m_map = new DiskMapWidget(this);

    auto *scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidget(m_map);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);

    m_details = new QTextBrowser(this);
    m_details->setFrameShape(QFrame::NoFrame);
    m_details->setOpenExternalLinks(false);
    m_details->setMinimumWidth(350);
    m_details->setMaximumWidth(560);

    auto *splitter = new QSplitter(Qt::Horizontal, this);
    splitter->addWidget(scroll);
    splitter->addWidget(m_details);
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 0);
    splitter->setSizes({850, 380});
    splitter->setChildrenCollapsible(false);

    auto *central = new QWidget(this);
    auto *centralLayout = new QVBoxLayout(central);
    centralLayout->setContentsMargins(0, 0, 0, 0);
    centralLayout->setSpacing(0);
    centralLayout->addWidget(header);
    centralLayout->addWidget(splitter, 1);
    setCentralWidget(central);

    // 日志
    auto *dock = new QDockWidget(QStringLiteral("运行日志"), this);
    dock->setObjectName(QStringLiteral("logDock"));
    dock->setAllowedAreas(Qt::BottomDockWidgetArea);
    m_log = new QPlainTextEdit(dock);
    m_log->setReadOnly(true);
    m_log->setMaximumBlockCount(4000);
    QFont logFont(QStringLiteral("Consolas"));
    logFont.setPointSize(9);
    m_log->setFont(logFont);
    dock->setWidget(m_log);
    addDockWidget(Qt::BottomDockWidgetArea, dock);
    resizeDocks({dock}, {170}, Qt::Vertical);

    // 工具栏
    auto *toolbar = addToolBar(QStringLiteral("主工具栏"));
    toolbar->setMovable(false);
    toolbar->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);

    auto *refreshAction = toolbar->addAction(
        style()->standardIcon(QStyle::SP_BrowserReload),
        QStringLiteral("刷新"),
        this,
        &MainWindow::refresh);
    refreshAction->setShortcut(QKeySequence::Refresh);
    refreshAction->setToolTip(QStringLiteral("重新扫描磁盘布局（F5）"));

    auto *exportAction = toolbar->addAction(
        style()->standardIcon(QStyle::SP_DialogSaveButton),
        QStringLiteral("导出报告"),
        this,
        &MainWindow::exportReport);

    auto *auditAction = toolbar->addAction(
        style()->standardIcon(QStyle::SP_FileDialogInfoView),
        QStringLiteral("空间体检"),
        this,
        &MainWindow::openAudit);
    auditAction->setToolTip(
        QStringLiteral("只读扫描 C: 的空间占用，给出可清理 / 可迁移清单"));

    auto *maintenanceAction = toolbar->addAction(
        style()->standardIcon(QStyle::SP_DialogApplyButton),
        QStringLiteral("释放空间"),
        this,
        &MainWindow::openMaintenance);
    maintenanceAction->setToolTip(
        QStringLiteral("关闭休眠、清理临时文件、把 Downloads / Tencent Files 迁到其他盘"));

    auto *rollbackAction = toolbar->addAction(
        style()->standardIcon(QStyle::SP_ArrowBack),
        QStringLiteral("回滚迁移"),
        this,
        &MainWindow::openRollback);
    rollbackAction->setToolTip(
        QStringLiteral("把指向 D:\\DiskExpander\\Data 的 junction 恢复成 C: 上的普通目录"));

    m_adminAction = toolbar->addAction(
        style()->standardIcon(QStyle::SP_MessageBoxWarning),
        QStringLiteral("以管理员身份重启"),
        this,
        &MainWindow::relaunchAsAdmin);
    m_adminAction->setVisible(false);

    // 菜单
    auto *fileMenu = menuBar()->addMenu(QStringLiteral("文件(&F)"));
    fileMenu->addAction(exportAction);
    fileMenu->addSeparator();
    fileMenu->addAction(QStringLiteral("退出(&X)"), QKeySequence::Quit, this, &QWidget::close);

    auto *helpMenu = menuBar()->addMenu(QStringLiteral("帮助(&H)"));
    helpMenu->addAction(QStringLiteral("关于(&A)"), this, &MainWindow::showAbout);

    // 状态栏
    m_status = new QLabel(this);
    statusBar()->addPermanentWidget(m_status);
    statusBar()->showMessage(QStringLiteral("就绪"));

    connect(m_map, &DiskMapWidget::selectionChanged,
            this, &MainWindow::onSelectionChanged);

    qInfo() << "Disk Expander 0.2 启动，只读模式";
}

void MainWindow::refresh()
{
    qInfo() << "开始扫描磁盘布局...";

    QApplication::setOverrideCursor(Qt::WaitCursor);
    QElapsedTimer timer;
    timer.start();
    const ScanResult result = DiskScanner::scan();
    m_scanMs = timer.elapsed();
    QApplication::restoreOverrideCursor();

    int partitions = 0;
    for (const auto &disk : result.disks)
        partitions += disk.partitions.size();

    qInfo() << QStringLiteral("扫描完成：%1 块磁盘，%2 个分区，耗时 %3 ms")
                   .arg(result.disks.size())
                   .arg(partitions)
                   .arg(m_scanMs);

    if (!result.error.isEmpty())
        qWarning() << result.error;

    applyScan(result);
}

void MainWindow::applyScan(const ScanResult &result)
{
    m_result = result;

    m_map->setData(result.disks, result.volumes);
    m_map->setMinimumHeight(m_map->sizeHint().height());

    updateBanner();
    updateStatus();

    if (!result.error.isEmpty())
        m_details->setHtml(
            QStringLiteral("<p style='color:#DC2626;font-weight:600;font-size:14px'>%1</p>")
                .arg(result.error.toHtmlEscaped()));
    else
        updateDetails(-1, -1);

    if (m_adminAction)
        m_adminAction->setVisible(!result.elevated);
}

void MainWindow::updateBanner()
{
    const VolumeInfo *systemVolume = nullptr;
    for (const auto &volume : m_result.volumes) {
        if (volume.isBootVolume) {
            systemVolume = &volume;
            break;
        }
    }
    if (!systemVolume) {
        for (const auto &volume : m_result.volumes) {
            if (volume.driveLetter == QStringLiteral("C:")) {
                systemVolume = &volume;
                break;
            }
        }
    }

    qint64 total = 0;
    for (const auto &disk : m_result.disks)
        total += disk.size;

    if (!systemVolume || systemVolume->size <= 0) {
        m_banner->setText(
            QStringLiteral("<span style='font-size:13px'><b>%1</b> 块磁盘 · 总容量 <b>%2</b></span>")
                .arg(m_result.disks.size())
                .arg(DiskScanner::formatBytes(total)));
        m_usage->setValue(0);
        m_percent->clear();
        return;
    }

    double used = 100.0 * double(systemVolume->size - systemVolume->freeBytes)
                  / double(systemVolume->size);
    used = qBound(0.0, used, 100.0);

    const QString color = used >= 90.0 ? QStringLiteral("#DC2626")
                                        : (used >= 75.0 ? QStringLiteral("#D97706")
                                                        : QStringLiteral("#16A34A"));
    const QString letter = systemVolume->driveLetter.isEmpty()
                               ? QStringLiteral("系统盘")
                               : systemVolume->driveLetter;

    m_banner->setText(
        QStringLiteral(
            "<span style='font-size:13px'><b>%1</b> 剩余 "
            "<span style='color:%2;font-weight:700'>%3</span> / %4"
            "（已用 %5%）</span>"
            " &nbsp;&nbsp;·&nbsp;&nbsp; %6 块磁盘 · 总容量 %7")
            .arg(letter,
                 color,
                 DiskScanner::formatBytes(systemVolume->freeBytes),
                 DiskScanner::formatBytes(systemVolume->size),
                 QString::number(used, 'f', 1))
            .arg(m_result.disks.size())
            .arg(DiskScanner::formatBytes(total)));

    m_usage->setValue(int(qRound(used)));
    m_usage->setStyleSheet(
        QStringLiteral("QProgressBar{border:none;background:%1;border-radius:4px;}"
                       "QProgressBar::chunk{background:%2;border-radius:4px;}")
            .arg(palette().color(QPalette::Midlight).name(), color));

    m_percent->setText(QStringLiteral("%1%").arg(used, 0, 'f', 1));
    m_percent->setStyleSheet(QStringLiteral("color:%1;font-weight:600;").arg(color));
}

void MainWindow::updateStatus()
{
    int partitions = 0;
    for (const auto &disk : m_result.disks)
        partitions += disk.partitions.size();

    m_status->setText(
        QStringLiteral("%1 · %2 块磁盘 · %3 个分区 · 扫描 %4 ms")
            .arg(m_result.elevated ? QStringLiteral("管理员模式")
                                   : QStringLiteral("非管理员模式"),
                 QString::number(m_result.disks.size()),
                 QString::number(partitions),
                 QString::number(m_scanMs)));
}

void MainWindow::onSelectionChanged(int diskNumber, int partitionNumber)
{
    updateDetails(diskNumber, partitionNumber);
}

QString MainWindow::capabilityHtml(const DiskInfo &disk,
                                   const PartitionInfo &part,
                                   const VolumeInfo *volume) const
{
    Q_UNUSED(volume)

    QStringList items;

    const UnallocatedRegion *after = nullptr;
    for (const auto &region : disk.unallocated) {
        if (qAbs(region.offset - (part.offset + part.length)) <= 512) {
            after = &region;
            break;
        }
    }

    const PartitionInfo *next = nullptr;
    for (const auto &candidate : disk.partitions) {
        if (candidate.offset >= part.offset + part.length
            && (!next || candidate.offset < next->offset))
            next = &candidate;
    }

    qint64 sameDiskMax = after ? after->length : 0;

    if (part.role != PartitionRole::Basic) {
        items << warnLine(QStringLiteral("这是%1，不参与扩容。").arg(roleName(part.role)));
    } else if (after) {
        items << okLine(
            QStringLiteral("可直接向右扩展 <b>+%1</b>（同盘相邻的未分配空间）。")
                .arg(DiskScanner::formatBytes(after->length)));
    } else if (next) {
        QString nextLabel;
        if (next->role == PartitionRole::Basic) {
            nextLabel = QStringLiteral("数据分区");
            for (const auto &candidate : m_result.volumes) {
                if (candidate.guid.compare(next->volumeGuid, Qt::CaseInsensitive) == 0
                    && !candidate.driveLetter.isEmpty()) {
                    nextLabel = candidate.driveLetter;
                    break;
                }
            }
        } else {
            nextLabel = roleName(next->role);
        }

        items << warnLine(
            QStringLiteral("后面紧邻“%1”（分区 %2，%3），需要先移动或删除它才能扩展。")
                .arg(nextLabel)
                .arg(next->partitionNumber)
                .arg(DiskScanner::formatBytes(next->length)));

        if (next->role == PartitionRole::Recovery) {
            sameDiskMax = next->length;
            items << infoLine(
                QStringLiteral("即使删除这个恢复分区，同盘最多也只能增加 %1。")
                    .arg(DiskScanner::formatBytes(next->length)));
        }
    } else {
        items << badLine(QStringLiteral("已经到磁盘末尾，后面没有可用空间。"));
    }

    if (part.isBoot) {
        items << warnLine(QStringLiteral(
            "这是 Windows 所在卷，扩容必须在 WinPE 等离线环境执行。"));

        const qint64 tenGiB = 10LL * 1024 * 1024 * 1024;
        if (sameDiskMax <= 0) {
            items << badLine(QStringLiteral(
                "同盘没有可扩展空间，无法通过分区操作解决 C: 空间不足。"));
        } else if (sameDiskMax < tenGiB) {
            items << badLine(
                QStringLiteral("同盘最多只能增加 %1，不足以解决 C: 空间不足；"
                               "建议迁移数据或更换更大的硬盘。")
                    .arg(DiskScanner::formatBytes(sameDiskMax)));
        }
    }

    if (disk.isDynamic)
        items << badLine(QStringLiteral(
            "动态磁盘：本工具只读展示，不支持任何写操作。"));

    if (part.role == PartitionRole::StorageSpaces)
        items << badLine(QStringLiteral(
            "存储池成员：不能按普通分区处理。"));

    items << infoLine(QStringLiteral(
        "Windows 不支持把其他物理磁盘的空间并入启动卷（C:）；跨盘只能挂载为文件夹。"));

    return QStringLiteral("<ul style='margin-left:-18px'>") + items.join(QString())
           + QStringLiteral("</ul>");
}

void MainWindow::updateDetails(int diskNumber, int partitionNumber)
{
    if (m_result.disks.isEmpty()) {
        m_details->setHtml(
            QStringLiteral("<h3>没有可显示的磁盘</h3><p style='color:%1'>%2</p>")
                .arg(mutedColor(), m_result.error.toHtmlEscaped()));
        return;
    }

    const DiskInfo *disk = nullptr;
    for (const auto &candidate : m_result.disks) {
        if (candidate.number == diskNumber) {
            disk = &candidate;
            break;
        }
    }

    if (!disk) {
        QString html;
        html += QStringLiteral("<h2 style='font-size:18px;font-weight:600'>磁盘布局</h2>");
        html += QStringLiteral("<p>点击上方任意磁盘色块查看分区详情。</p>");
        html += QStringLiteral("<p style='color:%1'>当前为只读模式：只读取分区表和卷信息，"
                               "不会修改磁盘。</p>")
                    .arg(mutedColor());
        html += QStringLiteral("<h3 style='font-size:14px'>能力边界</h3><ul>");
        html += QStringLiteral("<li>同一物理磁盘内，可以把后面的未分配空间扩给前面的分区；</li>");
        html += QStringLiteral("<li>不同物理磁盘之间，Windows 不支持把空间并入启动卷 C:；</li>");
        html += QStringLiteral("<li>跨盘只能把分区挂载为 C: 下的文件夹（后续版本）。</li>");
        html += QStringLiteral("</ul>");
        m_details->setHtml(html);
        return;
    }

    const PartitionInfo *part = nullptr;
    for (const auto &candidate : disk->partitions) {
        if (candidate.partitionNumber == partitionNumber) {
            part = &candidate;
            break;
        }
    }

    const VolumeInfo *volume = nullptr;
    if (part && !part->volumeGuid.isEmpty()) {
        for (const auto &candidate : m_result.volumes) {
            if (candidate.guid.compare(part->volumeGuid, Qt::CaseInsensitive) == 0) {
                volume = &candidate;
                break;
            }
        }
    }

    auto row = [](const QString &key, const QString &value) {
        return QStringLiteral(
                   "<tr><td style='color:%1;padding:2px 14px 2px 0;white-space:nowrap'>%2</td>"
                   "<td style='padding:2px 0'>%3</td></tr>")
            .arg(mutedColor(), key, value);
    };

    QString html;

    if (part) {
        const QString letter = volume && !volume->driveLetter.isEmpty()
                                   ? volume->driveLetter
                                   : QStringLiteral("分区 %1").arg(part->partitionNumber);

        html += QStringLiteral("<h2 style='font-size:18px;font-weight:600'>%1</h2>")
                    .arg(letter.toHtmlEscaped());
        html += QStringLiteral("<p style='color:%1;margin-top:-6px'>%2</p>")
                    .arg(mutedColor(), roleName(part->role));

        html += QStringLiteral("<table cellspacing='0' cellpadding='0'>");
        html += row(QStringLiteral("磁盘"),
                    QStringLiteral("磁盘 %1（%2）").arg(disk->number).arg(disk->model.toHtmlEscaped()));
        html += row(QStringLiteral("分区号"), QString::number(part->partitionNumber));
        html += row(QStringLiteral("起始偏移"), DiskScanner::formatBytes(part->offset));
        html += row(QStringLiteral("分区大小"), DiskScanner::formatBytes(part->length));
        html += row(QStringLiteral("分区类型"), part->typeGuid.toHtmlEscaped());
        if (!part->name.isEmpty())
            html += row(QStringLiteral("名称"), part->name.toHtmlEscaped());
        if (volume) {
            html += row(QStringLiteral("卷 GUID"), volume->guid.toHtmlEscaped());
            html += row(QStringLiteral("文件系统"),
                        volume->fileSystem.isEmpty() ? QStringLiteral("—")
                                                     : volume->fileSystem.toHtmlEscaped());
            html += row(QStringLiteral("卷标"),
                        volume->label.isEmpty() ? QStringLiteral("—")
                                                : volume->label.toHtmlEscaped());
            if (volume->size > 0) {
                html += row(QStringLiteral("可用空间"),
                            QStringLiteral("%1 / %2")
                                .arg(DiskScanner::formatBytes(volume->freeBytes),
                                     DiskScanner::formatBytes(volume->size)));
            }
            if (!volume->mountPoints.isEmpty()) {
                QStringList points;
                for (const auto &point : volume->mountPoints)
                    points << point.toHtmlEscaped();
                html += row(QStringLiteral("挂载点"), points.join(QStringLiteral("<br>")));
            }
        } else {
            html += row(QStringLiteral("卷"), QStringLiteral("无（未格式化或非数据分区）"));
        }
        QStringList flags;
        if (part->isBoot)
            flags << QStringLiteral("Windows 启动卷");
        if (part->isSystem)
            flags << QStringLiteral("系统/引导分区");
        html += row(QStringLiteral("标志"),
                    flags.isEmpty() ? QStringLiteral("—")
                                    : flags.join(QStringLiteral(" · ")));
        html += QStringLiteral("</table>");

        html += QStringLiteral("<h3 style='font-size:14px'>可执行操作</h3>");
        html += capabilityHtml(*disk, *part, volume);
    } else {
        html += QStringLiteral("<h2 style='font-size:18px;font-weight:600'>磁盘 %1</h2>")
                    .arg(disk->number);
        html += QStringLiteral("<p style='color:%1;margin-top:-6px'>%2</p>")
                    .arg(mutedColor(), disk->model.toHtmlEscaped());

        html += QStringLiteral("<table cellspacing='0' cellpadding='0'>");
        html += row(QStringLiteral("容量"), DiskScanner::formatBytes(disk->size));
        html += row(QStringLiteral("分区表"), styleName(disk->style));
        html += row(QStringLiteral("总线"), disk->busType.isEmpty()
                                               ? QStringLiteral("—")
                                               : disk->busType);
        html += row(QStringLiteral("扇区大小"),
                    QStringLiteral("%1 字节").arg(disk->sectorSize));
        html += row(QStringLiteral("分区数量"), QString::number(disk->partitions.size()));
        html += row(QStringLiteral("未分配"), DiskScanner::formatBytes(disk->unallocatedBytes()));
        html += QStringLiteral("</table>");

        if (disk->partitions.isEmpty()) {
            html += QStringLiteral("<p style='color:%1'>该磁盘没有分区。</p>")
                        .arg(mutedColor());
        } else {
            html += QStringLiteral("<h3 style='font-size:14px'>分区</h3><ul>");
            for (const auto &item : disk->partitions) {
                const QString letter = item.volumeGuid.isEmpty()
                                           ? QString()
                                           : QStringLiteral(" · 卷");
                html += QStringLiteral("<li>分区 %1 · %2 · %3%4</li>")
                            .arg(item.partitionNumber)
                            .arg(roleName(item.role))
                            .arg(DiskScanner::formatBytes(item.length))
                            .arg(letter);
            }
            html += QStringLiteral("</ul>");
        }

        if (!disk->unallocated.isEmpty()) {
            html += QStringLiteral("<h3 style='font-size:14px'>未分配空间</h3><ul>");
            for (const auto &region : disk->unallocated) {
                html += QStringLiteral("<li>起始 %1 · %2</li>")
                            .arg(DiskScanner::formatBytes(region.offset),
                                 DiskScanner::formatBytes(region.length));
            }
            html += QStringLiteral("</ul>");
        }
    }

    m_details->setHtml(html);
}

void MainWindow::exportReport()
{
    QString path = QFileDialog::getSaveFileName(
        this,
        QStringLiteral("导出磁盘报告"),
        QDir::homePath() + QStringLiteral("/disk-layout.json"),
        QStringLiteral("JSON 文件 (*.json)"));
    if (path.isEmpty())
        return;

    if (!path.endsWith(QStringLiteral(".json"), Qt::CaseInsensitive))
        path += QStringLiteral(".json");

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QMessageBox::warning(this,
                             QStringLiteral("导出失败"),
                             QStringLiteral("无法写入文件：%1").arg(path));
        return;
    }

    file.write(QJsonDocument(DiskScanner::toJson(m_result))
                   .toJson(QJsonDocument::Indented));
    statusBar()->showMessage(QStringLiteral("报告已保存：%1").arg(path), 5000);
    qInfo() << "报告已导出：" << path;
}

void MainWindow::openAudit()
{
    QString root = QString::fromLocal8Bit(qgetenv("SystemDrive"));
    if (root.isEmpty())
        root = QStringLiteral("C:");
    root += QStringLiteral("\\");

    AuditDialog dialog(root, this);
    dialog.exec();
}

void MainWindow::openMaintenance()
{
    MaintenanceDialog dialog(this);
    dialog.exec();

    // 操作可能释放/迁移了空间，关闭后重新扫描一次。
    refresh();
}

void MainWindow::openRollback()
{
    RollbackDialog dialog(this);
    dialog.exec();
    refresh();
}

void MainWindow::checkPendingMigration()
{
    MigrationJournal entry;
    if (!readMigrationJournal(&entry))
        return;

    QMessageBox box(this);
    box.setIcon(QMessageBox::Warning);
    box.setWindowTitle(QStringLiteral("未完成的迁移"));
    box.setTextFormat(Qt::RichText);
    box.setText(QStringLiteral(
                    "检测到上次迁移没有完成：<br><br><b>%1</b><br>→ %2<br><br>"
                    "步骤：%3<br>开始时间：%4")
                    .arg(entry.source.toHtmlEscaped(),
                         entry.target.toHtmlEscaped(),
                         entry.step.toHtmlEscaped(),
                         entry.startedAt.toHtmlEscaped()));
    box.setInformativeText(QStringLiteral("要完成迁移，还是撤销并恢复原目录？"));

    QPushButton *completeButton = box.addButton(QStringLiteral("完成迁移"),
                                                 QMessageBox::AcceptRole);
    QPushButton *undoButton = box.addButton(QStringLiteral("撤销并恢复"),
                                            QMessageBox::DestructiveRole);
    box.addButton(QStringLiteral("稍后处理"), QMessageBox::RejectRole);

    const bool canComplete = (entry.step == QStringLiteral("renamed")
                              || entry.step == QStringLiteral("linked"));
    completeButton->setEnabled(canComplete);

    box.exec();

    QString log;
    QString error;
    const auto logger = [&log](const QString &text) {
        log += text + QLatin1Char('\n');
    };

    if (box.clickedButton() == completeButton) {
        if (!completeMigration(entry, logger, &error)) {
            QMessageBox::warning(
                this, QStringLiteral("恢复失败"),
                error + QStringLiteral("\n\n迁移日志已保留，下次启动会再次提示。"));
            return;
        }
    } else if (box.clickedButton() == undoButton) {
        if (!undoMigration(entry, logger, &error)) {
            QMessageBox::warning(
                this, QStringLiteral("恢复失败"),
                error + QStringLiteral("\n\n迁移日志已保留，下次启动会再次提示。"));
            return;
        }
    } else {
        return;
    }

    QMessageBox::information(this, QStringLiteral("迁移恢复"),
                             log + QStringLiteral("\n已处理完成。"));
    refresh();
}

void MainWindow::relaunchAsAdmin()
{
    wchar_t path[MAX_PATH] = {};
    if (!GetModuleFileNameW(nullptr, path, MAX_PATH))
        return;

    SHELLEXECUTEINFOW info = {};
    info.cbSize = sizeof(info);
    info.lpVerb = L"runas";
    info.lpFile = path;
    info.nShow = SW_SHOWNORMAL;

    if (ShellExecuteExW(&info)) {
        QApplication::quit();
    } else {
        QMessageBox::warning(
            this,
            QStringLiteral("提权失败"),
            QStringLiteral("无法以管理员身份重新启动（错误码 %1）。").arg(GetLastError()));
    }
}

void MainWindow::showAbout()
{
    QMessageBox::about(
        this,
        QStringLiteral("关于 Disk Expander"),
        QStringLiteral(
            "<h3>Disk Expander 0.3</h3>"
            "<p>Windows 磁盘布局与空间管理工具。</p>"
            "<p>磁盘布局为只读；释放空间、迁移与回滚会修改文件，请先阅读说明。</p>"
            "<p style='color:#6B7280'>迁移日志：C:\\ProgramData\\DiskExpander\\journal.json</p>"));
}

void MainWindow::appendLog(const QString &text)
{
    if (!m_log)
        return;
    m_log->appendPlainText(text);
}

void MainWindow::showDemoData()
{
    const qint64 mib = 1024LL * 1024LL;
    const qint64 gib = 1024LL * mib;

    ScanResult demo;
    demo.elevated = true;

    auto addVolume = [&demo](const QString &guid, const QString &letter,
                             const QString &label, const QString &fs,
                             qint64 size, qint64 free, bool boot) {
        VolumeInfo volume;
        volume.guid = guid;
        volume.guidPath = QStringLiteral("\\\\?\\") + guid + QStringLiteral("\\");
        volume.driveLetter = letter;
        volume.label = label;
        volume.fileSystem = fs;
        volume.size = size;
        volume.freeBytes = free;
        volume.isBootVolume = boot;
        if (!letter.isEmpty())
            volume.mountPoints << letter + QStringLiteral("\\");
        demo.volumes << volume;
    };

    DiskInfo systemDisk;
    systemDisk.number = 0;
    systemDisk.model = QStringLiteral("SanDisk SD9SN8W128G1002");
    systemDisk.busType = QStringLiteral("SATA");
    systemDisk.size = static_cast<qint64>(119.2 * gib);
    systemDisk.sectorSize = 512;
    systemDisk.style = DiskStyle::Gpt;
    systemDisk.isSystemDisk = true;

    PartitionInfo efi;
    efi.diskNumber = 0;
    efi.partitionNumber = 1;
    efi.offset = 1 * mib;
    efi.length = 100 * mib;
    efi.role = PartitionRole::Efi;
    efi.isSystem = true;
    efi.name = QStringLiteral("EFI system partition");

    PartitionInfo msr;
    msr.diskNumber = 0;
    msr.partitionNumber = 2;
    msr.offset = 101 * mib;
    msr.length = 16 * mib;
    msr.role = PartitionRole::Msr;

    PartitionInfo windows;
    windows.diskNumber = 0;
    windows.partitionNumber = 3;
    windows.offset = 117 * mib;
    windows.length = static_cast<qint64>(118.2 * gib);
    windows.role = PartitionRole::Basic;
    windows.isBoot = true;
    windows.volumeGuid = QStringLiteral("{cac6d370-c76a-401c-acd4-8dea8c35d164}");

    PartitionInfo recovery;
    recovery.diskNumber = 0;
    recovery.partitionNumber = 4;
    recovery.length = 700 * mib;
    recovery.offset = systemDisk.size - recovery.length;
    recovery.role = PartitionRole::Recovery;
    recovery.name = QStringLiteral("WinRE");

    systemDisk.partitions << efi << msr << windows << recovery;
    const qint64 gap = recovery.offset - (windows.offset + windows.length);
    if (gap > 0)
        systemDisk.unallocated << UnallocatedRegion{windows.offset + windows.length, gap};

    DiskInfo dataDisk;
    dataDisk.number = 1;
    dataDisk.model = QStringLiteral("TOSHIBA MQ04ABF100");
    dataDisk.busType = QStringLiteral("SATA");
    dataDisk.size = static_cast<qint64>(931.5 * gib);
    dataDisk.sectorSize = 512;
    dataDisk.style = DiskStyle::Gpt;

    PartitionInfo d;
    d.diskNumber = 1;
    d.partitionNumber = 1;
    d.offset = 1 * mib;
    d.length = static_cast<qint64>(341.8 * gib);
    d.role = PartitionRole::Basic;
    d.volumeGuid = QStringLiteral("{86254aa1-d20e-4ff9-b39a-29c2cc78fbaf}");

    PartitionInfo e;
    e.diskNumber = 1;
    e.partitionNumber = 2;
    e.offset = d.offset + d.length;
    e.length = static_cast<qint64>(302.7 * gib);
    e.role = PartitionRole::Basic;
    e.volumeGuid = QStringLiteral("{3631f10f-69fb-4721-bd74-22fc1b924ae9}");

    PartitionInfo f;
    f.diskNumber = 1;
    f.partitionNumber = 3;
    f.offset = e.offset + e.length;
    f.length = static_cast<qint64>(287.0 * gib);
    f.role = PartitionRole::Basic;
    f.volumeGuid = QStringLiteral("{c003da1c-ee90-4d49-bbc9-84439ec50a04}");

    dataDisk.partitions << d << e << f;
    const qint64 tail = dataDisk.size - (f.offset + f.length);
    if (tail > 0)
        dataDisk.unallocated << UnallocatedRegion{f.offset + f.length, tail};

    demo.disks << systemDisk << dataDisk;

    addVolume(QStringLiteral("{00000000-0000-0000-0000-000000000001}"), QString(),
              QString(), QStringLiteral("FAT32"), 100 * mib, 70 * mib, false);
    addVolume(QStringLiteral("{cac6d370-c76a-401c-acd4-8dea8c35d164}"), QStringLiteral("C:"),
              QStringLiteral("Windows"), QStringLiteral("NTFS"),
              static_cast<qint64>(118.2 * gib), static_cast<qint64>(8.6 * gib), true);
    addVolume(QStringLiteral("{00000000-0000-0000-0000-000000000002}"), QString(),
              QStringLiteral("WinRE"), QStringLiteral("NTFS"), 700 * mib, 500 * mib, false);
    addVolume(QStringLiteral("{86254aa1-d20e-4ff9-b39a-29c2cc78fbaf}"), QStringLiteral("D:"),
              QStringLiteral("Data"), QStringLiteral("NTFS"),
              static_cast<qint64>(341.8 * gib), static_cast<qint64>(189.4 * gib), false);
    addVolume(QStringLiteral("{3631f10f-69fb-4721-bd74-22fc1b924ae9}"), QStringLiteral("E:"),
              QStringLiteral("Media"), QStringLiteral("NTFS"),
              static_cast<qint64>(302.7 * gib), static_cast<qint64>(203.8 * gib), false);
    addVolume(QStringLiteral("{c003da1c-ee90-4d49-bbc9-84439ec50a04}"), QStringLiteral("F:"),
              QStringLiteral("Backup"), QStringLiteral("NTFS"),
              static_cast<qint64>(287.0 * gib), static_cast<qint64>(231.6 * gib), false);

    applyScan(demo);
}
