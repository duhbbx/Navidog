#include "navidog/er_routing.hpp"

#include <algorithm>
#include <array>
#include <initializer_list>
#include <limits>

namespace navidog::er {
namespace {

constexpr qreal clearance = 5;
constexpr qreal bendCost = 24;

struct Path {
    std::array<QPointF, 6> points;
    int size = 0;
    qreal cost = 0;
};

Path makePath(std::initializer_list<QPointF> points) {
    Path path;
    for (const auto& point : points) {
        if (path.size && point == path.points[path.size - 1]) continue;
        if (path.size >= 2) {
            const auto first = path.points[path.size - 1] - path.points[path.size - 2];
            const auto next = point - path.points[path.size - 1];
            if ((first.x() == 0 && next.x() == 0) || (first.y() == 0 && next.y() == 0)) {
                if (QPointF::dotProduct(first, next) < 0) return {};
                --path.size;
            }
        }
        path.points[path.size++] = point;
    }
    if (path.size < 2) return {};
    path.cost = bendCost * (path.size - 2);
    for (int i = 1; i < path.size; ++i)
        path.cost += (path.points[i] - path.points[i - 1]).manhattanLength();
    return path;
}

bool clearSegment(const Arrangement& scene, const SpatialIndex& index,
                  const QPointF& a, const QPointF& b, int ignoreA, int ignoreB) {
    const auto corridor = QRectF(a, b).normalized().adjusted(-clearance, -clearance,
                                                               clearance, clearance);
    for (int card : index.intersecting(corridor, scene))
        if (card != ignoreA && card != ignoreB) return false;
    return true;
}

}

QVector<QPointF> routeRelation(const Arrangement& scene, const SpatialIndex& index,
                               int from, int to, qreal fromY, qreal toY, int lane) {
    const auto& a = scene.boxes[from];
    const auto& b = scene.boxes[to];
    const bool sameCard = from == to;
    if (sameCard && fromY == toY) {
        fromY -= 6;
        toY += 6;
    }

    const qreal laneOffset = std::clamp(lane, -3, 3) * 3;
    const qreal stub = 21 + laneOffset;
    const qreal gutter = 16 + laneOffset;
    const qreal preferredSide = b.center().x() >= a.center().x() ? 1 : -1;
    Path best, fallback;
    qreal bestCost = std::numeric_limits<qreal>::infinity();
    qreal fallbackCost = bestCost;

    const auto endpoints = a.united(b);
    QRectF bypassBounds;
    for (int pass = 0; pass < 2 && !best.size; ++pass) {
        if (pass == 1) {
            bypassBounds = endpoints;
            const auto region = endpoints.adjusted(-32, -32, 32, 32);
            for (int card : index.intersecting(region, scene))
                bypassBounds |= scene.boxes[card];
            if (bypassBounds.top() == endpoints.top() &&
                bypassBounds.bottom() == endpoints.bottom()) break;
        }
        for (qreal out : {preferredSide, -preferredSide}) {
            for (qreal in : {-out, out}) {
                const QPointF start(out > 0 ? a.right() : a.left(), fromY);
                const QPointF finish(in > 0 ? b.right() : b.left(), toY);
                const qreal x1 = start.x() + out * stub;
                const qreal x2 = finish.x() + in * stub;
                const auto consider = [&](const Path& path) {
                    if (path.size < 2 || path.cost >= bestCost) return;
                    if ((path.points[1].x() - start.x()) * out < 12 ||
                        (path.points[path.size - 2].x() - finish.x()) * in < 12) return;
                    if (path.cost < fallbackCost) {
                        fallback = path;
                        fallbackCost = path.cost;
                    }
                    for (int i = 1; i < path.size; ++i)
                        if (!clearSegment(scene, index, path.points[i - 1], path.points[i],
                                          i == 1 ? from : -1, i == path.size - 1 ? to : -1)) return;
                    best = path;
                    bestCost = path.cost;
                };
                const auto bypass = [&](qreal y) {
                    consider(makePath({start, {x1, fromY}, {x1, y}, {x2, y}, {x2, toY}, finish}));
                };
                if (pass == 1) {
                    bypass(bypassBounds.top() - gutter);
                    bypass(bypassBounds.bottom() + gutter);
                    continue;
                }
                if (!sameCard && fromY == toY) consider(makePath({start, finish}));

                const qreal middleX = std::clamp((x1 + x2) / 2 + laneOffset,
                                                 std::min(x1, x2), std::max(x1, x2));
                for (qreal x : {middleX, x1, x2, endpoints.left() - stub, endpoints.right() + stub})
                    consider(makePath({start, {x, fromY}, {x, toY}, finish}));

                if (const auto gap = std::max(a.top(), b.top()) - std::min(a.bottom(), b.bottom());
                    gap >= 2 * clearance + 2) {
                    const qreal boundedOffset = std::clamp(laneOffset, clearance + 1 - gap / 2,
                                                           gap / 2 - clearance - 1);
                    bypass((std::max(a.top(), b.top()) + std::min(a.bottom(), b.bottom())) / 2 + boundedOffset);
                }
                for (qreal y : {endpoints.top() - gutter, endpoints.bottom() + gutter, a.top() - gutter, a.bottom() + gutter,
                                b.top() - gutter, b.bottom() + gutter})
                    bypass(y);
            }
        }
    }

    // If the available corridors are blocked, keep the shortest candidate
    // visible wherever it emerges from behind a card.
    const auto& result = best.size ? best : fallback;
    QVector<QPointF> points;
    points.reserve(result.size);
    for (int i = 0; i < result.size; ++i) points.push_back(result.points[i]);
    return points;
}

}
