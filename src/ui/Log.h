// SPDX-License-Identifier: MIT
#pragma once

#include <QString>

namespace aslgc::log {

// Writes Qt messages (info and above) to $XDG_STATE_HOME/aslgc/aslgc.log
// (default ~/.local/state/aslgc/aslgc.log), keeping one rotated copy, while
// still printing them to stderr. Thread-safe; call once after QApplication.
void install();
QString directory();

}  // namespace aslgc::log
