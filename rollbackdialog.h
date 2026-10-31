#ifndef ROLLBACKDIALOG_H
#define ROLLBACKDIALOG_H

#include "rollback.h"

#include <QDialog>
#include <QPointer>

class QLabel;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QThread;
class QTreeWidget;

class RollbackDialog : public QDialog
{
    Q_OBJECT

public:
    explicit RollbackDialog(QWidget *parent = nullptr);
    ~RollbackDialog() override;

private slots:
    void rescan();
    void rollbackSelected();
    void onLog(const QString &text);
    void onProgress(int index, int total);
    void onFinished(int succeeded, int failed, const QString &summary);

private:
    void buildUi();
    void populate();
    QVector<MigrationLink> checkedLinks() const;

    QVector<MigrationLink> m_links;
    QTreeWidget *m_tree = nullptr;
    QPlainTextEdit *m_log = nullptr;
    QProgressBar *m_progress = nullptr;
    QLabel *m_status = nullptr;
    QPushButton *m_rescanButton = nullptr;
    QPushButton *m_rollbackButton = nullptr;
    QPushButton *m_closeButton = nullptr;
    QThread *m_thread = nullptr;
    QPointer<RollbackWorker> m_worker;
    bool m_running = false;
};

#endif // ROLLBACKDIALOG_H
