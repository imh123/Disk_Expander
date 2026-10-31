#ifndef ROLLBACK_H
#define ROLLBACK_H

#include "maintenance.h"

#include <QObject>
#include <QString>
#include <QVector>

#include <atomic>
#include <functional>

struct MigrationLink {
    QString source;   // C: 上的 junction 路径
    QString target;   // D: 上的实际目录
    qint64 bytes = 0;
};

// 扫描 scanRoot 下所有指向 migrationRoot 的 junction。
QVector<MigrationLink> findMigrationLinks(const QString &scanRoot,
                                          const QString &migrationRoot);

// 删除 junction，把数据从 target 移回 source，并校验；失败时尝试恢复 junction。
bool rollbackLink(const MigrationLink &link,
                  const std::function<void(const QString &)> &log,
                  QString *error);

// 断点恢复：完成或撤销 journal 中记录的未完成迁移。
bool completeMigration(const MigrationJournal &entry,
                       const std::function<void(const QString &)> &log,
                       QString *error);
bool undoMigration(const MigrationJournal &entry,
                   const std::function<void(const QString &)> &log,
                   QString *error);

class RollbackWorker : public QObject
{
    Q_OBJECT

public:
    explicit RollbackWorker(QVector<MigrationLink> links, QObject *parent = nullptr);

    void requestCancel();

public slots:
    void run();

signals:
    void logMessage(const QString &text);
    void progress(int index, int total);
    void finished(int succeeded, int failed, const QString &summary);

private:
    QVector<MigrationLink> m_links;
    std::atomic_bool m_cancel{false};
};

#endif // ROLLBACK_H
