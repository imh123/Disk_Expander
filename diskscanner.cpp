#include "diskscanner.h"

#ifndef NOMINMAX
#  define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winioctl.h>
#include <objbase.h>

#include <QHash>
#include <QJsonArray>
#include <QStringList>

#include <algorithm>
#include <vector>

namespace {

constexpr qint64 kMiB = 1024LL * 1024LL;

QString guidToString(const GUID &guid)
{
    wchar_t buffer[64] = {};
    if (StringFromGUID2(guid, buffer, 64) <= 0)
        return {};
    return QString::fromWCharArray(buffer);
}

QString win32ErrorText(DWORD error)
{
    wchar_t *message = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        error,
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<LPWSTR>(&message),
        0,
        nullptr);
    QString text;
    if (length > 0 && message)
        text = QString::fromWCharArray(message, static_cast<int>(length)).trimmed();
    if (message)
        LocalFree(message);
    return text;
}

QString busTypeName(int type)
{
    switch (type) {
    case 0x01: return QStringLiteral("SCSI");
    case 0x02: return QStringLiteral("ATAPI");
    case 0x03: return QStringLiteral("ATA");
    case 0x04: return QStringLiteral("1394");
    case 0x05: return QStringLiteral("SSA");
    case 0x06: return QStringLiteral("Fibre");
    case 0x07: return QStringLiteral("USB");
    case 0x08: return QStringLiteral("RAID");
    case 0x09: return QStringLiteral("iSCSI");
    case 0x0A: return QStringLiteral("SAS");
    case 0x0B: return QStringLiteral("SATA");
    case 0x0C: return QStringLiteral("SD");
    case 0x0D: return QStringLiteral("MMC");
    case 0x0E: return QStringLiteral("虚拟");
    case 0x0F: return QStringLiteral("虚拟磁盘");
    case 0x10: return QStringLiteral("存储池");
    case 0x11: return QStringLiteral("NVMe");
    case 0x12: return QStringLiteral("SCM");
    case 0x13: return QStringLiteral("UFS");
    default:   return QStringLiteral("未知");
    }
}

PartitionRole roleFromGuid(const QString &guid)
{
    static const QHash<QString, PartitionRole> roles = {
        {QStringLiteral("{C12A7328-F81F-11D2-BA4B-00A0C93EC93B}"), PartitionRole::Efi},
        {QStringLiteral("{E3C9E316-0B5C-4DB8-817D-F92DF00215AE}"), PartitionRole::Msr},
        {QStringLiteral("{DE94BBA4-06D1-4D40-A16A-BFD50179D6AC}"), PartitionRole::Recovery},
        {QStringLiteral("{EBD0A0A2-B9E5-4433-87C0-68B6B72699C7}"), PartitionRole::Basic},
        {QStringLiteral("{5808C8AA-7E8F-42E0-85D2-E1E90434CFB3}"), PartitionRole::DynamicMetadata},
        {QStringLiteral("{AF9B60A0-1431-4F62-BC68-3311714A69AD}"), PartitionRole::DynamicMetadata},
        {QStringLiteral("{E75CAF8F-F680-4CEE-AFA3-B001E56EFC2D}"), PartitionRole::StorageSpaces},
    };
    return roles.value(guid.toUpper(), PartitionRole::Unknown);
}

PartitionRole roleFromMbr(BYTE type)
{
    switch (type) {
    case 0xEF: return PartitionRole::Efi;
    case 0x27: return PartitionRole::Recovery;
    case 0x42: return PartitionRole::DynamicMetadata;
    case 0x06:
    case 0x07:
    case 0x0B:
    case 0x0C:
    case 0x0E: return PartitionRole::Basic;
    default:   return PartitionRole::Unknown;
    }
}

struct DeviceInfo {
    QString model;
    QString bus;
    bool removable = false;
};

