#include "rollback.h"
#include "diskscanner.h"

#ifndef NOMINMAX
#  define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winioctl.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>

#include <cstddef>
#include <utility>
#include <vector>

namespace {

QString extendedPath(const QString &path)
{
    QString native = QDir::toNativeSeparators(path);
    if (native.startsWith(QStringLiteral("\\\\?\\")))
        return native;
    if (native.startsWith(QStringLiteral("\\\\")))
        return QStringLiteral("\\\\?\\UNC\\") + native.mid(2);
    return QStringLiteral("\\\\?\\") + native;
}

QString normalized(const QString &path)
{
    QString value = QDir::fromNativeSeparators(QDir::cleanPath(path));
    while (value.endsWith(QLatin1Char('/')))
        value.chop(1);
    return value;
}

bool isUnder(const QString &path, const QString &root)
{
    const QString value = normalized(path).toLower();
    const QString prefix = normalized(root).toLower();
    if (prefix.isEmpty())
        return false;
    return value == prefix || value.startsWith(prefix + QLatin1Char('/'));
}

qint64 directorySize(const QString &path)
{
    qint64 total = 0;

    WIN32_FIND_DATAW data = {};
    const QString pattern = extendedPath(path) + QStringLiteral("\\*");
    HANDLE handle = FindFirstFileW(reinterpret_cast<LPCWSTR>(pattern.utf16()), &data);
    if (handle == INVALID_HANDLE_VALUE)
        return 0;

    do {
        const QString name = QString::fromWCharArray(data.cFileName);
        if (name == QStringLiteral(".") || name == QStringLiteral(".."))
            continue;
        if (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
            continue;

        const QString full = path + QLatin1Char('\\') + name;
        if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            total += directorySize(full);
        else
            total += (static_cast<qint64>(data.nFileSizeHigh) << 32)
                     | static_cast<qint64>(data.nFileSizeLow);
    } while (FindNextFileW(handle, &data));

    FindClose(handle);
    return total;
}

qint64 freeSpace(const QString &root)
{
    ULARGE_INTEGER available = {};
    if (!GetDiskFreeSpaceExW(reinterpret_cast<LPCWSTR>(root.utf16()),
                             &available, nullptr, nullptr))
        return 0;
    return static_cast<qint64>(available.QuadPart);
}

bool removeTree(const QString &path, QString *error)
{
    const QString longDir = extendedPath(path);
    WIN32_FIND_DATAW data = {};
    HANDLE handle = FindFirstFileW(
        reinterpret_cast<LPCWSTR>((longDir + QStringLiteral("\\*")).utf16()), &data);

    if (handle != INVALID_HANDLE_VALUE) {
        do {
            const QString name = QString::fromWCharArray(data.cFileName);
            if (name == QStringLiteral(".") || name == QStringLiteral(".."))
                continue;

            const QString full = path + QLatin1Char('\\') + name;
            const QString longFull = extendedPath(full);

            if (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
                if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                    RemoveDirectoryW(reinterpret_cast<LPCWSTR>(longFull.utf16()));
                else
                    DeleteFileW(reinterpret_cast<LPCWSTR>(longFull.utf16()));
                continue;
            }

            if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                if (!removeTree(full, error)) {
                    FindClose(handle);
                    return false;
                }
            } else {
                SetFileAttributesW(reinterpret_cast<LPCWSTR>(longFull.utf16()),
                                   FILE_ATTRIBUTE_NORMAL);
                DeleteFileW(reinterpret_cast<LPCWSTR>(longFull.utf16()));
            }
        } while (FindNextFileW(handle, &data));
        FindClose(handle);
    }

    SetFileAttributesW(reinterpret_cast<LPCWSTR>(longDir.utf16()), FILE_ATTRIBUTE_NORMAL);
    for (int attempt = 0; attempt < 6; ++attempt) {
        if (RemoveDirectoryW(reinterpret_cast<LPCWSTR>(longDir.utf16())))
            return true;
        ::Sleep(150);
    }

    *error = QStringLiteral("目录仍被占用");
    return false;
}

// 读取 junction 的目标路径；不是 junction/无法解析时返回空。
QString junctionTarget(const QString &path)
{
    HANDLE handle = CreateFileW(
        reinterpret_cast<LPCWSTR>(extendedPath(path).utf16()),
        0,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS,
        nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        return {};

    std::vector<wchar_t> buffer(32768);
    const DWORD length = GetFinalPathNameByHandleW(
        handle,
        buffer.data(),
        static_cast<DWORD>(buffer.size()),
        FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    CloseHandle(handle);

    if (length == 0 || length >= buffer.size())
        return {};

    QString target = QString::fromWCharArray(buffer.data(), static_cast<int>(length));
    if (target.startsWith(QStringLiteral("\\\\?\\UNC\\")))
        target = QStringLiteral("\\\\") + target.mid(8);
    else if (target.startsWith(QStringLiteral("\\\\?\\")))
        target = target.mid(4);

    return QDir::toNativeSeparators(target);
}

bool createJunction(const QString &source, const QString &target, QString *error)
{
    QProcess process;
    process.setProgram(QStringLiteral("cmd.exe"));
    process.setArguments({QStringLiteral("/c"),
                          QStringLiteral("mklink"),
                          QStringLiteral("/J"),
                          QDir::toNativeSeparators(source),
                          QDir::toNativeSeparators(target)});
    const QString errorPath =
        QDir(QDir::tempPath()).absoluteFilePath(QStringLiteral("disk_expander_rollback_link.err"));
    process.setStandardOutputFile(
        QDir(QDir::tempPath()).absoluteFilePath(QStringLiteral("disk_expander_rollback_link.out")),
        QIODevice::Truncate);
    process.setStandardErrorFile(errorPath, QIODevice::Truncate);
    process.start();
    process.waitForFinished(60000);

    if (process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0)
        return true;

    QFile file(errorPath);
    QString detail;
    if (file.open(QIODevice::ReadOnly))
        detail = QString::fromLocal8Bit(file.readAll()).trimmed();
    *error = QStringLiteral("恢复 junction 失败：%1").arg(detail);
    return false;
}

bool copyBack(const QString &source, const QString &target, QString *error)
{
    QProcess robocopy;
    robocopy.setProgram(QStringLiteral("C:\\Windows\\System32\\robocopy.exe"));
    robocopy.setArguments({
        QDir::toNativeSeparators(source),
        QDir::toNativeSeparators(target),
        QStringLiteral("/E"),
        QStringLiteral("/COPY:DAT"),
        QStringLiteral("/DCOPY:DAT"),
        QStringLiteral("/R:1"),
        QStringLiteral("/W:1"),
        QStringLiteral("/XJ"),
        QStringLiteral("/NFL"),
        QStringLiteral("/NDL"),
        QStringLiteral("/NP"),
    });
    const QString outputPath =
        QDir(QDir::tempPath()).absoluteFilePath(QStringLiteral("disk_expander_rollback.out"));
    const QString errorPath =
        QDir(QDir::tempPath()).absoluteFilePath(QStringLiteral("disk_expander_rollback.err"));
    robocopy.setStandardOutputFile(outputPath, QIODevice::Truncate);
    robocopy.setStandardErrorFile(errorPath, QIODevice::Truncate);
    robocopy.start();
    if (!robocopy.waitForStarted(30000)) {
        *error = QStringLiteral("无法启动 robocopy");
        return false;
    }
    robocopy.waitForFinished(-1);

    if (robocopy.exitCode() >= 8) {
        QFile file(errorPath);
        QString detail;
        if (file.open(QIODevice::ReadOnly))
            detail = QString::fromLocal8Bit(file.readAll()).trimmed();
        *error = QStringLiteral("robocopy 失败（退出码 %1）%2")
                     .arg(robocopy.exitCode())
                     .arg(detail.isEmpty() ? QString() : QStringLiteral("：") + detail);
        return false;
    }
    return true;
}

void scanDirectory(const QString &dir,
                   int depth,
                   const QString &migrationRoot,
                   QVector<MigrationLink> &links)
{
    if (depth > 8)
        return;

    WIN32_FIND_DATAW data = {};
    const QString pattern = extendedPath(dir) + QStringLiteral("\\*");
    HANDLE handle = FindFirstFileW(reinterpret_cast<LPCWSTR>(pattern.utf16()), &data);
    if (handle == INVALID_HANDLE_VALUE)
        return;

    do {
        const QString name = QString::fromWCharArray(data.cFileName);
        if (name == QStringLiteral(".") || name == QStringLiteral(".."))
            continue;
        if (!(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
            continue;

        const QString full = dir + QLatin1Char('\\') + name;

        if (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
            const QString target = junctionTarget(full);
            if (!target.isEmpty() && isUnder(target, migrationRoot)) {
                MigrationLink link;
                link.source = QDir::toNativeSeparators(full);
                link.target = QDir::toNativeSeparators(target);
                link.bytes = directorySize(target);
                links.append(link);
            }
            continue;  // 不进入 junction
        }

        if (name.compare(QStringLiteral("System Volume Information"), Qt::CaseInsensitive) == 0)
            continue;

        scanDirectory(full, depth + 1, migrationRoot, links);
    } while (FindNextFileW(handle, &data));

    FindClose(handle);
}

bool isJunctionTo(const QString &source, const QString &target)
{
    const QString current = junctionTarget(source);
    return !current.isEmpty()
           && normalized(current).compare(normalized(target), Qt::CaseInsensitive) == 0;
}

}  // namespace

QVector<MigrationLink> findMigrationLinks(const QString &scanRoot,
                                          const QString &migrationRoot)
{
    QVector<MigrationLink> links;
    if (!QFileInfo(scanRoot).isDir() || migrationRoot.trimmed().isEmpty())
        return links;

    scanDirectory(QDir::toNativeSeparators(scanRoot), 0,
                  QDir::toNativeSeparators(migrationRoot), links);
    return links;
}

bool rollbackLink(const MigrationLink &link,
                  const std::function<void(const QString &)> &log,
                  QString *error)
{
    const QString currentTarget = junctionTarget(link.source);
    if (currentTarget.isEmpty()) {
        *error = QStringLiteral("源路径不是 junction：%1").arg(link.source);
        return false;
    }
    if (normalized(currentTarget).compare(normalized(link.target), Qt::CaseInsensitive) != 0) {
        *error = QStringLiteral("junction 指向已变化：%1").arg(currentTarget);
        return false;
    }

    const qint64 needed = directorySize(link.target);
    const qint64 available = freeSpace(QStringLiteral("C:\\"));
    if (available < needed + (512LL * 1024 * 1024)) {
        *error = QStringLiteral("C: 空间不足：需要 %1，可用 %2")
                     .arg(DiskScanner::formatBytes(needed),
                          DiskScanner::formatBytes(available));
        return false;
    }

    if (!RemoveDirectoryW(
            reinterpret_cast<LPCWSTR>(extendedPath(link.source).utf16()))) {
        *error = QStringLiteral("无法删除 junction（可能被占用）");
        return false;
    }

    log(QStringLiteral("移回 %1 → %2").arg(link.target, link.source));

    if (!copyBack(link.target, link.source, error)) {
        QString linkError;
        createJunction(link.source, link.target, &linkError);
        *error += QStringLiteral("；已尝试恢复 junction");
        return false;
    }

    if (directorySize(link.source) < needed) {
        QString removeError;
        removeTree(link.source, &removeError);
        QString linkError;
        createJunction(link.source, link.target, &linkError);
        *error = QStringLiteral("校验失败，已尝试恢复 junction");
        return false;
    }

    QString removeError;
    if (!removeTree(link.target, &removeError))
        log(QStringLiteral("警告：D: 上的原数据未完全删除，请手动清理 %1").arg(link.target));

    return true;
}

bool completeMigration(const MigrationJournal &entry,
                       const std::function<void(const QString &)> &log,
                       QString *error)
{
    const QString backup = entry.source + QStringLiteral(".migrating");

    if (entry.step == QStringLiteral("linked")) {
        if (!isJunctionTo(entry.source, entry.target)) {
            *error = QStringLiteral("junction 状态与日志不符");
            return false;
        }
        if (QFileInfo::exists(backup)) {
            QString removeError;
            if (!removeTree(backup, &removeError)) {
                *error = QStringLiteral("删除备份失败：%1").arg(removeError);
                return false;
            }
        }
        clearMigrationJournal();
        log(QStringLiteral("迁移已完成，C: 上的备份已清理。"));
        return true;
    }

    if (entry.step == QStringLiteral("renamed")) {
        if (!QFileInfo::exists(backup) || !QFileInfo(entry.target).isDir()) {
            *error = QStringLiteral("缺少备份目录或目标目录，无法完成");
            return false;
        }
        if (!createJunction(entry.source, entry.target, error))
            return false;

        if (directorySize(entry.source) < directorySize(entry.target)) {
            RemoveDirectoryW(
                reinterpret_cast<LPCWSTR>(extendedPath(entry.source).utf16()));
            *error = QStringLiteral("junction 校验失败");
            return false;
        }

        QString removeError;
        if (!removeTree(backup, &removeError)) {
            *error = QStringLiteral("删除备份失败：%1").arg(removeError);
            return false;
        }
        clearMigrationJournal();
        log(QStringLiteral("迁移已完成。"));
        return true;
    }

    *error = QStringLiteral("当前步骤不支持自动完成，请选择撤销。");
    return false;
}

bool undoMigration(const MigrationJournal &entry,
                   const std::function<void(const QString &)> &log,
                   QString *error)
{
    const QString backup = entry.source + QStringLiteral(".migrating");

    if (!junctionTarget(entry.source).isEmpty()) {
        if (!RemoveDirectoryW(
                reinterpret_cast<LPCWSTR>(extendedPath(entry.source).utf16()))) {
            *error = QStringLiteral("无法删除 junction（可能被占用）");
            return false;
        }
        log(QStringLiteral("已删除 junction"));
    }

    if (!QFileInfo::exists(entry.source) && QFileInfo::exists(backup)) {
        if (!QDir().rename(backup, entry.source)) {
            *error = QStringLiteral("无法恢复原目录");
            return false;
        }
        log(QStringLiteral("已恢复原目录"));
    }

    if (!QFileInfo::exists(entry.source)) {
        *error = QStringLiteral("找不到原目录或备份，已保留 D: 上的数据");
        return false;
    }

    if (QFileInfo(entry.target).isDir()) {
        QString removeError;
        if (!removeTree(entry.target, &removeError))
            log(QStringLiteral("警告：D: 上的副本未完全删除：%1").arg(removeError));
    }

    clearMigrationJournal();
    log(QStringLiteral("已撤销迁移，原目录已恢复。"));
    return true;
}

RollbackWorker::RollbackWorker(QVector<MigrationLink> links, QObject *parent)    : QObject(parent)
    , m_links(std::move(links))
{
}

void RollbackWorker::requestCancel()
{
    m_cancel.store(true, std::memory_order_relaxed);
}

void RollbackWorker::run()
{
    int succeeded = 0;
    int failed = 0;
    int index = 0;

    for (const MigrationLink &link : m_links) {
        if (m_cancel.load(std::memory_order_relaxed))
            break;

        emit progress(index, m_links.size());
        emit logMessage(QStringLiteral("== 回滚 %1").arg(link.source));

        QString error;
        if (rollbackLink(link, [this](const QString &text) { emit logMessage(text); }, &error))
            ++succeeded;
        else {
            ++failed;
            emit logMessage(QStringLiteral("失败：%1").arg(error));
        }
        ++index;
    }

    emit progress(index, m_links.size());
    const QString summary = QStringLiteral("回滚完成：成功 %1 项，失败 %2 项。")
                                .arg(succeeded)
                                .arg(failed);
    emit finished(succeeded, failed, summary);
}
