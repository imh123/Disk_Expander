#include "diskscanner.h"
#include "maintenance.h"
#include "mainwindow.h"
#include "spaceaudit.h"

#include <QApplication>
#include <QFile>
#include <QJsonDocument>
#include <QLocale>
#include <QMessageBox>
#include <QPixmap>
#include <QTranslator>

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QApplication::setOrganizationName(QStringLiteral("DiskExpander"));
    QApplication::setApplicationName(QStringLiteral("Disk Expander"));
    QApplication::setApplicationVersion(QStringLiteral("0.3.0"));

    QTranslator translator;
    const QStringList uiLanguages = QLocale::system().uiLanguages();
    for (const QString &locale : uiLanguages) {
        const QString baseName = QStringLiteral("Disk_Expander_") + QLocale(locale).name();
        if (translator.load(QStringLiteral(":/i18n/") + baseName)) {
            app.installTranslator(&translator);
            break;
        }
    }

    // 无界面自检：扫描后写出 JSON，便于验证与问题反馈。
    const QStringList args = app.arguments();
    const int jsonIndex = args.indexOf(QStringLiteral("--scan-json"));
    if (jsonIndex >= 0 && jsonIndex + 1 < args.size()) {
        const ScanResult result = DiskScanner::scan();
        QFile file(args.at(jsonIndex + 1));
        if (file.open(QIODevice::WriteOnly | QIODevice::Text)) {
            file.write(QJsonDocument(DiskScanner::toJson(result))
                           .toJson(QJsonDocument::Indented));
        }
        return result.error.isEmpty() ? 0 : 1;
    }

    // 无界面空间体检，便于导出清单。
    const int auditIndex = args.indexOf(QStringLiteral("--audit-json"));
    if (auditIndex >= 0 && auditIndex + 1 < args.size()) {
        QString root = QString::fromLocal8Bit(qgetenv("SystemDrive"));
        if (root.isEmpty())
            root = QStringLiteral("C:");
        root += QStringLiteral("\\");

        SpaceAuditWorker worker(root);
        AuditResult result;
        QObject::connect(&worker, &SpaceAuditWorker::finished,
                         [&result](const AuditResult &value) { result = value; });
        worker.process();

        QFile file(args.at(auditIndex + 1));
        if (file.open(QIODevice::WriteOnly | QIODevice::Text)) {
            file.write(QJsonDocument(auditToJson(result))
                           .toJson(QJsonDocument::Indented));
        }
        return 0;
    }

    // 自测：临时目录里跑一遍迁移 + 回滚。
    if (args.contains(QStringLiteral("--self-test"))) {
        QString report;
        const bool ok = runSelfTest(&report);
        QMessageBox::information(nullptr,
                                 ok ? QStringLiteral("自测通过")
                                    : QStringLiteral("自测失败"),
                                 report);
        return ok ? 0 : 1;
    }

    // 离屏截图，用于布局自检。
    const int shotIndex = args.indexOf(QStringLiteral("--screenshot"));
    if (shotIndex >= 0 && shotIndex + 1 < args.size()) {
        MainWindow window;
        if (args.contains(QStringLiteral("--demo")))
            window.showDemoData();
        window.resize(1220, 800);
        window.show();
        QApplication::processEvents();
        QApplication::processEvents();
        window.grab().save(args.at(shotIndex + 1));
        return 0;
    }

    MainWindow window;
    window.show();
    return app.exec();
}