DeviceInfo queryDeviceInfo(HANDLE disk)
{
    DeviceInfo info;

    STORAGE_PROPERTY_QUERY query = {};
    query.PropertyId = StorageDeviceProperty;
    query.QueryType = PropertyStandardQuery;

    std::vector<BYTE> buffer(1024);
    DWORD returned = 0;
    if (!DeviceIoControl(disk,
                         IOCTL_STORAGE_QUERY_PROPERTY,
                         &query,
                         sizeof(query),
                         buffer.data(),
                         static_cast<DWORD>(buffer.size()),
                         &returned,
                         nullptr))
        return info;

    const auto *descriptor = reinterpret_cast<const STORAGE_DEVICE_DESCRIPTOR *>(buffer.data());
    info.bus = busTypeName(static_cast<int>(descriptor->BusType));
    info.removable = descriptor->RemovableMedia != FALSE;

    auto readString = [&buffer](DWORD offset) -> QString {
        if (offset == 0 || offset >= buffer.size())
            return {};
        const char *start = reinterpret_cast<const char *>(buffer.data() + offset);
        const size_t maximum = buffer.size() - offset;
        size_t length = 0;
        while (length < maximum && start[length] != '\0')
            ++length;
        return QString::fromLatin1(start, static_cast<int>(length)).trimmed();
    };

    const QString vendor = readString(descriptor->VendorIdOffset);
    const QString product = readString(descriptor->ProductIdOffset);
    info.model = (vendor + QLatin1Char(' ') + product).trimmed();
    return info;
}

// 卷可能由多个 extent 组成，缓冲区必须按实际数量动态放大。
QVector<DiskExtent> queryExtents(HANDLE volume)
{
    QVector<DiskExtent> extents;
    std::vector<BYTE> buffer(sizeof(VOLUME_DISK_EXTENTS) + sizeof(DISK_EXTENT) * 15);

    for (int attempt = 0; attempt < 5; ++attempt) {
        DWORD returned = 0;
        const BOOL ok = DeviceIoControl(volume,
                                        IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS,
                                        nullptr,
                                        0,
                                        buffer.data(),
                                        static_cast<DWORD>(buffer.size()),
                                        &returned,
                                        nullptr);
        if (ok) {
            const auto *data = reinterpret_cast<const VOLUME_DISK_EXTENTS *>(buffer.data());
            for (DWORD i = 0; i < data->NumberOfDiskExtents; ++i) {
                DiskExtent extent;
                extent.diskNumber = static_cast<int>(data->Extents[i].DiskNumber);
                extent.offset = static_cast<qint64>(data->Extents[i].StartingOffset.QuadPart);
                extent.length = static_cast<qint64>(data->Extents[i].ExtentLength.QuadPart);
                extents.append(extent);
            }
            break;
        }

        const DWORD error = GetLastError();
        if (error != ERROR_MORE_DATA && error != ERROR_INSUFFICIENT_BUFFER)
            break;
        buffer.resize(buffer.size() * 2);
    }
    return extents;
}

bool readLayout(HANDLE disk, std::vector<BYTE> &buffer, DRIVE_LAYOUT_INFORMATION_EX **layout)
{
    DWORD size = 4096;
    for (int attempt = 0; attempt < 6; ++attempt) {
        buffer.resize(size);
        DWORD returned = 0;
        if (DeviceIoControl(disk,
                            IOCTL_DISK_GET_DRIVE_LAYOUT_EX,
                            nullptr,
                            0,
                            buffer.data(),
                            size,
                            &returned,
                            nullptr)) {
            *layout = reinterpret_cast<DRIVE_LAYOUT_INFORMATION_EX *>(buffer.data());
            return true;
        }
        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER)
            return false;
        size *= 2;
    }
    return false;
}

qint64 alignUp(qint64 value, qint64 alignment)
{
    return ((value + alignment - 1) / alignment) * alignment;
}

qint64 alignDown(qint64 value, qint64 alignment)
{
    return (value / alignment) * alignment;
}

void addUnallocated(DiskInfo &disk, qint64 offset, qint64 length)
{
    if (length >= kMiB)
        disk.unallocated.append(UnallocatedRegion{offset, length});
}

