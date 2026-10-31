#include "maintenance.h"
#include "diskscanner.h"
#include "rollback.h"

#ifndef NOMINMAX
#  define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QSaveFile>
#include <QStandardPaths>

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

// 递归删除，清只读属性、跳过 reparse point、最后对目录重试几次。
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

qint64 directorySize(const QString &path)
{
    qint64 total = 0;

    WIN32_FIND_DATAW data = {};
    const QString pattern = QDir::toNativeSeparators(path) + QStringLiteral("\\*");
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
        if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            total += directorySize(full);
        } else {
            total += (static_cast<qint64>(data.nFileSizeHigh) << 32)
                     | static_cast<qint64>(data.nFileSizeLow);
        }
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

// 删除目录内容，但遇到 junction/符号链接只删链接本身，不进入目标。
void clearDirectory(const QString &path)
{
    QDir dir(path);
    const QFileInfoList entries = dir.entryInfoList(
        QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);

    for (const QFileInfo &entry : entries) {
        const QString full = entry.absoluteFilePath();
        const DWORD attributes =
            GetFileAttributesW(reinterpret_cast<LPCWSTR>(full.utf16()));

        if (attributes != INVALID_FILE_ATTRIBUTES
            && (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
            if (attributes & FILE_ATTRIBUTE_DIRECTORY)
                RemoveDirectoryW(reinterpret_cast<LPCWSTR>(full.utf16()));
            else
                DeleteFileW(reinterpret_cast<LPCWSTR>(full.utf16()));
            continue;
        }

        if (entry.isDir())
            QDir(full).removeRecursively();
        else
            QFile::remove(full);
    }
}

bool runProcess(const QString &program, const QStringList &arguments,
                int timeoutMs, QString *error)
{
    const QString outputPath =
        QDir(QDir::tempPath()).absoluteFilePath(QStringLiteral("disk_expander_cmd.out"));
    const QString errorPath =
        QDir(QDir::tempPath()).absoluteFilePath(QStringLiteral("disk_expander_cmd.err"));

    QProcess process;
    process.setProgram(program);
    process.setArguments(arguments);
    process.setStandardOutputFile(outputPath, QIODevice::Truncate);
    process.setStandardErrorFile(errorPath, QIODevice::Truncate);
    process.start();

    if (!process.waitForStarted(30000)) {
        *error = QStringLiteral("无法启动 %1").arg(program);
        return false;
    }
    if (!process.waitForFinished(timeoutMs)) {
        process.kill();
        process.waitForFinished(5000);
        *error = QStringLiteral("%1 执行超时").arg(program);
        return false;
    }
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        QFile file(errorPath);
        QString text;
        if (file.open(QIODevice::ReadOnly))
            text = QString::fromLocal8Bit(file.readAll()).trimmed();
        *error = text.isEmpty()
                     ? QStringLiteral("%1 失败（退出码 %2）").arg(program).arg(process.exitCode())
                     : text;
        return false;
    }
    return true;
}

QString journalPath()
{
    const QByteArray custom = qgetenv("DISK_EXPANDER_JOURNAL");
    if (!custom.isEmpty())
        return QString::fromLocal8Bit(custom);
    return QStringLiteral("C:\\ProgramData\\DiskExpander\\journal.json");
}

bool writeMigrationJournal(const MigrationJournal &entry)
{
    const QString path = journalPath();
    QDir().mkpath(QFileInfo(path).absolutePath());

    QJsonObject object;
    object.insert(QStringLiteral("source"), entry.source);
    object.insert(QStringLiteral("target"), entry.target);
    object.insert(QStringLiteral("step"), entry.step);
    object.insert(QStringLiteral("startedAt"), entry.startedAt);
    object.insert(QStringLiteral("bytes"), static_cast<double>(entry.bytes));

    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return false;
    file.write(QJsonDocument(object).toJson(QJsonDocument::Indented));
    return file.commit();
}

}  // namespace

bool readMigrationJournal(MigrationJournal *entry)
{
    QFile file(journalPath());
    if (!file.open(QIODevice::ReadOnly))
        return false;

    const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
    if (!document.isObject())
        return false;

    const QJsonObject object = document.object();
    entry->source = object.value(QStringLiteral("source")).toString();
    entry->target = object.value(QStringLiteral("target")).toString();
    entry->step = object.value(QStringLiteral("step")).toString();
    entry->startedAt = object.value(QStringLiteral("startedAt")).toString();
    entry->bytes = static_cast<qint64>(object.value(QStringLiteral("bytes")).toDouble());

    return !entry->source.isEmpty() && !entry->target.isEmpty();
}

