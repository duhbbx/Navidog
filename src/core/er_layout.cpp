#include "navidog/er_model.hpp"
#include <QMap>
#include <QSet>
#include <algorithm>
#include <cmath>
#include <limits>

namespace navidog::er {
namespace {
constexpr qreal slotWidth = cardWidth + 62;
constexpr qreal slotHeight = cardHeight + 55;
constexpr qreal padding = 28;
constexpr qreal headerHeight = 46;
constexpr qreal cell = 512;

QVector<int> ordered(const Project& project, const Graph& graph, const QVector<int>& members) {
    auto list = members;
    std::sort(list.begin(),list.end(),[&](int a, int b) {
        const auto& left = project.entities[a].table.node;
        const auto& right = project.entities[b].table.node;
        return std::tie(left.schema,left.database,left.object,a) <
               std::tie(right.schema,right.database,right.object,b);
    });
    if (project.layout != LayoutKind::GravityGrid || list.size() < 3) return list;

    // A bounded barycentric sweep pulls neighboring cards together within
    // each prefix/schema independent of the total number of groups.
    QHash<int,int> ranks;
    ranks.reserve(list.size());
    QVector<std::pair<double,int>> weighted;
    weighted.reserve(list.size());
    for (int pass = 0; pass < 4; ++pass) {
        for (int i = 0; i < list.size(); ++i) ranks.insert(list[i],i);
        weighted.clear();
        for (int i : list) {
            double sum = 0;
            int count = 0;
            for (int e : graph.incidents[i]) {
                const auto& edge = graph.edges[e];
                const int other = edge.from == i ? edge.to : edge.from;
                const auto rank = ranks.constFind(other);
                if (rank != ranks.cend() && other != i) {
                    sum += rank.value();
                    ++count;
                }
            }
            const auto current = ranks.value(i);
            weighted.push_back({count ? (current + sum/count)*0.5 : double(current),i});
        }
        std::stable_sort(weighted.begin(),weighted.end(),[](const auto& a,const auto& b) { return a.first < b.first; });
        for (int i = 0; i < list.size(); ++i) list[i] = weighted[i].second;
    }
    return list;
}

QRectF placeGroup(const Project& project, const Graph& graph, const QVector<int>& members,
                 QVector<QRectF>& boxes) {
    const auto list = ordered(project,graph,members);
    const int columns = std::max(1,int(std::ceil(std::sqrt(list.size()*0.74))));
    int maxColumns = columns, rows = (list.size()+columns-1)/columns;
    if (project.layout != LayoutKind::DependencyLayers) {
        for (int i = 0; i < list.size(); ++i)
            boxes[list[i]] = QRectF(padding + (i%columns)*slotWidth,
                                    headerHeight + (i/columns)*slotHeight,cardWidth,cardHeight);
    } else {
        // Position dependencies before dependants. Cycles share the last
        // layer. Limit layer width/height so a long chain remains navigable.
        QHash<int,int> pending, level;
        QVector<int> queue;
        for (int i : list) pending.insert(i,0);
        for (int i : list) {
            for (int id : graph.incidents[i]) {
                const auto& edge = graph.edges[id];
                if (edge.from == i && edge.to != i && pending.contains(edge.to)) ++pending[i];
            }
            if (!pending.value(i)) queue.push_back(i);
        }
        for (int pos = 0; pos < queue.size(); ++pos) {
            const int i = queue[pos];
            for (int id : graph.incidents[i]) {
                const auto& edge = graph.edges[id];
                if (edge.to != i || edge.from == i || !pending.contains(edge.from)) continue;
                level[edge.from] = std::min(23,std::max(level.value(edge.from),level.value(i)+1));
                if (--pending[edge.from] == 0) queue.push_back(edge.from);
            }
        }
        QMap<int,QVector<int>> layers;
        for (int i : list) layers[pending.value(i) ? 24 : level.value(i)].push_back(i);
        int x = 0;
        rows = 0;
        const int rowLimit = std::max(1,int(std::ceil(std::sqrt(list.size()))));
        for (const auto& nodes : layers) {
            for (int i = 0; i < nodes.size(); ++i)
                boxes[nodes[i]] = QRectF(padding+(x+i/rowLimit)*slotWidth,
                                        headerHeight+(i%rowLimit)*slotHeight,cardWidth,cardHeight);
            x += (nodes.size()+rowLimit-1)/rowLimit;
            rows = std::max(rows,std::min(rowLimit,int(nodes.size())));
        }
        maxColumns = x;
    }
    return {0,0,2*padding+(maxColumns-1)*slotWidth+cardWidth,
            headerHeight+padding+(rows-1)*slotHeight+cardHeight};
}

quint64 bucketKey(int x, int y) { return (quint64(quint32(x)) << 32) | quint32(y); }
struct Cells { int left, right, top, bottom; };
Cells cells(const QRectF& r) {
    return {int(std::floor(r.left()/cell)),int(std::floor(r.right()/cell)),
            int(std::floor(r.top()/cell)),int(std::floor(r.bottom()/cell))};
}
template<class Visit>
void visitBuckets(const QRectF& box, const Visit& visit) {
    if (!box.isValid()) return;
    const auto range = cells(box);
    for (int y = range.top; y <= range.bottom; ++y)
        for (int x = range.left; x <= range.right; ++x)
            visit(bucketKey(x,y));
}
}

Arrangement arrange(const Project& project, const Graph& graph) {
    Arrangement scene;
    scene.boxes.resize(project.entities.size());
    QMap<QString,QVector<int>> members;
    for (int i : graph.active) members[groupName(project,project.entities[i])].push_back(i);
    qreal area = 0, widest = 0;
    for (auto it = members.cbegin(); it != members.cend(); ++it) {
        auto rect = placeGroup(project,graph,it.value(),scene.boxes);
        area += (rect.width()+padding)*(rect.height()+padding);
        widest = std::max(widest,rect.width());
        scene.groups.push_back({it.key(),it.value(),rect});
    }
    const qreal shelfWidth = std::max(widest,std::sqrt(area*1.45));
    qreal x = 0, y = 0, row = 0;
    for (auto& group : scene.groups) {
        if (x > 0 && x+group.bounds.width() > shelfWidth) {
            x = 0;
            y += row + padding;
            row = 0;
        }
        const QPointF shift(x,y);
        group.bounds.translate(shift);
        const QRectF shelfBox = group.bounds;
        for (int i : group.members) {
            auto& box = scene.boxes[i];
            if (const auto& saved = project.entities[i].position) box.moveTopLeft(*saved);
            else box.translate(shift);
            group.bounds |= box.adjusted(-padding,-padding,padding,padding);
        }
        scene.bounds |= group.bounds;
        x += shelfBox.width() + padding;
        row = std::max(row,shelfBox.height());
    }
    return scene;
}

void SpatialIndex::reset(const Arrangement& scene) {
    buckets_.clear();
    buckets_.reserve(scene.boxes.size());
    for (int i = 0; i < scene.boxes.size(); ++i)
        visitBuckets(scene.boxes[i],[&](quint64 key){ buckets_[key].push_back(i); });
}

void SpatialIndex::move(int index, const QRectF& before, const QRectF& after) {
    visitBuckets(before,[&](quint64 key) {
        auto it = buckets_.find(key);
        if (it == buckets_.end()) return;
        it->removeOne(index);
        if (it->isEmpty()) buckets_.erase(it);
    });
    visitBuckets(after,[&](quint64 key){ buckets_[key].push_back(index); });
}

QVector<int> SpatialIndex::intersecting(const QRectF& visible,const Arrangement& scene) const {
    QVector<int> result;
    const auto range = cells(visible);
    const qint64 count = qint64(range.right-range.left+1)*(range.bottom-range.top+1);
    if (count > 2048) {
        for (int i = 0; i < scene.boxes.size(); ++i)
            if (scene.boxes[i].intersects(visible)) result.push_back(i);
        return result;
    }
    QSet<int> seen;
    visitBuckets(visible,[&](quint64 key) {
        const auto it = buckets_.constFind(key);
        if (it == buckets_.cend()) return;
        for (int index : *it) {
            if (seen.contains(index) || !scene.boxes[index].intersects(visible)) continue;
            seen.insert(index);
            result.push_back(index);
        }
    });
    return result;
}
}
