// SPDX-License-Identifier: MIT
#pragma once

#include <QColor>
#include <QIcon>
#include <QString>
#include <QStyle>

namespace aslgc::theme {

bool isDark();
QColor mix(const QColor& a, const QColor& b, qreal t);

QColor laneColor(int index);
QColor statusColor(char status);
QIcon statusIcon(char status);
QString statusName(char status);

enum class Badge { Head, CurrentBranch, Local, Remote, Tag, Stash };
QColor badgeColor(Badge kind);

struct DiffColors {
    QColor addBg, delBg, addGutter, delGutter, hunkBg, gutterBg, gutterFg, markerBg, oursBg, theirsBg;
};
DiffColors diffColors();

// Monochrome action icon from resources/icons/actions/<name>.svg (Tabler Icons, MIT),
// tinted with the current palette at paint time so it follows light/dark themes.
QIcon actionIcon(const QString& name);
QIcon appIcon();

}  // namespace aslgc::theme
