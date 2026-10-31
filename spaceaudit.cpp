#include "spaceaudit.h"

#ifndef NOMINMAX
#  define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>

#include <algorithm>
#include <functional>
#include <utility>

namespace {

constexpr qint64 kMiB = 1024LL * 1024LL;
constexpr qint64 kGiB = 1024LL * kMiB;

// 长路径前缀，避免 MAX_PATH 限制。
QString extendedPath(const QString &path)
{
    QString native = QDir::toNativeSeparators(path);
    if (native.startsWith(QStringLiteral("\\\\?\\")))
        return native;
    if (native.startsWith(QStringLiteral("\\\\")))
        return QStringLiteral("\\\\?\\UNC\\") + native.mid(2);
    return QStringLiteral("\\\\?\\") + native;
}

class FsWalker
{
public:
    FsWalker(std::atomic_bool &cancel,
             std::function<void(const QString &, qint64, int)> progress)
        : m_cancel(cancel)
        , m_progress(std::move(progress))
    {
    }

    qint64 directorySize(const QString &path)
    {
        qint64 size = 0;
        scan(path, size, 0);
        return size;
    }

    qint64 totalBytes() const { return m_totalBytes; }
    int files() const { return m_files; }

private:
    void scan(const QString &dir, qint64 &size, int depth)
    {
        if (m_cancel.load(std::memory_order_relaxed) || depth > 64)
            return;

        const QString pattern = extendedPath(dir) + QStringLiteral("\\*");
        WIN32_FIND_DATAW data = {};
        HANDLE handle = FindFirstFileW(reinterpret_cast<LPCWSTR>(pattern.utf16()), &data);
        if (handle == INVALID_HANDLE_VALUE)
            return;

        do {
            if (m_cancel.load(std::memory_order_relaxed))
                break;

            const QString name = QString::fromWCharArray(data.cFileName);
            if (name == QStringLiteral(".") || name == QStringLiteral(".."))
                continue;
            if (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
                continue;

            const QString full = dir + QLatin1Char('\\') + name;
            if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                if (name.compare(QStringLiteral("System Volume Information"),
                                 Qt::CaseInsensitive) == 0)
                    continue;
                scan(full, size, depth + 1);
            } else {
                const qint64 length =
                    (static_cast<qint64>(data.nFileSizeHigh) << 32)
                    | static_cast<qint64>(data.nFileSizeLow);
                size += length;
                m_totalBytes += length;
                ++m_files;

                if ((m_files & 0x7FF) == 0 && m_progress)
                    m_progress(full, m_totalBytes, m_files);
            }
        } while (FindNextFileW(handle, &data));

        FindClose(handle);
    }

    std::atomic_bool &m_cancel;
    std::function<void(const QString &, qint64, int)> m_progress;
    qint64 m_totalBytes = 0;
    int m_files = 0;
};

}  // namespace

SpaceAuditWorker::SpaceAuditWorker(QString root, QObject *parent)
    : QObject(parent)
    , m_root(std::move(root))
{
}

void SpaceAuditWorker::requestCancel()
{
    m_cancel.store(true, std::memory_order_relaxed);
}

