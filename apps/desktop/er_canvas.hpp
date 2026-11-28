#pragma once

#include "navidog/er_model.hpp"
#include <QColor>
#include <QWidget>
#include <functional>
#include <optional>

class QPainter;
class QImage;

namespace navidog::ui::er {
struct CardAnnotations {
    int table;
    QRectF tableNote;
    QVector<qreal> fieldTops;
    QVector<QRectF> fieldNotes;
    qreal contentBottom;
};
void paintDiagram(QPainter&, const navidog::er::Project&, const navidog::er::Graph&,
                  const navidog::er::Arrangement&, const QVector<int>& visible, const QRectF& region,
                  qreal scale, int selected = -1, const navidog::er::Group* only = nullptr,
                  const CardAnnotations* annotations = nullptr);
QImage groupImage(const navidog::er::Project&, const navidog::er::Graph&,
                  const navidog::er::Arrangement&, const navidog::er::Group&);

class Canvas final : public QWidget {
public:
    explicit Canvas(QWidget* parent = nullptr);
    void reset(const navidog::er::Project&, const navidog::er::Graph&, navidog::er::Arrangement, navidog::er::SpatialIndex);
    void select(int table, bool locate);
    void fit();
    void zoom(qreal factor);
    bool canShowAnnotations() const;
    bool annotationsVisible() const;
    void setAnnotationsVisible(bool visible);
    const navidog::er::Arrangement& arrangement() const { return expanded_ ? expanded_->scene : arrangement_; }
    std::function<void(int)> selected;
    std::function<void(int)> opened;
    std::function<void(int,const QPointF&)> moved;
    std::function<void(qreal)> zoomed;
    std::function<void()> annotationStateChanged;
protected:
    void paintEvent(QPaintEvent*) override;
    void resizeEvent(QResizeEvent*) override;
    void wheelEvent(QWheelEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void mouseDoubleClickEvent(QMouseEvent*) override;
    void keyPressEvent(QKeyEvent*) override;
private:
    const navidog::er::Project* project_ = nullptr;
    const navidog::er::Graph* graph_ = nullptr;
    navidog::er::Arrangement arrangement_;
    navidog::er::SpatialIndex index_;
    struct ExpandedLayout {
        int table;
        bool allFields;
        navidog::er::Arrangement scene;
        navidog::er::SpatialIndex index;
        std::optional<CardAnnotations> annotations;
    };
    std::optional<ExpandedLayout> expanded_;
    QPointF origin_, anchor_, draggingStart_, last_;
    qreal scale_ = 1;
    int selection_ = -1, dragging_ = -1;
    int pressedCard_ = -1, pendingOpen_ = -1;
    bool pan_ = false, dragged_ = false, fitted_ = true;
    QPointF world(const QPointF&) const;
    int hit(const QPointF&) const;
    bool hitsFieldsLink(int table, const QPointF& point) const;
    void expandCard(int table, bool allFields, bool showComments);
    void collapseCard(bool revealCard = true);
    void zoomAt(qreal factor, const QPointF&);
};
}