void parseLayout(DiskInfo &disk, const DRIVE_LAYOUT_INFORMATION_EX *layout)
{
    if (layout->PartitionStyle == PARTITION_STYLE_GPT)
        disk.style = DiskStyle::Gpt;
    else if (layout->PartitionStyle == PARTITION_STYLE_MBR)
        disk.style = DiskStyle::Mbr;

    for (DWORD i = 0; i < layout->PartitionCount; ++i) {
        const PARTITION_INFORMATION_EX &source = layout->PartitionEntry[i];
        if (source.PartitionLength.QuadPart <= 0)
            continue;

        PartitionInfo part;
        part.diskNumber = disk.number;
        part.partitionNumber = static_cast<int>(source.PartitionNumber);
        part.offset = static_cast<qint64>(source.StartingOffset.QuadPart);
        part.length = static_cast<qint64>(source.PartitionLength.QuadPart);
        part.name = QStringLiteral("分区 %1").arg(part.partitionNumber);

        if (source.PartitionStyle == PARTITION_STYLE_GPT) {
            part.typeGuid = guidToString(source.Gpt.PartitionType);
            part.role = roleFromGuid(part.typeGuid);

            const size_t maximum = sizeof(source.Gpt.Name) / sizeof(source.Gpt.Name[0]);
            size_t length = 0;
            while (length < maximum && source.Gpt.Name[length] != L'\0')
                ++length;
            if (length > 0)
                part.name = QString::fromWCharArray(source.Gpt.Name, static_cast<int>(length));
        } else {
            const BYTE type = source.Mbr.PartitionType;
            if (type == 0x00 || type == 0xEE)  // 空项 / 保护性 MBR
                continue;
            part.typeGuid = QStringLiteral("0x%1")
                                .arg(type, 2, 16, QLatin1Char('0'))
                                .toUpper();
            part.role = roleFromMbr(type);
            part.isSystem = source.Mbr.BootIndicator != FALSE;
            part.name = roleName(part.role);
        }

        if (part.role == PartitionRole::Efi)
            part.isSystem = true;
        if (part.role == PartitionRole::DynamicMetadata)
            disk.isDynamic = true;

        disk.partitions.append(part);
    }

    std::sort(disk.partitions.begin(), disk.partitions.end(),
              [](const PartitionInfo &a, const PartitionInfo &b) {
                  return a.offset < b.offset;
              });

    // 计算“真正可用”的范围：GPT 头部/备份区不能算作未分配空间。
    qint64 usableStart = kMiB;
    qint64 usableEnd = disk.size;

    if (disk.style == DiskStyle::Gpt) {
        const qint64 sector = disk.sectorSize > 0 ? disk.sectorSize : 512;
        const qint64 entryBytes = 128LL * 128LL;  // 128 项 × 128 字节
        const qint64 reservedSectors = 2 + (entryBytes + sector - 1) / sector;
        usableStart = alignUp(reservedSectors * sector, kMiB);
        usableEnd = alignDown(disk.size - reservedSectors * sector, kMiB);
    }

    qint64 cursor = usableStart;
    for (const auto &part : disk.partitions) {
        if (part.offset > cursor)
            addUnallocated(disk, cursor, part.offset - cursor);
        cursor = std::max(cursor, part.offset + part.length);
    }
    if (usableEnd > cursor)
        addUnallocated(disk, cursor, usableEnd - cursor);
}

