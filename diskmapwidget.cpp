#include "diskmapwidget.h"
#include "diskscanner.h"

#include <QFontMetrics>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QToolTip>

namespace {

constexpr int kLeft = 244;
constexpr int kTop = 48;
constexpr int kRowHeight = 76;
constexpr int kRowGap = 8;
constexpr int kBarHeight = 38;
constexpr int kRight = 20;

QColor colorForRole(PartitionRole role)
{
    switch (role) {
    case PartitionRole::Efi:             return QColor(0x64, 0x74, 0x8B);
    case PartitionRole::Msr:             return QColor(0xA8, 0xB0, 0xBC);
    case PartitionRole::Recovery:        return QColor(0xC2, 0x76, 0x2A);
    case PartitionRole::Basic:           return QColor(0x3B, 0x82, 0xF6);
    case PartitionRole::DynamicMetadata: return QColor(0x8B, 0x5C, 0xF6);
    case PartitionRole::StorageSpaces:   return QColor(0x0E, 0xA5, 0xE9);
    default:                             return QColor(0x9C, 0xA3, 0xAF);
    }
}

QColor textColorFor(const QColor &background)
{
    const int luma = (background.red() * 299 + background.green() * 587
                      + background.blue() * 114)
                     / 1000;
    return luma > 162 ? QColor(0x11, 0x18, 0x27) : QColor(0xFF, 0xFF, 0xFF);
}

QString shortSize(qint64 bytes)
{
    const double gb = static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0);
    if (gb >= 1000.0)
        return QStringLiteral("%1T").arg(gb / 1024.0, 0, 'f', 2);
    if (gb >= 1.0)
        return QStringLiteral("%1G").arg(gb, 0, 'f', gb >= 100.0 ? 0 : 1);
    return QStringLiteral("%1M").arg(static_cast<double>(bytes) / (1024.0 * 1024.0), 0, 'f', 0);
}

}  // namespace

DiskMapWidget::DiskMapWidget(QWidget *parent)
    : QWidget(parent)
{
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);
    setMinimumWidth(640);
}

void DiskMapWidget::setData(const QVector<DiskInfo> &disks, const QVector<VolumeInfo> &volumes)
{
    m_disks = disks;
    m_volumes = volumes;
    m_hoverDisk = -1;
    m_hoverPartition = -1;
    m_selectedDisk = -1;
    m_selectedPartition = -1;
    updateGeometry();
    update();
}

void DiskMapWidget::clearSelection()
{
    m_selectedDisk = -1;
    m_selectedPartition = -1;
    update();
}

QSize DiskMapWidget::sizeHint() const
{
    const int rows = qMax(1, m_disks.size());
    return QSize(1040, kTop + rows * (kRowHeight + kRowGap) + 14);
}

QSize DiskMapWidget::minimumSizeHint() const
{
    return QSize(640, kTop + kRowHeight + 14);
}

QRect DiskMapWidget::rowRect(int index) const
{
    return QRect(0, kTop + index * (kRowHeight + kRowGap), width(), kRowHeight);
}

QRect DiskMapWidget::barRect(int index) const
{
    const QRect row = rowRect(index);
    return QRect(kLeft,
                 row.top() + (kRowHeight - kBarHeight) / 2,
                 qMax(80, width() - kLeft - kRight),
                 kBarHeight);
}

QRectF DiskMapWidget::segmentRect(const DiskInfo &disk, const QRect &bar,
                                  qint64 offset, qint64 length) const
{
    if (disk.size <= 0)
        return {};

    const qreal scale = static_cast<qreal>(bar.width()) / static_cast<qreal>(disk.size);
    const qreal left = bar.left() + static_cast<qreal>(offset) * scale;
    qreal width = static_cast<qreal>(length) * scale;
    if (width < 2.0)
        width = 2.0;
    if (left >= bar.right())
        return {};
    width = qMin(width, bar.right() + 1 - left);
    return QRectF(left, bar.top(), width, bar.height());
}

const VolumeInfo *DiskMapWidget::volumeFor(const QString &guid) const
{
    if (guid.isEmpty())
        return nullptr;
    for (const auto &volume : m_volumes) {
        if (volume.guid.compare(guid, Qt::CaseInsensitive) == 0)
            return &volume;
    }
    return nullptr;
}

QColor DiskMapWidget::colorFor(const PartitionInfo &partition) const
{
    if (partition.isBoot)
        return QColor(0x1D, 0x4E, 0xD8);
    return colorForRole(partition.role);
}

