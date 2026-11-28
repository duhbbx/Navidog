#pragma once

#include "navidog/er_model.hpp"
#include <QSet>
#include <QWidget>
#include <functional>
#include <optional>

class QImage;

namespace navidog::ui::er {
struct CardAnnotations {
    int table;
    QRectF tableNote;
    QVector<qreal> fieldTops;
    QVector<QRectF> fieldNotes;
    qreal contentBottom;
};
QImage groupImage(const navidog::er::Project&, const navidog::er::Graph&,
                  const navidog::er::Arrangement&, const navidog::er::SpatialIndex&,
                  const navidog::er::Group&);

class Canvas final : public QWidget {
public:
    explicit Canvas(QWidget* parent = nullptr);
    void reset(const navidog::er::Project&, const navidog::er::Graph&, navidog::er::Arrangement,
               navidog::er::SpatialIndex, bool keepCardState = false);
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
        navidog::er::Arrangement scene;
        navidog::er::SpatialIndex index;
        std::optional<CardAnnotations> annotations;
    };
    std::optional<ExpandedLayout> expanded_;
    QSet<navidog::er::EntityId> expandedFields_;
    std::optional<navidog::er::EntityId> annotatedCard_;
    QHash<int, QVector<QPointF>> routes_;
    QPointF origin_, anchor_, draggingStart_, last_;
    qreal scale_ = 1;
    int selection_ = -1, dragging_ = -1;
    int pressedCard_ = -1, pendingOpen_ = -1;
    bool pan_ = false, dragged_ = false, fitted_ = true;
    QPointF world(const QPointF&) const;
    int hit(const QPointF&) const;
    bool hitsFieldsLink(int table, const QPointF& point) const;
    void rebuildExpandedLayout(int anchorTable = -1);
    void zoomAt(qreal factor, const QPointF&);
};
}
