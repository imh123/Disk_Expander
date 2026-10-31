#ifndef AUDITDIALOG_H
#define AUDITDIALOG_H

#include "spaceaudit.h"

#include <QDialog>
#include <QPointer>

class QLabel;
class QProgressBar;
class QPushButton;
class QThread;
class QTreeWidget;

class AuditDialog : public QDialog
{
    Q_OBJECT

public:
    explicit AuditDialog(const QString &root, QWidget *parent = nullptr);
    ~AuditDialog() override;

private slots:
    void onProgress(const QString &path, qint64 bytes, int files);
    void onFinished(const AuditResult &result);
    void cancelScan();
    void openSelected();
    void copySelectedPath();

private:
    void buildUi();
    void populate(const AuditResult &result);
    QString selectedPath() const;

    QString m_root;
    QLabel *m_status = nullptr;
    QLabel *m_current = nullptr;
    QLabel *m_scannedValue = nullptr;
    QLabel *m_cleanableValue = nullptr;
    QLabel *m_movableValue = nullptr;
    QLabel *m_cautionValue = nullptr;
    QProgressBar *m_progress = nullptr;
    QTreeWidget *m_tree = nullptr;
    QPushButton *m_cancelButton = nullptr;
    QPushButton *m_openButton = nullptr;
    QPushButton *m_copyButton = nullptr;
    QThread *m_thread = nullptr;
    QPointer<SpaceAuditWorker> m_worker;
    bool m_finished = false;
};

#endif // AUDITDIALOG_H
