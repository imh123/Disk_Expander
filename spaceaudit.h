#ifndef SPACEAUDIT_H
#define SPACEAUDIT_H

#include <QJsonObject>
#include <QMetaType>
#include <QObject>
#include <QString>
#include <QVector>

#include <atomic>

enum class AuditKind {
    Cleanable,  // 可以直接清理
    Movable,    // 可以迁移到其他盘
    Caution     // 需要谨慎处理
};

struct AuditItem {
    QString category;
    QString name;
    QString path;
    qint64 bytes = 0;
    QString advice;
    AuditKind kind = AuditKind::Caution;
};

struct AuditResult {
    QVector<AuditItem> items;
    qint64 scannedBytes = 0;
    qint64 cleanableBytes = 0;
    qint64 movableBytes = 0;
    qint64 cautionBytes = 0;
    int files = 0;
    bool cancelled = false;
    QString error;
};

Q_DECLARE_METATYPE(AuditResult)

QJsonObject auditToJson(const AuditResult &result);

class SpaceAuditWorker : public QObject
{
    Q_OBJECT

public:
    explicit SpaceAuditWorker(QString root, QObject *parent = nullptr);

    void requestCancel();

public slots:
    void process();

signals:
    void progress(const QString &currentPath, qint64 bytes, int files);
    void finished(const AuditResult &result);

private:
    QString m_root;
    std::atomic_bool m_cancel{false};
};

#endif // SPACEAUDIT_H