void DiskMapWidget::drawLegend(QPainter &painter)
{
    struct Item {
        QColor color;
        QString text;
    };
    const QVector<Item> items = {
        {QColor(0x1D, 0x4E, 0xD8), QStringLiteral("Windows 卷")},
        {QColor(0x3B, 0x82, 0xF6), QStringLiteral("数据")},
        {QColor(0x64, 0x74, 0x8B), QStringLiteral("EFI")},
        {QColor(0xA8, 0xB0, 0xBC), QStringLiteral("MSR")},
        {QColor(0xC2, 0x76, 0x2A), QStringLiteral("恢复")},
        {QColor(0x8B, 0x5C, 0xF6), QStringLiteral("动态磁盘")},
        {QColor(0x9C, 0xA3, 0xAF), QStringLiteral("未分配")},
    };

    QFont legendFont = font();
    legendFont.setPixelSize(11);
    painter.setFont(legendFont);

    int x = 16;
    const int y = 18;
    for (const auto &item : items) {
        painter.setPen(Qt::NoPen);
        painter.setBrush(item.color);
        painter.drawRoundedRect(QRectF(x, y, 10, 10), 2, 2);
        painter.setPen(palette().color(QPalette::Text));
        const int textWidth = QFontMetrics(legendFont).horizontalAdvance(item.text);
        painter.drawText(QPointF(x + 15, y + 9), item.text);
        x += 15 + textWidth + 16;
    }

    painter.setPen(QColor(128, 128, 128));
    painter.drawText(QRectF(width() - 220, y - 3, 204, 16),
                     Qt::AlignRight | Qt::AlignVCenter,
                     QStringLiteral("点击色块查看详情"));
}