void clearMigrationJournal()
{
    QFile::remove(journalPath());
}

bool migrateDirectory(const QString &source,
                      const QString &targetRoot,
                      const std::function<void(const QString &)> &log,
                      QString *error)
{
    const QFileInfo sourceInfo(source);
    if (!sourceInfo.exists() || !sourceInfo.isDir()) {
        *error = QStringLiteral("源目录不存在：%1").arg(source);
        return false;
    }
    if (targetRoot.trimmed().isEmpty()) {
        *error = QStringLiteral("目标根目录为空");
        return false;
    }

    const QString name = sourceInfo.fileName();
    const QString destination = QDir(targetRoot).absoluteFilePath(name);

    if (QFileInfo::exists(destination)) {
        *error = QStringLiteral("目标已存在：%1").arg(destination);
        return false;
    }
    if (!QDir().mkpath(targetRoot)) {
        *error = QStringLiteral("无法创建目标目录：%1").arg(targetRoot);
        return false;
    }

    MigrationJournal journal;
    journal.source = QDir::toNativeSeparators(source);
    journal.target = QDir::toNativeSeparators(destination);
    journal.step = QStringLiteral("copying");
    journal.startedAt = QDateTime::currentDateTime().toString(Qt::ISODate);
    writeMigrationJournal(journal);

    auto failBeforeRename = [&](const QString &message) -> bool {
        QString removeError;
        removeTree(destination, &removeError);
        clearMigrationJournal();
        *error = message;
        return false;
    };

    log(QStringLiteral("复制 %1 → %2").arg(source, destination));

    QProcess robocopy;
    robocopy.setProgram(QStringLiteral("C:\\Windows\\System32\\robocopy.exe"));
    robocopy.setArguments({
        QDir::toNativeSeparators(source),
        QDir::toNativeSeparators(destination),
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
    const QString robocopyLog =
        QDir(QDir::tempPath()).absoluteFilePath(QStringLiteral("disk_expander_robocopy.log"));
    const QString robocopyError =
        QDir(QDir::tempPath()).absoluteFilePath(QStringLiteral("disk_expander_robocopy.err"));
    robocopy.setStandardOutputFile(robocopyLog, QIODevice::Truncate);
    robocopy.setStandardErrorFile(robocopyError, QIODevice::Truncate);
    robocopy.start();
    if (!robocopy.waitForStarted(30000))
        return failBeforeRename(QStringLiteral("无法启动 robocopy"));
    robocopy.waitForFinished(-1);

    const int code = robocopy.exitCode();
    if (code >= 8) {
        QFile file(robocopyError);
        QString detail;
        if (file.open(QIODevice::ReadOnly))
            detail = QString::fromLocal8Bit(file.readAll()).trimmed();
        return failBeforeRename(QStringLiteral("robocopy 失败（退出码 %1）%2")
                                    .arg(code)
                                    .arg(detail.isEmpty() ? QString()
                                                          : QStringLiteral("：") + detail));
    }

    const qint64 sourceBytes = directorySize(source);
    const qint64 destinationBytes = directorySize(destination);
    if (destinationBytes < sourceBytes) {
        return failBeforeRename(QStringLiteral("校验失败：源 %1，目标 %2")
                                    .arg(DiskScanner::formatBytes(sourceBytes),
                                         DiskScanner::formatBytes(destinationBytes)));
    }
    log(QStringLiteral("校验通过：%1").arg(DiskScanner::formatBytes(destinationBytes)));

    journal.step = QStringLiteral("copied");
    journal.bytes = destinationBytes;
    writeMigrationJournal(journal);

    const QString backup = source + QStringLiteral(".migrating");
    if (QFileInfo::exists(backup))
        return failBeforeRename(QStringLiteral("备份目录已存在：%1").arg(backup));

    if (!QDir().rename(source, backup))
        return failBeforeRename(QStringLiteral("无法重命名源目录（可能被占用）：%1").arg(source));

    journal.step = QStringLiteral("renamed");
    writeMigrationJournal(journal);

    log(QStringLiteral("建立 junction：%1 → %2").arg(source, destination));

    QProcess link;
    link.setProgram(QStringLiteral("cmd.exe"));
    link.setArguments({
        QStringLiteral("/c"),
        QStringLiteral("mklink"),
        QStringLiteral("/J"),
        QDir::toNativeSeparators(source),
        QDir::toNativeSeparators(destination),
    });
    const QString linkError =
        QDir(QDir::tempPath()).absoluteFilePath(QStringLiteral("disk_expander_link.err"));
    link.setStandardOutputFile(
        QDir(QDir::tempPath()).absoluteFilePath(QStringLiteral("disk_expander_link.out")),
        QIODevice::Truncate);
    link.setStandardErrorFile(linkError, QIODevice::Truncate);
    link.start();
    link.waitForFinished(60000);

    if (link.exitStatus() != QProcess::NormalExit || link.exitCode() != 0) {
        QFile file(linkError);
        QString detail;
        if (file.open(QIODevice::ReadOnly))
            detail = QString::fromLocal8Bit(file.readAll()).trimmed();

        const bool restored = QDir().rename(backup, source);
        if (restored) {
            QString removeError;
            removeTree(destination, &removeError);
            clearMigrationJournal();
        }
        *error = QStringLiteral("创建 junction 失败：%1").arg(detail);
        return false;
    }

    if (!QFileInfo(source).exists() || directorySize(source) < sourceBytes) {
        // 只删除 reparse point，不能递归删除，否则会删到 D: 上的数据。
        RemoveDirectoryW(
            reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(source).utf16()));

        const bool restored = QDir().rename(backup, source);
        if (restored) {
            QString removeError;
            removeTree(destination, &removeError);
            clearMigrationJournal();
        }
        *error = QStringLiteral("junction 校验失败，已回滚");
        return false;
    }

    journal.step = QStringLiteral("linked");
    writeMigrationJournal(journal);

    log(QStringLiteral("删除 C: 上的原目录备份…"));
    QString removeError;
    if (!removeTree(backup, &removeError)) {
        log(QStringLiteral("警告：原目录未完全删除（%1），请手动清理 %2")
                .arg(removeError, backup));
        log(QStringLiteral("迁移已完成，但 C: 上的备份未删净；下次启动会提示完成清理。"));
        return true;
    }

    clearMigrationJournal();
    log(QStringLiteral("迁移完成：%1").arg(destination));
    return true;
}

MaintenanceWorker::MaintenanceWorker(MaintenanceOptions options, QObject *parent)
    : QObject(parent)
    , m_options(std::move(options))
{
}

void MaintenanceWorker::requestCancel()
{
    m_cancel.store(true, std::memory_order_relaxed);
}

bool MaintenanceWorker::disableHibernate(QString *error)
{
    return runProcess(QStringLiteral("powercfg"),
                      {QStringLiteral("/h"), QStringLiteral("off")},
                      120000, error);
}

bool MaintenanceWorker::cleanup(QString *error)
{
    Q_UNUSED(error)

    QStringList targets;
    const QString temp = QString::fromLocal8Bit(qgetenv("TEMP"));
    if (!temp.isEmpty())
        targets << temp;
    targets << QStringLiteral("C:\\Windows\\Temp");
    targets << QStringLiteral("C:\\Windows\\SoftwareDistribution\\Download");

    for (const QString &path : targets) {
        if (m_cancel.load(std::memory_order_relaxed))
            break;
        if (!QFileInfo(path).isDir())
            continue;
        emit logMessage(QStringLiteral("清理 %1").arg(QDir::toNativeSeparators(path)));
        clearDirectory(path);
    }

    SHEmptyRecycleBinW(nullptr, nullptr,
                       SHERB_NOCONFIRMATION | SHERB_NOPROGRESSUI | SHERB_NOSOUND);
    emit logMessage(QStringLiteral("回收站已清空"));
    return true;
}

void MaintenanceWorker::run()
{
    const qint64 before = freeSpace(QStringLiteral("C:\\"));

    int total = 0;
    if (m_options.disableHibernate) ++total;
    if (m_options.cleanupTemp) ++total;
    if (m_options.migrateDownloads) ++total;
    if (m_options.migrateTencent) ++total;
    total = qMax(total, 1);

    int index = 0;
    auto step = [&](const QString &title, const std::function<bool(QString *)> &action) {
        if (m_cancel.load(std::memory_order_relaxed))
            return;
        emit stepStarted(index++, total, title);
        emit logMessage(QStringLiteral("== %1").arg(title));

        QString error;
        if (!action(&error))
            emit logMessage(QStringLiteral("失败：%1").arg(error));
    };

    if (m_options.disableHibernate) {
        step(QStringLiteral("关闭休眠"), [this](QString *error) {
            return disableHibernate(error);
        });
    }

    if (m_options.cleanupTemp) {
        step(QStringLiteral("清理临时文件"), [this](QString *error) {
            return cleanup(error);
        });
    }

    const QString profile = QString::fromLocal8Bit(qgetenv("USERPROFILE"));

    if (m_options.migrateDownloads && !profile.isEmpty()) {
        step(QStringLiteral("迁移 Downloads"), [this, profile](QString *error) {
            return migrateDirectory(profile + QStringLiteral("\\Downloads"),
                                    m_options.targetRoot,
                                    [this](const QString &text) { emit logMessage(text); },
                                    error);
        });
    }

    if (m_options.migrateTencent && !profile.isEmpty()) {
        step(QStringLiteral("迁移 Tencent Files"), [this, profile](QString *error) {
            return migrateDirectory(profile + QStringLiteral("\\Documents\\Tencent Files"),
                                    m_options.targetRoot,
                                    [this](const QString &text) { emit logMessage(text); },
                                    error);
        });
    }

    const qint64 after = freeSpace(QStringLiteral("C:\\"));
    const qint64 gained = after - before;

    QString summary;
    if (m_cancel.load(std::memory_order_relaxed))
        summary = QStringLiteral("操作已取消。");
    else
        summary = QStringLiteral("完成。C: 可用空间 %1 → %2（增加 %3）")
                      .arg(DiskScanner::formatBytes(before),
                           DiskScanner::formatBytes(after),
                           DiskScanner::formatBytes(qMax<qint64>(0, gained)));

    emit finished(gained >= 0, summary);
}

bool runSelfTest(QString *report)
{
    QString log;
    QString error;

    QString base = QString::fromLocal8Bit(qgetenv("DISK_EXPANDER_SELFTEST_DIR"));
    if (base.isEmpty())
        base = QDir(QDir::tempPath()).absoluteFilePath(QStringLiteral("DiskExpanderSelfTest"));
    base = QDir::toNativeSeparators(base);
    const QString source = base + QStringLiteral("\\src");
    const QString targetRoot = base + QStringLiteral("\\dst");
    const QString destination = targetRoot + QStringLiteral("\\src");

    if (QFileInfo::exists(base)) {
        QString cleanupError;
        removeTree(base, &cleanupError);
    }
    QDir().mkpath(source + QStringLiteral("\\sub"));

    auto writeFile = [](const QString &path, const QByteArray &data) {
        QFile file(path);
        if (file.open(QIODevice::WriteOnly))
            file.write(data);
    };
    writeFile(source + QStringLiteral("\\marker.txt"), QByteArrayLiteral("hello"));
    writeFile(source + QStringLiteral("\\sub\\nested.txt"), QByteArrayLiteral("world"));

    const QString journal = base + QStringLiteral("\\.journal.json");
    qputenv("DISK_EXPANDER_JOURNAL", journal.toLocal8Bit());

    bool ok = migrateDirectory(
        source, targetRoot,
        [&log](const QString &text) { log += text + QLatin1Char('\n'); },
        &error);

    auto isReparse = [](const QString &path) {
        const DWORD attributes = GetFileAttributesW(
            reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(path).utf16()));
        return attributes != INVALID_FILE_ATTRIBUTES
               && (attributes & FILE_ATTRIBUTE_REPARSE_POINT);
    };

    ok = ok && QFile::exists(source + QStringLiteral("\\marker.txt"))
         && QFile::exists(destination + QStringLiteral("\\marker.txt"))
         && isReparse(source);
    if (!ok && error.isEmpty())
        error = QStringLiteral("迁移后校验失败");

    log += QStringLiteral("迁移阶段：")
           + (ok ? QStringLiteral("通过") : QStringLiteral("失败")) + QLatin1Char('\n');

    if (ok) {
        MigrationLink link;
        link.source = QDir::toNativeSeparators(source);
        link.target = QDir::toNativeSeparators(destination);
        link.bytes = directorySize(destination);

        const bool rolledBack = rollbackLink(
            link,
            [&log](const QString &text) { log += text + QLatin1Char('\n'); },
            &error);

        const bool restored = QFile::exists(source + QStringLiteral("\\marker.txt"))
                              && !isReparse(source)
                              && !QFileInfo::exists(destination);

        ok = rolledBack && restored;
        if (!ok && error.isEmpty())
            error = QStringLiteral("回滚后校验失败");

        log += QStringLiteral("回滚阶段：")
               + (ok ? QStringLiteral("通过") : QStringLiteral("失败")) + QLatin1Char('\n');
    }

    if (QFileInfo::exists(base)) {
        QString cleanupError;
        removeTree(base, &cleanupError);
    }
    qunsetenv("DISK_EXPANDER_JOURNAL");

    *report = log;
    if (!ok)
        *report += QStringLiteral("\n失败原因：") + error;
    return ok;
}
