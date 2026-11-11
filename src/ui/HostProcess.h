// SPDX-License-Identifier: MIT
#pragma once

#include <QString>
#include <QStringList>

// Starts the user's own programs (terminal, merge tool). Inside the Flatpak sandbox these live on
// the host, so they are looked up and started there through `flatpak-spawn --host`.
namespace aslgc::host {

bool isFlatpak();

// The first of `programs` that is installed, or an empty string. Outside the sandbox this is its
// full path; inside, its name as given (it is resolved on the host when started).
QString findExecutable(const QStringList& programs);

bool startDetached(const QString& program, const QStringList& args, const QString& workDir);

}  // namespace aslgc::host