void DiskMapWidget::drawDisk(QPainter &painter, int index)
{
    const DiskInfo &disk = m_disks.at(index);
    const QRect row = rowRect(index);
    const QRect bar = barRect(index);

    const QColor textColor = palette().color(QPalette::Text);
    QColor muted = textColor;
    muted.setAlpha(150);

    if (index == m_hoverDisk) {
        QColor highlight = palette().color(QPalette::Highlight);
        highlight.setAlpha(20);
        painter.fillRect(row, highlight);
    }

    // 左侧信息
    QFont titleFont = font();
    titleFont.setPixelSize(14);
    titleFont.setBold(true);
    painter.setFont(titleFont);
    painter.setPen(textColor);
    painter.drawText(QRect(16, row.top() + 6, kLeft - 26, 20),
                     Qt::AlignLeft | Qt::AlignVCenter,
                     QStringLiteral("磁盘 %1").arg(disk.number));

    QFont modelFont = font();
    modelFont.setPixelSize(12);
    painter.setFont(modelFont);
    painter.setPen(muted);
    const QFontMetrics modelMetrics(modelFont);
    painter.drawText(QRect(16, row.top() + 27, kLeft - 26, 18),
                     Qt::AlignLeft | Qt::AlignVCenter,
                     modelMetrics.elidedText(disk.model, Qt::ElideRight, kLeft - 30));

    QFont metaFont = font();
    metaFont.setPixelSize(11);
    painter.setFont(metaFont);
    painter.setPen(muted);
    QStringList meta;
    meta << DiskScanner::formatBytes(disk.size);
    meta << styleName(disk.style);
    if (!disk.busType.isEmpty())
        meta << disk.busType;
    if (disk.removable)
        meta << QStringLiteral("可移动");
    if (disk.isSystemDisk)
        meta << QStringLiteral("系统盘");
    painter.drawText(QRect(16, row.top() + 47, kLeft - 26, 18),
                     Qt::AlignLeft | Qt::AlignVCenter,
                     meta.join(QStringLiteral(" · ")));

    // 进度条底板
    QPainterPath clipPath;
    clipPath.addRoundedRect(bar, 5, 5);

    painter.save();
    painter.setPen(Qt::NoPen);
    QColor track = palette().color(QPalette::AlternateBase);
    if (track.lightness() > 235)
        track = QColor(0xE5, 0xE7, 0xEB);
    painter.setBrush(track);
    painter.drawRoundedRect(bar, 5, 5);
    painter.restore();

    painter.save();
    painter.setClipPath(clipPath);

    // 未分配空间（先画，避免盖住分区）
    for (int r = 0; r < disk.unallocated.size(); ++r) {
        const auto &region = disk.unallocated.at(r);
        const QRectF rect = segmentRect(disk, bar, region.offset, region.length);
        if (!rect.isValid())
            continue;
        painter.fillRect(rect, QColor(0x9C, 0xA3, 0xAF, 55));
        painter.fillRect(rect, QBrush(QColor(0x6B, 0x72, 0x80, 90), Qt::BDiagPattern));
        if (rect.width() > 84) {
            QFont f = font();
            f.setPixelSize(11);
            painter.setFont(f);
            painter.setPen(QColor(0x4B, 0x55, 0x63));
            painter.drawText(rect.adjusted(8, 0, -6, 0),
                             Qt::AlignLeft | Qt::AlignVCenter,
                             QStringLiteral("未分配 %1").arg(shortSize(region.length)));
        }
    }

    // 分区
    for (int p = 0; p < disk.partitions.size(); ++p) {
        const PartitionInfo &part = disk.partitions.at(p);
        const QRectF rect = segmentRect(disk, bar, part.offset, part.length);
        if (!rect.isValid())
            continue;

        const QColor base = colorFor(part);
        painter.fillRect(rect, base);

        if (index == m_hoverDisk && p == m_hoverPartition) {
            painter.fillRect(rect, QColor(255, 255, 255, 38));
        }

        const VolumeInfo *volume = volumeFor(part.volumeGuid);

        if (rect.width() >= 44) {
            const QColor text = textColorFor(base);
            const bool wide = rect.width() >= 118;

            QFont f = font();
            f.setPixelSize(13);
            f.setBold(true);
            painter.setFont(f);
            painter.setPen(text);

            const QString title = (volume && !volume->driveLetter.isEmpty())
                                      ? volume->driveLetter
                                      : roleShortName(part.role);
            painter.drawText(rect.adjusted(8, 3, -8, -3),
                             Qt::AlignLeft | Qt::AlignTop,
                             title);

            QFont small = font();
            small.setPixelSize(10);
            painter.setFont(small);

            if (wide) {
                painter.drawText(rect.adjusted(8, 3, -8, -3),
                                 Qt::AlignRight | Qt::AlignTop,
                                 DiskScanner::formatBytes(part.length));
                if (volume && volume->size > 0) {
                    const double percent = volume->size > 0
                                               ? 100.0 * double(volume->size - volume->freeBytes)
                                                     / double(volume->size)
                                               : 0.0;
                    painter.drawText(
                        rect.adjusted(8, 0, -8, -4),
                        Qt::AlignLeft | Qt::AlignBottom,
                        QStringLiteral("剩余 %1（%2% 已用）")
                            .arg(shortSize(volume->freeBytes))
                            .arg(percent, 0, 'f', 0));
                }
            }
        }

        // 快满警告
        if (volume && volume->size > 0 && volume->freeBytes * 10 < volume->size
            && rect.width() > 6) {
            painter.fillRect(QRectF(rect.left(), rect.bottom() - 3, rect.width(), 3),
                             QColor(0xEF, 0x44, 0x44));
        }

        if (index == m_selectedDisk && p == m_selectedPartition) {
            painter.setBrush(Qt::NoBrush);
            painter.setPen(QPen(QColor(0xF5, 0x9E, 0x0B), 2));
            painter.drawRoundedRect(rect.adjusted(1, 1, -1, -1), 3, 3);
        }
    }

    painter.restore();

    painter.setBrush(Qt::NoBrush);
    QColor border = palette().color(QPalette::Text);
    border.setAlpha(40);
    painter.setPen(QPen(border, 1));
    painter.drawRoundedRect(bar, 5, 5);
}

void DiskMapWidget::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event)

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::TextAntialiasing, true);
    painter.fillRect(rect(), palette().color(QPalette::Base));

    drawLegend(painter);

    if (m_disks.isEmpty()) {
        painter.setPen(palette().color(QPalette::Text));
        painter.drawText(rect().adjusted(24, 80, -24, -24),
                         Qt::AlignHCenter | Qt::AlignTop,
                         QStringLiteral("未发现磁盘。请确认已以管理员身份运行。"));
        return;
    }

    for (int i = 0; i < m_disks.size(); ++i)
        drawDisk(painter, i);
}

