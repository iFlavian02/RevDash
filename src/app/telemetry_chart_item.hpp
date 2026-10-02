#pragma once

#include <QColor>
#include <QPointF>
#include <QQuickItem>
#include <QVector>
#include <qqmlregistration.h>

namespace revdash::app {

class TelemetryChartItem : public QQuickItem {
    Q_OBJECT
    QML_NAMED_ELEMENT(TelemetryChartItem)
    Q_PROPERTY(int metricId READ metricId WRITE setMetricId NOTIFY metricIdChanged)
    Q_PROPERTY(int historyCapacity READ historyCapacity WRITE setHistoryCapacity NOTIFY historyCapacityChanged)
    Q_PROPERTY(int historySeconds READ historySeconds WRITE setHistorySeconds NOTIFY historySecondsChanged)
    Q_PROPERTY(QColor color READ color WRITE setColor NOTIFY colorChanged)
    Q_PROPERTY(int sampleCount READ sampleCount NOTIFY sampleCountChanged)

public:
    explicit TelemetryChartItem(QQuickItem* parent = nullptr);
    int metricId() const noexcept { return metric_id_; }
    int historyCapacity() const noexcept { return history_capacity_; }
    int historySeconds() const noexcept { return history_seconds_; }
    QColor color() const { return color_; }
    int sampleCount() const noexcept { return samples_.size(); }
    void setMetricId(int value);
    void setHistoryCapacity(int value);
    void setHistorySeconds(int value);
    void setColor(const QColor& value);
    Q_INVOKABLE void appendSample(int metricId, double value);
    Q_INVOKABLE void clear();

signals:
    void metricIdChanged();
    void historyCapacityChanged();
    void historySecondsChanged();
    void colorChanged();
    void sampleCountChanged();

protected:
    QSGNode* updatePaintNode(QSGNode* oldNode, UpdatePaintNodeData*) override;

private:
    int metric_id_{0};
    int history_capacity_{600};
    int history_seconds_{60};
    QColor color_{QStringLiteral("#39d98a")};
    QVector<double> samples_;
};
} // namespace revdash::app
