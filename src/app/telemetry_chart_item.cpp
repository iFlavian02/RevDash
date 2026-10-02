#include "app/telemetry_chart_item.hpp"

#include <QSGFlatColorMaterial>
#include <QSGGeometryNode>
#include <QThread>
#include <algorithm>
#include <cmath>

namespace revdash::app {
TelemetryChartItem::TelemetryChartItem(QQuickItem* parent):QQuickItem(parent) { setFlag(ItemHasContents, true); }
void TelemetryChartItem::setMetricId(int value) { if(metric_id_==value)return; metric_id_=value; clear(); emit metricIdChanged(); }
void TelemetryChartItem::setHistoryCapacity(int value) { value=std::max(2,value); if(history_capacity_==value)return; history_capacity_=value; if(samples_.size()>value)samples_.remove(0,samples_.size()-value); update(); emit historyCapacityChanged(); emit sampleCountChanged(); }
void TelemetryChartItem::setHistorySeconds(int value) {
    value = std::clamp(value, 1, 120);
    if (history_seconds_ == value) return;
    history_seconds_ = value;
    setHistoryCapacity(history_seconds_ * 10);
    emit historySecondsChanged();
}
void TelemetryChartItem::setColor(const QColor& value) { if(color_==value)return;color_=value;update();emit colorChanged(); }
void TelemetryChartItem::appendSample(int metricId,double value) { Q_ASSERT(QThread::currentThread()==thread()); if(metricId!=metric_id_||!std::isfinite(value))return; if(samples_.size()==history_capacity_)samples_.removeFirst(); samples_.append(value); update(); emit sampleCountChanged(); }
void TelemetryChartItem::clear() { Q_ASSERT(QThread::currentThread()==thread()); if(samples_.isEmpty())return;samples_.clear();update();emit sampleCountChanged(); }
QSGNode* TelemetryChartItem::updatePaintNode(QSGNode* oldNode,UpdatePaintNodeData*) {
    auto* node=static_cast<QSGGeometryNode*>(oldNode);
    if(!node){ node=new QSGGeometryNode; auto* material=new QSGFlatColorMaterial; node->setMaterial(material); node->setFlag(QSGNode::OwnsMaterial); }
    auto* geometry=node->geometry(); const int count=samples_.size();
    if(!geometry){ geometry=new QSGGeometry(QSGGeometry::defaultAttributes_Point2D(),count); geometry->setDrawingMode(QSGGeometry::DrawLineStrip); node->setGeometry(geometry); node->setFlag(QSGNode::OwnsGeometry); } else geometry->allocate(count);
    static_cast<QSGFlatColorMaterial*>(node->material())->setColor(color_);
    if(count>0){ const auto [lowIt,highIt]=std::minmax_element(samples_.cbegin(),samples_.cend()); const double low=*lowIt; const double span=std::max(0.000001,*highIt-low); auto* vertices=geometry->vertexDataAsPoint2D(); for(int i=0;i<count;++i){ const float x=count==1?0.0F:static_cast<float>(i)*static_cast<float>(width())/static_cast<float>(count-1); const float y=static_cast<float>(height()-(samples_[i]-low)/span*height()); vertices[i].set(x,y); } }
    node->markDirty(QSGNode::DirtyGeometry|QSGNode::DirtyMaterial);
    return node;
}
} // namespace revdash::app