void SpaceAuditWorker::process()
{
    AuditResult result;

    FsWalker walker(m_cancel, [this](const QString &path, qint64 bytes, int files) {
        emit progress(path, bytes, files);
    });

    QString root = QDir::toNativeSeparators(m_root);
    if (!root.endsWith(QLatin1Char('\\')))
        root += QLatin1Char('\\');

    const QString windows = root + QStringLiteral("Windows");
    const QString programFiles = root + QStringLiteral("Program Files");
    const QString programFilesX86 = root + QStringLiteral("Program Files (x86)");
    const QString programData = root + QStringLiteral("ProgramData");

    QVector<AuditItem> items;

    auto addItem = [&](const QString &category, const QString &name, const QString &path,
                       const QString &advice, AuditKind kind) {
        if (m_cancel.load(std::memory_order_relaxed))
            return;
        const QFileInfo info(path);
        if (!info.exists())
            return;

        const qint64 bytes = info.isDir() ? walker.directorySize(path) : info.size();
        if (bytes <= 0)
            return;

        AuditItem item;
        item.category = category;
        item.name = name;
        item.path = QDir::toNativeSeparators(path);
        item.bytes = bytes;
        item.advice = advice;
        item.kind = kind;
        items.append(item);

        emit progress(item.path, walker.totalBytes(), walker.files());
    };

    auto addChildren = [&](const QString &parent, const QString &category,
                           const QString &advice, AuditKind kind,
                           qint64 threshold, int limit,
                           const QStringList &exclude = QStringList()) {
        QDir dir(parent);
        if (!dir.exists())
            return;

        QVector<AuditItem> children;
        const QFileInfoList entries = dir.entryInfoList(
            QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);

        for (const QFileInfo &entry : entries) {
            if (m_cancel.load(std::memory_order_relaxed))
                break;
            if (exclude.contains(entry.fileName(), Qt::CaseInsensitive))
                continue;

            const qint64 bytes = walker.directorySize(entry.absoluteFilePath());
            if (bytes < threshold)
                continue;

            AuditItem item;
            item.category = category;
            item.name = entry.fileName();
            item.path = QDir::toNativeSeparators(entry.absoluteFilePath());
            item.bytes = bytes;
            item.advice = advice;
            item.kind = kind;
            children.append(item);

            emit progress(item.path, walker.totalBytes(), walker.files());
        }

        std::sort(children.begin(), children.end(),
                  [](const AuditItem &a, const AuditItem &b) {
                      return a.bytes > b.bytes;
                  });

        const int count = qMin(limit, children.size());
        for (int i = 0; i < count; ++i)
            items.append(children.at(i));
    };

    // ---------- 系统可清理项 ----------
    addItem(QStringLiteral("系统可清理"), QStringLiteral("休眠文件 hiberfil.sys"),
            root + QStringLiteral("hiberfil.sys"),
            QStringLiteral("关闭休眠可释放：powercfg /h off"), AuditKind::Cleanable);
    addItem(QStringLiteral("系统可清理"), QStringLiteral("页面文件 pagefile.sys"),
            root + QStringLiteral("pagefile.sys"),
            QStringLiteral("可移到其他盘，需重启"), AuditKind::Cleanable);
    addItem(QStringLiteral("系统可清理"), QStringLiteral("交换文件 swapfile.sys"),
            root + QStringLiteral("swapfile.sys"),
            QStringLiteral("随页面文件一起移动"), AuditKind::Cleanable);
    addItem(QStringLiteral("系统可清理"), QStringLiteral("回收站"),
            root + QStringLiteral("$Recycle.Bin"),
            QStringLiteral("清空回收站即可释放"), AuditKind::Cleanable);
    addItem(QStringLiteral("系统可清理"), QStringLiteral("Windows 临时文件"),
            windows + QStringLiteral("\\Temp"),
            QStringLiteral("可清理"), AuditKind::Cleanable);
    addItem(QStringLiteral("系统可清理"), QStringLiteral("Windows 更新缓存"),
            windows + QStringLiteral("\\SoftwareDistribution\\Download"),
            QStringLiteral("可清理，建议先停止 Windows Update 服务"), AuditKind::Cleanable);
    addItem(QStringLiteral("系统可清理"), QStringLiteral("传递优化缓存"),
            windows + QStringLiteral("\\ServiceProfiles\\NetworkService\\AppData\\Local\\Microsoft\\Windows\\DeliveryOptimization"),
            QStringLiteral("可清理"), AuditKind::Cleanable);
    addItem(QStringLiteral("系统可清理"), QStringLiteral("内存转储 MEMORY.DMP"),
            windows + QStringLiteral("\\MEMORY.DMP"),
            QStringLiteral("可删除"), AuditKind::Cleanable);
    addItem(QStringLiteral("系统可清理"), QStringLiteral("小型转储 Minidump"),
            windows + QStringLiteral("\\Minidump"),
            QStringLiteral("可删除"), AuditKind::Cleanable);
    addItem(QStringLiteral("系统可清理"), QStringLiteral("旧系统备份 Windows.old"),
            root + QStringLiteral("Windows.old"),
            QStringLiteral("确认无用后删除"), AuditKind::Cleanable);
    addItem(QStringLiteral("系统可清理"), QStringLiteral("更新残留 $WinREAgent"),
            root + QStringLiteral("$WinREAgent"),
            QStringLiteral("Windows 更新残留，可删"), AuditKind::Cleanable);
    addItem(QStringLiteral("系统可清理"), QStringLiteral("内核报告 LiveKernelReports"),
            windows + QStringLiteral("\\LiveKernelReports"),
            QStringLiteral("可删"), AuditKind::Cleanable);

    // WinSxS 最后扫，耗时最长。
    addItem(QStringLiteral("组件存储（需 DISM）"), QStringLiteral("WinSxS 组件存储"),
            windows + QStringLiteral("\\WinSxS"),
            QStringLiteral("显示值含硬链接，实际可回收更少；用 DISM 清理"),
            AuditKind::Caution);

    // ---------- 用户目录 ----------
    const QString profile = QString::fromLocal8Bit(qgetenv("USERPROFILE"));
    if (!profile.isEmpty()) {
        addItem(QStringLiteral("用户数据（可迁移）"), QStringLiteral("下载 Downloads"),
                profile + QStringLiteral("\\Downloads"),
                QStringLiteral("可迁移到 D/E/F"), AuditKind::Movable);
        addItem(QStringLiteral("用户数据（可迁移）"), QStringLiteral("桌面 Desktop"),
                profile + QStringLiteral("\\Desktop"),
                QStringLiteral("可迁移"), AuditKind::Movable);
        addItem(QStringLiteral("用户数据（可迁移）"), QStringLiteral("图片 Pictures"),
                profile + QStringLiteral("\\Pictures"),
                QStringLiteral("可迁移"), AuditKind::Movable);
        addItem(QStringLiteral("用户数据（可迁移）"), QStringLiteral("视频 Videos"),
                profile + QStringLiteral("\\Videos"),
                QStringLiteral("可迁移"), AuditKind::Movable);
        addItem(QStringLiteral("用户数据（可迁移）"), QStringLiteral("音乐 Music"),
                profile + QStringLiteral("\\Music"),
                QStringLiteral("可迁移"), AuditKind::Movable);

        addItem(QStringLiteral("用户缓存"), QStringLiteral("用户临时文件"),
                profile + QStringLiteral("\\AppData\\Local\\Temp"),
                QStringLiteral("可清理"), AuditKind::Cleanable);
        addItem(QStringLiteral("用户缓存"), QStringLiteral("资源管理器缓存"),
                profile + QStringLiteral("\\AppData\\Local\\Microsoft\\Windows\\Explorer"),
                QStringLiteral("缩略图/图标缓存，可清理"), AuditKind::Cleanable);
        addItem(QStringLiteral("用户缓存"), QStringLiteral("崩溃转储 CrashDumps"),
                profile + QStringLiteral("\\AppData\\Local\\CrashDumps"),
                QStringLiteral("崩溃转储，可清理"), AuditKind::Cleanable);

        // 文档里通常藏着微信/QQ 的聊天文件。
        const int before = items.size();
        addChildren(profile + QStringLiteral("\\Documents"),
                    QStringLiteral("用户数据（可迁移）"),
                    QStringLiteral("可迁移到其他盘"), AuditKind::Movable,
                    512 * kMiB, 8);
        if (items.size() == before) {
            addItem(QStringLiteral("用户数据（可迁移）"), QStringLiteral("文档 Documents"),
                    profile + QStringLiteral("\\Documents"),
                    QStringLiteral("可迁移到 D/E/F"), AuditKind::Movable);
        }

        addChildren(profile + QStringLiteral("\\AppData\\Local"),
                    QStringLiteral("应用缓存（谨慎）"),
                    QStringLiteral("确认后再清理/迁移"), AuditKind::Caution,
                    1 * kGiB, 8);
    }

    // ---------- 程序与程序数据 ----------
    addChildren(programFiles, QStringLiteral("程序（不建议直接搬）"),
                QStringLiteral("需卸载重装或改安装路径"), AuditKind::Caution,
                1 * kGiB, 10);
    addChildren(programFilesX86, QStringLiteral("程序（不建议直接搬）"),
                QStringLiteral("需卸载重装或改安装路径"), AuditKind::Caution,
                1 * kGiB, 10);
    addChildren(programData, QStringLiteral("程序数据（谨慎）"),
                QStringLiteral("部分可迁移，需逐个确认"), AuditKind::Caution,
                1 * kGiB, 10);

    // ---------- 根目录下其他大目录 ----------
    const QStringList rootExclude = {
        QStringLiteral("Windows"),
        QStringLiteral("Users"),
        QStringLiteral("Program Files"),
        QStringLiteral("Program Files (x86)"),
        QStringLiteral("ProgramData"),
        QStringLiteral("$Recycle.Bin"),
        QStringLiteral("System Volume Information"),
        QStringLiteral("Windows.old"),
        QStringLiteral("$WinREAgent"),
        QStringLiteral("Recovery"),
        QStringLiteral("Documents and Settings"),
    };
    addChildren(root, QStringLiteral("其他目录（可迁移）"),
                QStringLiteral("可整体迁到其他盘"), AuditKind::Movable,
                1 * kGiB, 10, rootExclude);

    std::sort(items.begin(), items.end(), [](const AuditItem &a, const AuditItem &b) {
        return a.bytes > b.bytes;
    });

    for (const AuditItem &item : items) {
        switch (item.kind) {
        case AuditKind::Cleanable: result.cleanableBytes += item.bytes; break;
        case AuditKind::Movable:   result.movableBytes += item.bytes; break;
        case AuditKind::Caution:   result.cautionBytes += item.bytes; break;
        }
    }

    result.items = items;
    result.scannedBytes = walker.totalBytes();
    result.files = walker.files();
    result.cancelled = m_cancel.load(std::memory_order_relaxed);

    emit finished(result);
}

QJsonObject auditToJson(const AuditResult &result)
{
    QJsonObject object;
    object.insert(QStringLiteral("scannedBytes"), static_cast<double>(result.scannedBytes));
    object.insert(QStringLiteral("cleanableBytes"), static_cast<double>(result.cleanableBytes));
    object.insert(QStringLiteral("movableBytes"), static_cast<double>(result.movableBytes));
    object.insert(QStringLiteral("cautionBytes"), static_cast<double>(result.cautionBytes));
    object.insert(QStringLiteral("files"), result.files);
    object.insert(QStringLiteral("cancelled"), result.cancelled);

    QJsonArray items;
    for (const AuditItem &item : result.items) {
        QJsonObject entry;
        entry.insert(QStringLiteral("category"), item.category);
        entry.insert(QStringLiteral("name"), item.name);
        entry.insert(QStringLiteral("path"), item.path);
        entry.insert(QStringLiteral("bytes"), static_cast<double>(item.bytes));
        entry.insert(QStringLiteral("advice"), item.advice);
        entry.insert(QStringLiteral("kind"), static_cast<int>(item.kind));
        items.append(entry);
    }
    object.insert(QStringLiteral("items"), items);
    return object;
}
