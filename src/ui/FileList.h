// SPDX-License-Identifier: MIT
#pragma once

#include "git/Types.h"

#include <QListWidget>

namespace aslgc {

// List of changed files: status badge, file name, dimmed directory.
class FileList : public QListWidget {
    Q_OBJECT
public:
    explicit FileList(QWidget* parent = nullptr);

    void setFiles(const QVector<FileChange>& files);
    const QVector<FileChange>& files() const { return m_files; }
    int currentFileIndex() const;
    const FileChange* currentFile() const;
    QVector<FileChange> selectedFiles() const;
    bool selectPath(const QString& path);

signals:
    void fileActivated();  // double click, Enter or Space

protected:
    void keyPressEvent(QKeyEvent* event) override;

private:
    QVector<FileChange> m_files;
};

}  // namespace aslgc
