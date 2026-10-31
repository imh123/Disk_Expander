#ifndef MAINTENANCEDIALOG_H
#define MAINTENANCEDIALOG_H

#include "maintenance.h"

#include <QDialog>
#include <QPointer>

class QCheckBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QThread;

class MaintenanceDialog : public QDialog
{
    Q_OBJECT

public:
    explicit MaintenanceDialog(QWidget *parent = nullptr);
    ~MaintenanceDialog() override;

private slots:
    void start();
    void cancelRun();
    void browseTarget();
    void openTarget();
    void onLog(const QString &text);
    void onStep(int index, int total, const QString &title);
    void onFinished(bool success, const QString &summary);

private:
    void buildUi();

    QCheckBox *m_hibernate = nullptr;
    QCheckBox *m_cleanup = nullptr;
    QCheckBox *m_downloads = nullptr;
    QCheckBox *m_tencent = nullptr;
    QLineEdit *m_target = nullptr;
    QPlainTextEdit *m_log = nullptr;
    QProgressBar *m_progress = nullptr;
    QLabel *m_status = nullptr;
    QPushButton *m_startButton = nullptr;
    QPushButton *m_openButton = nullptr;
    QPushButton *m_closeButton = nullptr;
    QThread *m_thread = nullptr;
    QPointer<MaintenanceWorker> m_worker;
    bool m_running = false;
};

#endif // MAINTENANCEDIALOG_H
