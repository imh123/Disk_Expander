#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include "diskmodel.h"

#include <QMainWindow>

class DiskMapWidget;
class QAction;
class QLabel;
class QPlainTextEdit;
class QProgressBar;
class QTextBrowser;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

    void showDemoData();

public slots:
    void appendLog(const QString &text);

private slots:
    void refresh();
    void exportReport();
    void openAudit();
    void openMaintenance();
    void openRollback();
    void checkPendingMigration();
    void onSelectionChanged(int diskNumber, int partitionNumber);
    void relaunchAsAdmin();
    void showAbout();

private:
    void buildUi();
    void applyScan(const ScanResult &result);
    void updateBanner();
    void updateDetails(int diskNumber, int partitionNumber);
    void updateStatus();
    QString capabilityHtml(const DiskInfo &disk,
                           const PartitionInfo &part,
                           const VolumeInfo *volume) const;

    DiskMapWidget *m_map = nullptr;
    QLabel *m_banner = nullptr;
    QProgressBar *m_usage = nullptr;
    QLabel *m_percent = nullptr;
    QTextBrowser *m_details = nullptr;
    QPlainTextEdit *m_log = nullptr;
    QLabel *m_status = nullptr;
    QAction *m_adminAction = nullptr;

    ScanResult m_result;
    qint64 m_scanMs = 0;
};

#endif // MAINWINDOW_H
