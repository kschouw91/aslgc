// SPDX-License-Identifier: MIT
#include "Graph.h"

#include <algorithm>

namespace aslgc {

QVector<GraphRow> computeGraph(const QVector<Commit>& commits) {
    QVector<GraphRow> rows;
    rows.reserve(commits.size());
    QVector<QString> lanes;  // sha each lane is waiting for; empty = free
    QVector<int> colors;
    int nextColor = 0;

    auto freeLane = [&](int exclude) {
        for (int i = 0; i < lanes.size(); ++i) {
            if (lanes[i].isEmpty() && i != exclude) return i;
        }
        lanes.push_back({});
        colors.push_back(0);
        return int(lanes.size() - 1);
    };

    for (const Commit& c : commits) {
        GraphRow row;
        QVector<int> incoming;
        for (int i = 0; i < lanes.size(); ++i) {
            if (lanes[i] == c.sha) incoming.push_back(i);
        }

        int col;
        if (incoming.isEmpty()) {
            col = freeLane(-1);
            lanes[col] = c.sha;
            colors[col] = nextColor++;
        } else {
            col = incoming.first();
        }
        row.column = col;
        row.color = colors[col];

        // Upper half: lanes converging on this commit, everything else passes through.
        for (int i = 0; i < lanes.size(); ++i) {
            if (lanes[i].isEmpty()) continue;
            if (lanes[i] == c.sha) {
                if (!incoming.isEmpty()) row.lines.push_back({i, col, colors[i], GraphLine::TopToCenter});
            } else {
                row.lines.push_back({i, i, colors[i], GraphLine::TopToBottom});
            }
        }
        for (int i : incoming) {
            if (i != col) lanes[i].clear();
        }

        // Lower half: edges to parents.
        if (c.parents.isEmpty()) {
            lanes[col].clear();
        } else {
            const QString& first = c.parents.first();
            int existing = -1;
            for (int i = 0; i < lanes.size(); ++i) {
                if (i != col && lanes[i] == first) {
                    existing = i;
                    break;
                }
            }
            if (existing >= 0) {
                row.lines.push_back({col, existing, colors[col], GraphLine::CenterToBottom});
                lanes[col].clear();
            } else {
                lanes[col] = first;
                row.lines.push_back({col, col, colors[col], GraphLine::CenterToBottom});
            }
            for (int k = 1; k < c.parents.size(); ++k) {
                const QString& p = c.parents[k];
                int lane = -1;
                for (int i = 0; i < lanes.size(); ++i) {
                    if (lanes[i] == p) {
                        lane = i;
                        break;
                    }
                }
                if (lane < 0) {
                    lane = freeLane(col);
                    lanes[lane] = p;
                    colors[lane] = nextColor++;
                }
                row.lines.push_back({col, lane, colors[lane], GraphLine::CenterToBottom});
            }
        }

        while (!lanes.isEmpty() && lanes.last().isEmpty()) {
            lanes.removeLast();
            colors.removeLast();
        }
        int width = col + 1;
        for (const GraphLine& l : row.lines) width = std::max({width, l.from + 1, l.to + 1});
        row.width = width;
        rows.push_back(std::move(row));
    }
    return rows;
}

}  // namespace aslgc
