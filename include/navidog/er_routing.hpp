#pragma once

#include "er_model.hpp"

namespace navidog::er {

// Ports are on the left or right side of a card at the given world-space Y.
QVector<QPointF> routeRelation(const Arrangement&, const SpatialIndex&, int from, int to,
                               qreal fromY, qreal toY, int lane = 0);

}
