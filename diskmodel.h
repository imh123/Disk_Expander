#ifndef DISKMODEL_H
#define DISKMODEL_H

#include <QString>
#include <QStringList>
#include <QVector>
#include <QtGlobal>

// 分区角色：既决定 UI 配色，也决定“这个分区能不能动”。
enum class PartitionRole {
    Efi,
    Msr,
    Recovery,
    Basic,
    DynamicMetadata,
    StorageSpaces,
    Unknown
};

enum class DiskStyle {
    Unknown,
    Mbr,
    Gpt
};

struct DiskExtent {
    int diskNumber = -1;
    qint64 offset = 0;
    qint64 length = 0;
};

// 一个卷（可能由多个 extent 组成，对应多个分区）。
struct VolumeInfo {
    QString guid;        // {xxxxxxxx-xxxx-...}
    QString guidPath;    // 形如 \\?\Volume{GUID}\（末尾带斜杠）
    QString label;
    QString fileSystem;
    qint64 size = 0;
    qint64 freeBytes = 0;
    QStringList mountPoints;
    QString driveLetter;         // "C:"，没有盘符时为空
    bool isBootVolume = false;   // 承载 %SystemRoot%
    bool isSystemVolume = false; // ESP / 活动分区
    QVector<DiskExtent> extents;
};

struct PartitionInfo {
    int diskNumber = -1;
    int partitionNumber = 0;
    qint64 offset = 0;
    qint64 length = 0;
    PartitionRole role = PartitionRole::Unknown;
    QString typeGuid;
    QString name;
    QString volumeGuid;  // 关联 VolumeInfo::guid，空表示无卷
    bool isBoot = false;
    bool isSystem = false;
};

struct UnallocatedRegion {
    qint64 offset = 0;
    qint64 length = 0;
};

struct DiskInfo {
    int number = -1;
    QString model;
    QString busType;
    qint64 size = 0;
    int sectorSize = 512;
    DiskStyle style = DiskStyle::Unknown;
    bool removable = false;
    bool isSystemDisk = false;
    bool isDynamic = false;
    QVector<PartitionInfo> partitions;
    QVector<UnallocatedRegion> unallocated;

    qint64 allocatedBytes() const
    {
        qint64 total = 0;
        for (const auto &p : partitions)
            total += p.length;
        return total;
    }

    qint64 unallocatedBytes() const
    {
        qint64 total = 0;
        for (const auto &r : unallocated)
            total += r.length;
        return total;
    }
};

struct ScanResult {
    QVector<DiskInfo> disks;
    QVector<VolumeInfo> volumes;
    QString error;
    bool elevated = false;
};

inline QString roleName(PartitionRole role)
{
    switch (role) {
    case PartitionRole::Efi:             return QStringLiteral("EFI 系统分区");
    case PartitionRole::Msr:             return QStringLiteral("MSR 保留分区");
    case PartitionRole::Recovery:        return QStringLiteral("恢复分区");
    case PartitionRole::Basic:           return QStringLiteral("基本数据分区");
    case PartitionRole::DynamicMetadata: return QStringLiteral("动态磁盘元数据");
    case PartitionRole::StorageSpaces:   return QStringLiteral("存储池成员");
    default:                             return QStringLiteral("未知分区");
    }
}

inline QString roleShortName(PartitionRole role)
{
    switch (role) {
    case PartitionRole::Efi:             return QStringLiteral("EFI");
    case PartitionRole::Msr:             return QStringLiteral("MSR");
    case PartitionRole::Recovery:        return QStringLiteral("恢复");
    case PartitionRole::Basic:           return QStringLiteral("数据");
    case PartitionRole::DynamicMetadata: return QStringLiteral("动态");
    case PartitionRole::StorageSpaces:   return QStringLiteral("存储池");
    default:                             return QStringLiteral("未知");
    }
}

inline QString styleName(DiskStyle style)
{
    switch (style) {
    case DiskStyle::Gpt: return QStringLiteral("GPT");
    case DiskStyle::Mbr: return QStringLiteral("MBR");
    default:             return QStringLiteral("未知");
    }
}

#endif // DISKMODEL_H