QVector<VolumeInfo> scanVolumes()
{
    QVector<VolumeInfo> volumes;

    wchar_t name[MAX_PATH] = {};
    HANDLE find = FindFirstVolumeW(name, MAX_PATH);
    if (find == INVALID_HANDLE_VALUE)
        return volumes;

    do {
        VolumeInfo volume;
        volume.guidPath = QString::fromWCharArray(name);
        volume.guid = volume.guidPath;
        if (volume.guid.startsWith(QStringLiteral("\\\\?\\")))
            volume.guid = volume.guid.mid(4);
        if (volume.guid.endsWith(QLatin1Char('\\')))
            volume.guid.chop(1);

        wchar_t paths[4096] = {};
        DWORD needed = 0;
        if (GetVolumePathNamesForVolumeNameW(name, paths, 4096, &needed)) {
            for (const wchar_t *path = paths; *path != L'\0'; path += wcslen(path) + 1) {
                const QString mountPoint = QString::fromWCharArray(path);
                volume.mountPoints.append(mountPoint);
                if (volume.driveLetter.isEmpty()
                    && mountPoint.size() >= 2
                    && mountPoint.at(1) == QLatin1Char(':'))
                    volume.driveLetter = mountPoint.left(2);
            }
        }

        wchar_t label[256] = {};
        wchar_t fileSystem[64] = {};
        if (GetVolumeInformationW(name, label, 256, nullptr, nullptr, nullptr, fileSystem, 64)) {
            volume.label = QString::fromWCharArray(label);
            volume.fileSystem = QString::fromWCharArray(fileSystem);
        }

        ULARGE_INTEGER freeBytes = {};
        ULARGE_INTEGER totalBytes = {};
        ULARGE_INTEGER totalFree = {};
        if (GetDiskFreeSpaceExW(name, &freeBytes, &totalBytes, &totalFree)) {
            volume.size = static_cast<qint64>(totalBytes.QuadPart);
            volume.freeBytes = static_cast<qint64>(freeBytes.QuadPart);
        }

        auto tryExtents = [](const QString &path) -> QVector<DiskExtent> {
            HANDLE handle = CreateFileW(reinterpret_cast<LPCWSTR>(path.utf16()),
                                        GENERIC_READ,
                                        FILE_SHARE_READ | FILE_SHARE_WRITE,
                                        nullptr,
                                        OPEN_EXISTING,
                                        0,
                                        nullptr);
            if (handle == INVALID_HANDLE_VALUE)
                return {};
            const QVector<DiskExtent> extents = queryExtents(handle);
            CloseHandle(handle);
            return extents;
        };

        QString guidWithoutSlash = volume.guidPath;
        if (guidWithoutSlash.endsWith(QLatin1Char('\\')))
            guidWithoutSlash.chop(1);

        QStringList candidates;
        if (!volume.driveLetter.isEmpty())
            candidates << QStringLiteral("\\\\.\\") + volume.driveLetter;
        candidates << guidWithoutSlash << volume.guidPath;

        for (const QString &candidate : candidates) {
            const QVector<DiskExtent> extents = tryExtents(candidate);
            if (!extents.isEmpty()) {
                volume.extents = extents;
                break;
            }
        }

        volumes.append(volume);
    } while (FindNextVolumeW(find, name, MAX_PATH));

    FindVolumeClose(find);
    return volumes;
}

QVector<DiskInfo> scanDisks(QString *error)
{
    QVector<DiskInfo> disks;
    int consecutiveMissing = 0;

    for (int index = 0; index < 128; ++index) {
        const QString path = QStringLiteral("\\\\.\\PhysicalDrive%1").arg(index);
        HANDLE handle = CreateFileW(reinterpret_cast<LPCWSTR>(path.utf16()),
                                    GENERIC_READ,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE,
                                    nullptr,
                                    OPEN_EXISTING,
                                    0,
                                    nullptr);
        if (handle == INVALID_HANDLE_VALUE) {
            const DWORD code = GetLastError();
            if (code == ERROR_ACCESS_DENIED) {
                if (error && error->isEmpty())
                    *error = QStringLiteral("读取 PhysicalDrive%1 被拒绝，请以管理员身份运行。")
                                 .arg(index);
                break;
            }
            if (++consecutiveMissing >= 4)
                break;
            continue;
        }
        consecutiveMissing = 0;

        DiskInfo disk;
        disk.number = index;

        GET_LENGTH_INFORMATION length = {};
        DWORD returned = 0;
        if (DeviceIoControl(handle,
                            IOCTL_DISK_GET_LENGTH_INFO,
                            nullptr,
                            0,
                            &length,
                            sizeof(length),
                            &returned,
                            nullptr))
            disk.size = static_cast<qint64>(length.Length.QuadPart);

        std::vector<BYTE> geometry(1024);
        if (DeviceIoControl(handle,
                            IOCTL_DISK_GET_DRIVE_GEOMETRY_EX,
                            nullptr,
                            0,
                            geometry.data(),
                            static_cast<DWORD>(geometry.size()),
                            &returned,
                            nullptr)) {
            const auto *info = reinterpret_cast<const DISK_GEOMETRY_EX *>(geometry.data());
            disk.sectorSize = info->Geometry.BytesPerSector;
            if (disk.size <= 0)
                disk.size = static_cast<qint64>(info->DiskSize.QuadPart);
        }

        const DeviceInfo device = queryDeviceInfo(handle);
        disk.model = device.model.isEmpty()
                         ? QStringLiteral("物理磁盘 %1").arg(index)
                         : device.model;
        disk.busType = device.bus;
        disk.removable = device.removable;

        std::vector<BYTE> layoutBuffer;
        DRIVE_LAYOUT_INFORMATION_EX *layout = nullptr;
        if (readLayout(handle, layoutBuffer, &layout)) {
            parseLayout(disk, layout);
        } else if (error && error->isEmpty()) {
            *error = QStringLiteral("读取磁盘 %1 的分区表失败：%2")
                         .arg(index)
                         .arg(win32ErrorText(GetLastError()));
        }

        disks.append(disk);
        CloseHandle(handle);
    }

    return disks;
}

