// SPDX-License-Identifier: MIT
#pragma once

#include "Types.h"

namespace aslgc {

// One line segment inside a commit row of the history graph. Lanes are columns;
// vertical positions are the top edge, the node centre, or the bottom edge.
struct GraphLine {
    enum Kind : quint8 { TopToBottom, TopToCenter, CenterToBottom };
    int from = 0;
    int to = 0;
    int color = 0;
    Kind kind = TopToBottom;
};

struct GraphRow {
    int column = 0;  // lane of the commit node
    int color = 0;   // colour index of the node
    int width = 1;   // number of lanes touched by this row
    QVector<GraphLine> lines;
};

// Assigns lanes to commits listed children-before-parents (e.g. --date-order).
QVector<GraphRow> computeGraph(const QVector<Commit>& commits);

}  // namespace aslgc
