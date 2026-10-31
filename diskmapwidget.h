#ifndef DISKMAPWIDGET_H
#define DISKMAPWIDGET_H

#include "diskmodel.h"

#include <QWidget>

class DiskMapWidget : public QWidget
{
    Q_OBJECT

public:
    explicit DiskMapWidget(QWidget *parent = nullptr);

    void setData(const QVector<DiskInfo> &disks, const QVector<VolumeInfo> &volumes);
    void clearSelection();

    int selectedDisk() const { return m_selectedDisk; }
    int selectedPartition() const { return m_selectedPartition; }

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

signals:
    void selectionChanged(int diskNumber, int partitionNumber);

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void leaveEvent(QEvent *event) override;

private:
    struct Hit {
        int disk = -1;
        int partition = -1;
        bool unallocated = false;
        int region = -1;
    };

    Hit hitTest(const QPoint &position) const;
    QRect rowRect(int index) const;
    QRect barRect(int index) const;
    QRectF segmentRect(const DiskInfo &disk, const QRect &bar,
                       qint64 offset, qint64 length) const;
    const VolumeInfo *volumeFor(const QString &guid) const;
    QColor colorFor(const PartitionInfo &partition) const;
    QString tooltipFor(const Hit &hit) const;

    void drawLegend(QPainter &painter);
    void drawDisk(QPainter &painter, int index);

    QVector<DiskInfo> m_disks;
    QVector<VolumeInfo> m_volumes;
    int m_selectedDisk = -1;
    int m_selectedPartition = -1;
    int m_hoverDisk = -1;
    int m_hoverPartition = -1;
};

#endif // DISKMAPWIDGET_H
