#include "er_canvas.hpp"
#include "navidog/er_routing.hpp"
#include <QApplication>
#include <QFontMetricsF>
#include <QImage>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QSet>
#include <QTimer>
#include <utility>
#include <algorithm>
#include <cmath>
#include <iterator>
#include <map>

namespace navidog::ui::er {
namespace {
namespace m = navidog::er;
const QColor canvasBackground("#f3f6f9");
const QColor ink("#213249");
const QColor muted("#52667d");
const QColor focus("#0e8790");
constexpr int previewColumns = 4;
constexpr qreal columnZoom = .57;
constexpr qreal headerHeight = 26;
constexpr qreal columnTop = headerHeight+1;
constexpr qreal columnHeight = 20;
constexpr int noteFlags = Qt::AlignLeft | Qt::AlignTop | Qt::TextWrapAnywhere;

int displayedColumns(const m::Entity& entity,const QRectF& box) {
    const int count = int(entity.table.columns.size());
    return box.height() > m::cardHeight ? count : std::min(previewColumns,count);
}

QRectF fieldsLink(const QRectF& box) {
    return box.adjusted(8,box.height()-21,-8,-3);
}
bool hasComments(const m::Entity& entity) {
    return !entity.table.comment.isEmpty() || std::any_of(entity.table.columns.cbegin(),entity.table.columns.cend(),
        [](const auto& field) { return !field.comment.isEmpty(); });
}
QColor groupColor(const QString& name) {
    static const QColor colors[] = {QColor("#137782"), QColor("#5b5793"), QColor("#865b3a"),
                                    QColor("#376889"), QColor("#4e715e"), QColor("#96576a")};
    return colors[qHash(name, 0) % 6];
}

QFont diagramFont(int pixels, bool bold = false) {
    auto value = QApplication::font();
    value.setPixelSize(pixels);
    value.setBold(bold);
    return value;
}

CardAnnotations positionNotes(const m::Entity& entity,int table) {
    CardAnnotations notes{table,{}, {}, {},columnTop};
    const QFontMetricsF metrics(diagramFont(11));
    const auto placeNote = [&](const QString& text,qreal left,qreal gap) {
        if (text.isEmpty()) return QRectF{};
        const qreal width = m::cardWidth-left-10;
        const auto height = metrics.boundingRect(QRectF(0,0,width,1),noteFlags,text).height();
        const QRectF note(left,notes.contentBottom,width,std::ceil(height)+1);
        notes.contentBottom = note.bottom()+gap;
        return note;
    };
    notes.tableNote = placeNote(entity.table.comment,10,6);
    for (const auto& column : entity.table.columns) {
        notes.fieldTops.push_back(notes.contentBottom);
        notes.contentBottom += columnHeight;
        notes.fieldNotes.push_back(placeNote(column.comment,14,5));
    }
    return notes;
}

void drawText(QPainter& painter, const QRectF& rect, const QString& text) {
    painter.drawText(rect,Qt::AlignLeft | Qt::AlignVCenter,
                     painter.fontMetrics().elidedText(text,Qt::ElideRight,int(rect.width())));
}

qreal port(const m::Entity& e,const QRectF& box,const QStringList& keys,const CardAnnotations* notes) {
    if (!keys.isEmpty())
        for (int i = 0, count = displayedColumns(e,box); i < count; ++i)
            if (e.table.columns[i].name == keys.front())
                return box.top()+(notes ? notes->fieldTops[i] : columnTop+columnHeight*i)+columnHeight/2;
    return box.top()+m::cardHeight/2;
}

void drawEdge(QPainter& p, const QVector<QPointF>& points, const m::Edge& edge,
              bool enabled, bool highlight) {
    if (points.size() < 2) return;
    QPainterPath line(points.front());
    for (int i = 1; i + 1 < points.size(); ++i) {
        const auto before = points[i - 1] - points[i];
        const auto after = points[i + 1] - points[i];
        const auto radius = std::min({5.0, before.manhattanLength() / 2, after.manhattanLength() / 2});
        line.lineTo(points[i] + before * (radius / before.manhattanLength()));
        line.quadTo(points[i], points[i] + after * (radius / after.manhattanLength()));
    }
    line.lineTo(points.back());
    p.setBrush(Qt::NoBrush);
    if (highlight) {
        QPen halo(canvasBackground, 4.5);
        halo.setCosmetic(true);
        p.setPen(halo);
        p.drawPath(line);
    }
    QPen pen(highlight ? focus : QColor("#607d96"), highlight ? 2.25 : 1.5);
    pen.setCosmetic(true);
    if (!enabled) pen.setStyle(Qt::DashLine);
    p.setPen(pen);
    p.drawPath(line);
    pen.setStyle(Qt::SolidLine);
    p.setPen(pen);
    const auto from = points.front(), to = points.back();
    const qreal out = points[1].x() > from.x() ? 1 : -1;
    const qreal in = points[points.size() - 2].x() > to.x() ? 1 : -1;
    p.drawLine(to + QPointF(in * 7, -5), to + QPointF(in * 7, 5));
    if (edge.uniqueSource) p.drawLine(from + QPointF(out * 7, -5), from + QPointF(out * 7, 5));
    else {
        const auto tail = from + QPointF(out * 10, 0);
        p.drawLine(tail, from + QPointF(out * 2, -5));
        p.drawLine(tail, from + QPointF(out * 2, 5));
    }
}

void drawCard(QPainter& p, const m::Entity& e, const QRectF& box, const QRectF& region,
              bool selected, bool columns, bool interactive, const QFont& title, const QFont& field,
              const CardAnnotations* notes) {
    const bool view = e.table.node.kind == "view";
    const auto accent = view ? QColor("#685c92") : focus;
    QPen outline(selected ? focus : QColor("#c2cfdd"),selected ? 2 : 1);
    outline.setCosmetic(true);
    p.setPen(outline);
    p.setBrush(Qt::white);
    p.drawRoundedRect(box,6,6);
    p.setPen(Qt::NoPen);
    p.setBrush(selected ? QColor("#d6edec") : view ? QColor("#eeebf5") : QColor("#e6f1f3"));
    const QRectF header(box.left()+1,box.top()+1,box.width()-2,headerHeight-1);
    p.drawRect(header);
    p.setPen(accent);
    p.setFont(title);
    const QString name = (view ? QString("视图 · ") : QString{}) + e.table.node.object;
    drawText(p,header.adjusted(9,0,-7,0),name);
    if (!columns) return;
    const auto note = [&](const QRectF& area,const QString& text) {
        const auto target = area.translated(box.topLeft());
        if (!target.intersects(region)) return;
        p.setPen(muted);
        p.drawText(target,noteFlags,text);
    };
    p.setFont(field);
    if (notes) note(notes->tableNote,e.table.comment);
    const int shown = displayedColumns(e,box);
    const auto rowsTop = box.top()+columnTop;
    const int first = notes ? 0 : std::clamp(int(std::floor((region.top()-rowsTop)/columnHeight)),0,shown);
    const int last = notes ? shown : std::clamp(int(std::ceil((region.bottom()-rowsTop)/columnHeight)),0,shown);
    for (int i = first; i < last; ++i) {
        const auto& c = e.table.columns[i];
        const auto y = notes ? box.top()+notes->fieldTops[i] : rowsTop+i*columnHeight;
        if (y < region.bottom() && y+columnHeight > region.top()) {
            p.setPen(c.primaryKey ? accent : ink);
            drawText(p,{box.left()+10,y,132,columnHeight},(c.primaryKey ? QString("PK  ") : QString{}) + c.name);
            p.setPen(muted);
            drawText(p,{box.left()+153,y,89,columnHeight},c.type);
        }
        if (notes) note(notes->fieldNotes[i],c.comment);
    }
    if (e.table.columns.size() > shown) {
        p.setPen(interactive ? accent : muted);
        drawText(p,fieldsLink(box),QString(interactive ? "展开其余 %1 个字段" : "另有 %1 个字段")
                 .arg(e.table.columns.size()-shown));
    } else if (interactive && !notes && box.height() > m::cardHeight) {
        p.setPen(accent);
        drawText(p,fieldsLink(box),"收起字段");
    }
}

QPointF mousePosition(const QMouseEvent* event) {
#if QT_VERSION >= QT_VERSION_CHECK(6,0,0)
    return event->position();
#else
    return event->localPos();
#endif
}

void paintDiagram(QPainter& p, const m::Project& project, const m::Graph& graph,
                  const m::Arrangement& scene, const m::SpatialIndex& index,
                  QHash<int, QVector<QPointF>>& routes, const QVector<int>& visible, const QRectF& region,
                  qreal scale, int selected, const m::Group* only, const CardAnnotations* annotations) {
    p.setRenderHint(QPainter::Antialiasing,scale >= .26);
    const auto label = diagramFont(15,true);
    const auto title = diagramFont(13,true);
    const auto field = diagramFont(11);
    const auto drawGroup = [&](const m::Group& group) {
        if (!region.intersects(group.bounds)) return;
        const auto color = groupColor(group.name);
        auto shade = color;
        shade.setAlpha(14);
        QPen border(color.lighter(168));
        border.setCosmetic(true);
        p.setPen(border);
        p.setBrush(shade);
        if (scale >= .22) p.drawRoundedRect(group.bounds,11,11);
        else p.drawRect(group.bounds);
        if (scale < .32) return;
        p.setFont(label);
        p.setPen(color);
        drawText(p,{group.bounds.left()+15,group.bounds.top()+8,group.bounds.width()-30,24},
                 group.name + "  ·  " + QString::number(group.members.size()));
    };
    if (only) drawGroup(*only);
    else for (const auto& group : scene.groups) drawGroup(group);

    if (scale >= .25 || (only && visible.size() <= 2000)) {
        QSet<int> visited;
        QSet<int> membership;
        if (only) membership = QSet<int>(only->members.cbegin(),only->members.cend());
        QVector<int> highlighted;
        const auto drawConnection = [&](int id, bool highlight) {
            const auto& edge = graph.edges[id];
            const auto& source = project.entities[edge.from];
            const auto& target = project.entities[edge.to];
            const auto& key = source.table.objects[edge.constraint];
            auto route = routes.constFind(id);
            if (route == routes.cend()) {
                if (routes.size() >= 8192) routes.clear();
                const auto fromY = port(source, scene.boxes[edge.from], key.columns,
                                       annotations && annotations->table == edge.from ? annotations : nullptr);
                const auto toY = port(target, scene.boxes[edge.to], key.referenceColumns,
                                     annotations && annotations->table == edge.to ? annotations : nullptr);
                route = routes.insert(id, m::routeRelation(scene, index, edge.from, edge.to,
                                                          fromY, toY, id % 7 - 3));
            }
            drawEdge(p, route.value(), edge, key.enabled, highlight);
        };
        int edgeBudget = 3000;
        const auto connections = [&](int i) {
            for (int id : graph.incidents[i]) {
                if (edgeBudget <= 0) break;
                if (visited.contains(id)) continue;
                const auto& edge = graph.edges[id];
                if (only && (!membership.contains(edge.from) || !membership.contains(edge.to))) continue;
                if (!scene.boxes[edge.from].united(scene.boxes[edge.to]).adjusted(-64,-64,64,64).intersects(region)) continue;
                visited.insert(id);
                --edgeBudget;
                if (edge.from == selected || edge.to == selected) highlighted.push_back(id);
                else drawConnection(id, false);
            }
        };
        if (selected >= 0 && selected < graph.incidents.size()) connections(selected);
        for (int i : visible) {
            if (edgeBudget <= 0) break;
            connections(i);
        }
        for (int id : highlighted) drawConnection(id, true);
    }

    if (scale < .30) {
        QVector<QRectF> boxes;
        boxes.reserve(visible.size());
        for (int i : visible) if (i != selected) boxes << scene.boxes[i];
        p.setPen(Qt::NoPen);
        p.setBrush(QColor("#7296a8"));
        if (!boxes.isEmpty()) p.drawRects(boxes.constData(),int(boxes.size()));
        if (selected >= 0 && scene.boxes[selected].isValid()) {
            p.setBrush(focus);
            p.drawRect(scene.boxes[selected]);
        }
        return;
    }
    const bool showColumns = scale >= columnZoom;
    for (int i : visible)
        drawCard(p,project.entities[i],scene.boxes[i],region,i == selected,showColumns,!only,title,field,
                 annotations && annotations->table == i ? annotations : nullptr);
}
}

QImage groupImage(const m::Project& project,const m::Graph& graph,const m::Arrangement& scene,
                  const m::SpatialIndex& index,const m::Group& group) {
    const auto region = group.bounds.adjusted(-14,-14,14,14);
    const qreal scale = std::min({2.0,4096.0/region.width(),4096.0/region.height()});
    const QSize pixels(std::max(1,int(std::ceil(region.width()*scale))),
                       std::max(1,int(std::ceil(region.height()*scale))));
    QImage image(pixels,QImage::Format_RGB32);
    image.fill(canvasBackground);
    QPainter painter(&image);
    painter.scale(scale,scale);
    painter.translate(-region.topLeft());
    QHash<int, QVector<QPointF>> routes;
    paintDiagram(painter,project,graph,scene,index,routes,group.members,region,scale,-1,&group,nullptr);
    return image;
}

Canvas::Canvas(QWidget* parent) : QWidget(parent) {
    setObjectName("erCanvas");
    setMinimumWidth(280);
    setFocusPolicy(Qt::StrongFocus);
    setCursor(Qt::OpenHandCursor);
    setAttribute(Qt::WA_OpaquePaintEvent);
    setAccessibleName("ER 图画布");
    setToolTip("拖动空白区域平移，滚轮缩放。点击卡片底部展开字段，在工具栏切换所选表的注释。");
}

void Canvas::reset(const m::Project& project, const m::Graph& graph, m::Arrangement scene,
                   m::SpatialIndex index, bool keepCardState) {
    expanded_.reset();
    if (!keepCardState) {
        expandedFields_.clear();
        annotatedCard_.reset();
        selection_ = -1;
    }
    pressedCard_ = pendingOpen_ = -1;
    project_ = &project;
    graph_ = &graph;
    arrangement_ = std::move(scene);
    index_ = std::move(index);
    if (selection_ >= arrangement_.boxes.size()) selection_ = -1;
    rebuildExpandedLayout();
    if (fitted_) fit();
}
QPointF Canvas::world(const QPointF& point) const { return origin_ + point/scale_; }

int Canvas::hit(const QPointF& point) const {
    if (!project_) return -1;
    const auto& index = expanded_ ? expanded_->index : index_;
    const auto found = index.intersecting(QRectF(point-QPointF(.01,.01),QSizeF(.02,.02)),arrangement());
    if (found.isEmpty()) return -1;
    if (found.contains(selection_)) return selection_;
    return *std::max_element(found.cbegin(),found.cend());
}
bool Canvas::hitsFieldsLink(int table,const QPointF& point) const {
    if (table < 0 || scale_ < columnZoom) return false;
    const auto& box = arrangement().boxes[table];
    const auto& entity = project_->entities[table];
    const bool annotated = annotatedCard_ == entity.id;
    return !annotated && fieldsLink(box).contains(point) && entity.table.columns.size() > previewColumns;
}
bool Canvas::canShowAnnotations() const {
    return project_ && selection_ >= 0 && selection_ < arrangement_.boxes.size() &&
        arrangement_.boxes[selection_].isValid() && hasComments(project_->entities[selection_]);
}
bool Canvas::annotationsVisible() const {
    return expanded_ && expanded_->annotations && expanded_->annotations->table == selection_;
}
void Canvas::setAnnotationsVisible(bool visible) {
    if (!canShowAnnotations() || visible == annotationsVisible()) return;
    if (visible) annotatedCard_ = project_->entities[selection_].id;
    else annotatedCard_.reset();
    fitted_ = false;
    rebuildExpandedLayout(selection_);
    if (!visible) return;
    const QRectF viewport(origin_,QSizeF(width()/scale_,height()/scale_));
    const auto title = arrangement().boxes[selection_].topLeft()+QPointF(m::cardWidth/2,headerHeight/2);
    if (scale_ < columnZoom || !viewport.contains(title)) select(selection_,true);
}
void Canvas::rebuildExpandedLayout(int anchorTable) {
    const auto anchor = anchorTable >= 0 ? arrangement().boxes[anchorTable] : QRectF{};
    QHash<int, qreal> heights;
    for (auto it = expandedFields_.begin(); it != expandedFields_.end();) {
        const int i = graph_->byId.value(*it, -1);
        if (i < 0 || !arrangement_.boxes[i].isValid() ||
            project_->entities[i].table.columns.size() <= previewColumns) {
            it = expandedFields_.erase(it);
            continue;
        }
        heights.insert(i, arrangement_.boxes[i].height() +
                       columnHeight * (project_->entities[i].table.columns.size() - previewColumns));
        ++it;
    }
    std::optional<CardAnnotations> notes;
    if (annotatedCard_) {
        const int i = graph_->byId.value(*annotatedCard_, -1);
        if (i >= 0 && arrangement_.boxes[i].isValid() && hasComments(project_->entities[i])) {
            notes = positionNotes(project_->entities[i], i);
            heights.insert(i, std::max(arrangement_.boxes[i].height(), notes->contentBottom + 8));
        } else annotatedCard_.reset();
    }

    if (heights.isEmpty()) expanded_.reset();
    else {
        ExpandedLayout next{arrangement_, {}, std::move(notes)};
        std::map<qreal, qreal> rowGrowth;
        // Cards with the same baseline share the extra height of the tallest card.
        for (auto it = heights.cbegin(); it != heights.cend(); ++it) {
            const auto& base = arrangement_.boxes[it.key()];
            auto& extra = rowGrowth[base.bottom()];
            extra = std::max(extra, it.value() - base.height());
        }
        qreal cumulative = 0;
        for (auto& row : rowGrowth) {
            cumulative += row.second;
            row.second = cumulative;
        }
        const auto offsetAt = [&](qreal y) {
            const auto nextRow = rowGrowth.upper_bound(y);
            return nextRow == rowGrowth.begin() ? 0.0 : std::prev(nextRow)->second;
        };
        for (int i = 0; i < next.scene.boxes.size(); ++i) {
            auto& box = next.scene.boxes[i];
            if (!box.isValid()) continue;
            box.translate(0, offsetAt(box.top()));
            box.setHeight(heights.value(i, box.height()));
        }
        const auto extendBounds = [&](QRectF& bounds) {
            if (!bounds.isValid()) return;
            bounds.setBottom(bounds.bottom() + offsetAt(bounds.bottom()));
            bounds.setTop(bounds.top() + offsetAt(bounds.top()));
        };
        for (auto& group : next.scene.groups) extendBounds(group.bounds);
        extendBounds(next.scene.bounds);
        next.index.reset(next.scene);
        expanded_ = std::move(next);
    }
    if (anchor.isValid()) {
        const auto& box = arrangement().boxes[anchorTable];
        origin_ += box.topLeft() - anchor.topLeft();
        if (box.height() < anchor.height() && box.bottom() < origin_.y() + 12 / scale_)
            origin_.setY(box.top() - 12 / scale_);
    }
    routes_.clear();
    update();
    if (annotationStateChanged) annotationStateChanged();
}

void Canvas::select(int table,bool locate) {
    const bool changed = table != selection_;
    selection_ = table;
    const auto& boxes = arrangement().boxes;
    if (locate && table >= 0 && table < boxes.size() && boxes[table].isValid()) {
        scale_ = std::max(scale_,.85);
        origin_ = boxes[table].topLeft()+QPointF(m::cardWidth,m::cardHeight)/2-QPointF(width(),height())/(2*scale_);
        fitted_ = false;
        if (zoomed) zoomed(scale_);
    }
    update();
    if (changed && annotationStateChanged) annotationStateChanged();
}
void Canvas::fit() {
    const auto bounds = arrangement().bounds;
    if (!bounds.isValid()) { origin_ = {}; scale_ = 1; }
    else {
        scale_ = std::clamp(std::min((width()-36.0)/bounds.width(),
                                    (height()-36.0)/bounds.height()),.0005,1.15);
        origin_ = bounds.center()-QPointF(width(),height())/(2*scale_);
    }
    fitted_ = true;
    if (zoomed) zoomed(scale_);
    update();
}
void Canvas::zoomAt(qreal factor,const QPointF& point) {
    const auto fixed = world(point);
    scale_ = std::clamp(scale_*factor,.0005,3.0);
    origin_ = fixed-point/scale_;
    fitted_ = false;
    if (zoomed) zoomed(scale_);
    update();
}
void Canvas::zoom(qreal factor) { zoomAt(factor,QPointF(width()/2.,height()/2.)); }
void Canvas::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.fillRect(rect(),canvasBackground);
    if (!project_ || !graph_ || graph_->active.isEmpty()) {
        painter.setFont(diagramFont(15,true));
        painter.setPen(muted);
        painter.drawText(rect(),Qt::AlignCenter,"导入数据库结构，或新建表开始建模");
        return;
    }
    const QRectF screen(origin_,QSizeF(width()/scale_,height()/scale_));
    const auto& index = expanded_ ? expanded_->index : index_;
    const auto visible = index.intersecting(screen,arrangement());
    painter.scale(scale_,scale_);
    painter.translate(-origin_);
    const auto* notes = expanded_ && expanded_->annotations ? &*expanded_->annotations : nullptr;
    paintDiagram(painter,*project_,*graph_,arrangement(),index,routes_,visible,screen,scale_,selection_,nullptr,notes);
}
void Canvas::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    if (fitted_) fit();
}
void Canvas::wheelEvent(QWheelEvent* event) {
    const qreal delta = event->angleDelta().y();
    if (!delta) { event->ignore(); return; }
    zoomAt(std::pow(1.0018,delta),event->position());
    event->accept();
}
void Canvas::mousePressEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton && event->button() != Qt::MiddleButton) return;
    pendingOpen_ = -1;
    last_ = anchor_ = mousePosition(event);
    dragging_ = event->button() == Qt::LeftButton ? hit(world(anchor_)) : -1;
    pressedCard_ = hitsFieldsLink(dragging_,world(anchor_)) ? dragging_ : -1;
    pan_ = dragging_ < 0;
    dragged_ = false;
    setCursor(pressedCard_ >= 0 ? Qt::PointingHandCursor : Qt::ClosedHandCursor);
    if (dragging_ >= 0) {
        draggingStart_ = arrangement().boxes[dragging_].topLeft();
        select(dragging_,false);
        if (selected) selected(dragging_);
    }
    setFocus();
}
void Canvas::mouseMoveEvent(QMouseEvent* event) {
    if (!(event->buttons() & (Qt::LeftButton | Qt::MiddleButton))) return;
    const auto point = mousePosition(event);
    dragged_ |= (point-anchor_).manhattanLength() >= QApplication::startDragDistance();
    if (pressedCard_ >= 0) return;
    if (pan_) {
        origin_ -= (point-last_)/scale_;
        fitted_ = false;
        update();
    } else if (dragging_ >= 0 && dragged_) {
        auto& scene = expanded_ ? expanded_->scene : arrangement_;
        auto& index = expanded_ ? expanded_->index : index_;
        auto& box = scene.boxes[dragging_];
        const auto before = box;
        box.moveTopLeft(draggingStart_+(point-anchor_)/scale_);
        index.move(dragging_,before,box);
        scene.bounds |= box;
        routes_.clear();
        update();
    }
    last_ = point;
}
void Canvas::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton && event->button() != Qt::MiddleButton) return;
    const int completed = std::exchange(dragging_,-1);
    const int card = std::exchange(pressedCard_,-1);
    pan_ = false;
    setCursor(Qt::OpenHandCursor);
    if (event->button() == Qt::LeftButton) {
        const int table = std::exchange(pendingOpen_,-1);
        if (table >= 0 && !dragged_ && hit(world(mousePosition(event))) == table) {
            QTimer::singleShot(0,this,[this,table] {
                if (isVisible() && opened)
                    opened(table);
            });
            return;
        }
    }
    if (card >= 0) {
        if (!dragged_ && hitsFieldsLink(card,world(mousePosition(event)))) {
            const auto id = project_->entities[card].id;
            if (!expandedFields_.remove(id)) expandedFields_.insert(id);
            fitted_ = false;
            rebuildExpandedLayout(card);
        }
        return;
    }
    if (dragged_ && completed >= 0) {
        auto& box = arrangement_.boxes[completed];
        const bool expandedLayout = expanded_.has_value();
        if (expanded_) {
            const auto before = box;
            box.translate(expanded_->scene.boxes[completed].topLeft() - draggingStart_);
            index_.move(completed, before, box);
        }
        for (auto& group : arrangement_.groups)
            if (group.members.contains(completed)) {
                group.bounds |= box;
                break;
            }
        arrangement_.bounds |= box;
        if (expandedLayout) rebuildExpandedLayout(completed);
        if (moved) moved(completed,box.topLeft());
    }
}
void Canvas::mouseDoubleClickEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton) return;
    const auto i = hit(world(mousePosition(event)));
    pendingOpen_ = i >= 0 && !hitsFieldsLink(i,world(mousePosition(event))) ? i : -1;
}
void Canvas::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Escape && expanded_ && selection_ >= 0) {
        const auto id = project_->entities[selection_].id;
        expandedFields_.remove(id);
        if (annotatedCard_ == id) annotatedCard_.reset();
        rebuildExpandedLayout(selection_);
    } else if (event->key() == Qt::Key_Plus || event->key() == Qt::Key_Equal) zoom(1.25);
    else if (event->key() == Qt::Key_Minus) zoom(.8);
    else if (event->key() == Qt::Key_0) fit();
    else QWidget::keyPressEvent(event);
}
}
