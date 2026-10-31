#ifndef MAINTENANCE_H
#define MAINTENANCE_H

#include <QObject>
#include <QString>

#include <atomic>
#include <functional>

struct MaintenanceOptions {
    bool disableHibernate = true;
    bool cleanupTemp = true;
    bool migrateDownloads = true;
    bool migrateTencent = true;
    QString targetRoot;
};

struct MigrationJournal {
    QString source;
    QString target;
    QString step;       // copying / copied / renamed / linked
    QString startedAt;
    qint64 bytes = 0;
};

bool readMigrationJournal(MigrationJournal *entry);
void clearMigrationJournal();

// 仅用于开发/自检：在临时目录跑一遍迁移+回滚，不接触真实数据。
bool runSelfTest(QString *report);

// 独立可测试：把 source 复制到 targetRoot/<name>，校验后建立 junction。
bool migrateDirectory(const QString &source,
                      const QString &targetRoot,
                      const std::function<void(const QString &)> &log,
                      QString *error);

class MaintenanceWorker : public QObject
{
    Q_OBJECT

public:
    explicit MaintenanceWorker(MaintenanceOptions options, QObject *parent = nullptr);

    void requestCancel();

public slots:
    void run();

signals:
    void logMessage(const QString &text);
    void stepStarted(int index, int total, const QString &title);
    void finished(bool success, const QString &summary);

private:
    bool disableHibernate(QString *error);
    bool cleanup(QString *error);

    MaintenanceOptions m_options;
    std::atomic_bool m_cancel{false};
};

#endif // MAINTENANCE_H
