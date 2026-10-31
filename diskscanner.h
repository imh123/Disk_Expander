#ifndef DISKSCANNER_H
#define DISKSCANNER_H

#include "diskmodel.h"

#include <QJsonObject>
#include <QString>

class DiskScanner
{
public:
    DiskScanner() = delete;

    static ScanResult scan();
    static bool isElevated();

    static QString formatBytes(qint64 bytes);
    static QJsonObject toJson(const ScanResult &result);
};

#endif // DISKSCANNER_H