void matchVolumes(ScanResult &result)
{
    for (auto &disk : result.disks) {
        for (auto &part : disk.partitions) {
            for (const auto &volume : result.volumes) {
                bool matched = false;
                for (const auto &extent : volume.extents) {
                    // 先精确匹配，再退化为“extent 落在分区内”，兼容 BitLocker/动态卷。
                    const bool exact = extent.diskNumber == disk.number
                                       && extent.offset == part.offset
                                       && extent.length == part.length;
                    const bool contained = extent.diskNumber == disk.number
                                           && extent.offset >= part.offset
                                           && extent.offset + extent.length
                                                  <= part.offset + part.length;
                    if (exact || contained) {
                        matched = true;
                        break;
                    }
                }
                if (matched) {
                    part.volumeGuid = volume.guid;
                    break;
                }
            }
        }
    }
}

void markBootVolumes(ScanResult &result)
{
    QString windowsRoot = QString::fromLocal8Bit(qgetenv("SystemRoot"));
    if (windowsRoot.size() < 2)
        windowsRoot = QStringLiteral("C:\\Windows");

    const QString rootPath = windowsRoot.left(2) + QStringLiteral("\\");

    wchar_t guidBuffer[64] = {};
    QString bootGuid;
    if (GetVolumeNameForVolumeMountPointW(
            reinterpret_cast<LPCWSTR>(rootPath.utf16()), guidBuffer, 64)) {
        bootGuid = QString::fromWCharArray(guidBuffer);
        if (bootGuid.startsWith(QStringLiteral("\\\\?\\")))
            bootGuid = bootGuid.mid(4);
        if (bootGuid.endsWith(QLatin1Char('\\')))
            bootGuid.chop(1);
    }

    for (auto &volume : result.volumes) {
        if (!bootGuid.isEmpty()
            && volume.guid.compare(bootGuid, Qt::CaseInsensitive) == 0)
            volume.isBootVolume = true;
    }

    for (auto &disk : result.disks) {
        for (auto &part : disk.partitions) {
            if (!bootGuid.isEmpty()
                && part.volumeGuid.compare(bootGuid, Qt::CaseInsensitive) == 0)
                part.isBoot = true;
            if (part.isBoot || part.isSystem)
                disk.isSystemDisk = true;
        }
    }

    for (auto &volume : result.volumes) {
        for (const auto &disk : result.disks) {
            for (const auto &part : disk.partitions) {
                if (part.isSystem
                    && part.volumeGuid.compare(volume.guid, Qt::CaseInsensitive) == 0)
                    volume.isSystemVolume = true;
            }
        }
    }
}

}  // namespace

bool DiskScanner::isElevated()
{
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
        return false;

    TOKEN_ELEVATION elevation = {};
    DWORD size = sizeof(elevation);
    const BOOL ok = GetTokenInformation(token, TokenElevation, &elevation, size, &size);
    CloseHandle(token);
    return ok && elevation.TokenIsElevated != 0;
}

