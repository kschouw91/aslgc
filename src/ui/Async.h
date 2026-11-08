// SPDX-License-Identifier: MIT
#pragma once

#include <QFutureWatcher>
#include <QObject>
#include <QtConcurrent/QtConcurrentRun>

#include <type_traits>
#include <utility>

namespace aslgc {

// Runs `work` on the thread pool and delivers its result to `done` on the thread
// of `context`. If `context` is destroyed first, `done` is never called.
template <typename Work, typename Done>
void runAsync(QObject* context, Work work, Done done) {
    using Result = std::invoke_result_t<Work&>;
    auto* watcher = new QFutureWatcher<Result>(context);
    QObject::connect(watcher, &QFutureWatcherBase::finished, context, [watcher, done = std::move(done)]() mutable {
        done(watcher->result());
        watcher->deleteLater();
    });
    watcher->setFuture(QtConcurrent::run(std::move(work)));
}

}  // namespace aslgc