DiskMapWidget::Hit DiskMapWidget::hitTest(const QPoint &position) const
{
    Hit hit;
    for (int i = 0; i < m_disks.size(); ++i) {
        const QRect row = rowRect(i);
        if (!row.contains(position))
            continue;

        hit.disk = i;
        const QRect bar = barRect(i);
        if (!bar.contains(position))
            return hit;

        const DiskInfo &disk = m_disks.at(i);
        for (int p = 0; p < disk.partitions.size(); ++p) {
            if (segmentRect(disk, bar, disk.partitions.at(p).offset,
                            disk.partitions.at(p).length)
                    .contains(position)) {
                hit.partition = p;
                return hit;
            }
        }
        for (int r = 0; r < disk.unallocated.size(); ++r) {
            if (segmentRect(disk, bar, disk.unallocated.at(r).offset,
                            disk.unallocated.at(r).length)
                    .contains(position)) {
                hit.unallocated = true;
                hit.region = r;
                return hit;
            }
        }
        return hit;
    }
    return hit;
}

QString DiskMapWidget::tooltipFor(const Hit &hit) const
{
    if (hit.disk < 0 || hit.disk >= m_disks.size())
        return {};

    const DiskInfo &disk = m_disks.at(hit.disk);

    if (hit.partition >= 0 && hit.partition < disk.partitions.size()) {
        const PartitionInfo &part = disk.partitions.at(hit.partition);
        const VolumeInfo *volume = volumeFor(part.volumeGuid);

        QStringList lines;
        const QString letter = volume && !volume->driveLetter.isEmpty()
                                   ? volume->driveLetter
                                   : roleShortName(part.role);
        lines << QStringLiteral("%1 · %2").arg(letter, roleName(part.role));
        lines << QStringLiteral("%1 · 起始 %2")
                     .arg(DiskScanner::formatBytes(part.length),
                          DiskScanner::formatBytes(part.offset));
        if (volume && volume->size > 0) {
            lines << QStringLiteral("剩余 %1 / %2（%3%）")
                         .arg(DiskScanner::formatBytes(volume->freeBytes),
                              DiskScanner::formatBytes(volume->size))
                         .arg(100.0 * double(volume->size - volume->freeBytes)
                                  / double(volume->size),
                              0, 'f', 1);
        }
        lines << QStringLiteral("磁盘 %1 · 分区 %2 · %3")
                     .arg(disk.number)
                     .arg(part.partitionNumber)
                     .arg(styleName(disk.style));
        return lines.join(QLatin1Char('\n'));
    }

    if (hit.unallocated && hit.region >= 0 && hit.region < disk.unallocated.size()) {
        const auto &region = disk.unallocated.at(hit.region);
        return QStringLiteral("未分配空间 %1\n磁盘 %2 · 起始 %3")
            .arg(DiskScanner::formatBytes(region.length))
            .arg(disk.number)
            .arg(DiskScanner::formatBytes(region.offset));
    }

    return QStringLiteral("磁盘 %1\n%2 · %3 · %4")
        .arg(disk.number)
        .arg(disk.model,
             DiskScanner::formatBytes(disk.size),
             styleName(disk.style));
}

void DiskMapWidget::mousePressEvent(QMouseEvent *event)
{
    const Hit hit = hitTest(event->position().toPoint());
    if (hit.disk < 0) {
        clearSelection();
        emit selectionChanged(-1, -1);
        return;
    }

    m_selectedDisk = hit.disk;
    m_selectedPartition = hit.partition;
    update();

    const DiskInfo &disk = m_disks.at(hit.disk);
    const int number = (hit.partition >= 0 && hit.partition < disk.partitions.size())
                           ? disk.partitions.at(hit.partition).partitionNumber
                           : -1;
    emit selectionChanged(disk.number, number);
}

void DiskMapWidget::mouseMoveEvent(QMouseEvent *event)
{
    const Hit hit = hitTest(event->position().toPoint());
    if (hit.disk != m_hoverDisk || hit.partition != m_hoverPartition) {
        m_hoverDisk = hit.disk;
        m_hoverPartition = hit.partition;
        update();
    }

    if (hit.disk >= 0) {
        setCursor(hit.partition >= 0 ? Qt::PointingHandCursor : Qt::ArrowCursor);
        const QString text = tooltipFor(hit);
        if (!text.isEmpty())
            QToolTip::showText(event->globalPosition().toPoint(), text, this);
        else
            QToolTip::hideText();
    } else {
        unsetCursor();
        QToolTip::hideText();
    }
}

void DiskMapWidget::leaveEvent(QEvent *event)
{
    Q_UNUSED(event)
    m_hoverDisk = -1;
    m_hoverPartition = -1;
    QToolTip::hideText();
    update();
}