ScanResult DiskScanner::scan()
{
    ScanResult result;
    result.elevated = isElevated();
    if (!result.elevated) {
        result.error = QStringLiteral(
            "需要管理员权限才能读取磁盘布局。请右键以管理员身份运行，"
            "或从 UAC 提示中允许提权。");
        return result;
    }

    result.volumes = scanVolumes();

    QString diskError;
    result.disks = scanDisks(&diskError);

    if (result.disks.isEmpty()) {
        result.error = diskError.isEmpty()
                           ? QStringLiteral("没有发现可读取的物理磁盘。")
                           : diskError;
        return result;
    }

    matchVolumes(result);
    markBootVolumes(result);
    return result;
}

QString DiskScanner::formatBytes(qint64 bytes)
{
    if (bytes < 0)
        bytes = 0;

    const double gb = static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0);
    if (gb >= 1024.0)
        return QStringLiteral("%1 TB").arg(gb / 1024.0, 0, 'f', 2);
    if (gb >= 1.0)
        return QStringLiteral("%1 GB").arg(gb, 0, 'f', 1);

    const double mb = static_cast<double>(bytes) / (1024.0 * 1024.0);
    if (mb >= 1.0)
        return QStringLiteral("%1 MB").arg(mb, 0, 'f', 0);

    return QStringLiteral("%1 KB").arg(static_cast<double>(bytes) / 1024.0, 0, 'f', 0);
}

QJsonObject DiskScanner::toJson(const ScanResult &result)
{
    QJsonObject root;
    root.insert(QStringLiteral("error"), result.error);
    root.insert(QStringLiteral("elevated"), result.elevated);

    QJsonArray disks;
    for (const auto &disk : result.disks) {
        QJsonObject object;
        object.insert(QStringLiteral("number"), disk.number);
        object.insert(QStringLiteral("model"), disk.model);
        object.insert(QStringLiteral("bus"), disk.busType);
        object.insert(QStringLiteral("size"), static_cast<double>(disk.size));
        object.insert(QStringLiteral("sectorSize"), disk.sectorSize);
        object.insert(QStringLiteral("style"), styleName(disk.style));
        object.insert(QStringLiteral("removable"), disk.removable);
        object.insert(QStringLiteral("systemDisk"), disk.isSystemDisk);
        object.insert(QStringLiteral("dynamic"), disk.isDynamic);

        QJsonArray partitions;
        for (const auto &part : disk.partitions) {
            QJsonObject entry;
            entry.insert(QStringLiteral("number"), part.partitionNumber);
            entry.insert(QStringLiteral("offset"), static_cast<double>(part.offset));
            entry.insert(QStringLiteral("length"), static_cast<double>(part.length));
            entry.insert(QStringLiteral("role"), roleName(part.role));
            entry.insert(QStringLiteral("typeGuid"), part.typeGuid);
            entry.insert(QStringLiteral("name"), part.name);
            entry.insert(QStringLiteral("volumeGuid"), part.volumeGuid);
            entry.insert(QStringLiteral("boot"), part.isBoot);
            entry.insert(QStringLiteral("system"), part.isSystem);
            partitions.append(entry);
        }
        object.insert(QStringLiteral("partitions"), partitions);

        QJsonArray unallocated;
        for (const auto &region : disk.unallocated) {
            QJsonObject entry;
            entry.insert(QStringLiteral("offset"), static_cast<double>(region.offset));
            entry.insert(QStringLiteral("length"), static_cast<double>(region.length));
            unallocated.append(entry);
        }
        object.insert(QStringLiteral("unallocated"), unallocated);

        disks.append(object);
    }
    root.insert(QStringLiteral("disks"), disks);

    QJsonArray volumes;
    for (const auto &volume : result.volumes) {
        QJsonObject object;
        object.insert(QStringLiteral("guid"), volume.guid);
        object.insert(QStringLiteral("label"), volume.label);
        object.insert(QStringLiteral("fileSystem"), volume.fileSystem);
        object.insert(QStringLiteral("size"), static_cast<double>(volume.size));
        object.insert(QStringLiteral("free"), static_cast<double>(volume.freeBytes));
        object.insert(QStringLiteral("driveLetter"), volume.driveLetter);
        object.insert(QStringLiteral("mountPoints"),
                      QJsonArray::fromStringList(volume.mountPoints));
        object.insert(QStringLiteral("boot"), volume.isBootVolume);
        volumes.append(object);
    }
    root.insert(QStringLiteral("volumes"), volumes);

    return root;
}
