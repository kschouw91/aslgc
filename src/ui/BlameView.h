// SPDX-License-Identifier: MIT
#pragma once

#include "git/Types.h"

#include <QPlainTextEdit>
#include <functional>

class QMenu;

namespace aslgc {

class BlameGutter;

// Read-only file view with a blame column: short SHA, author and date at the start of each run
// of lines from one commit, an age bar (newer commits are stronger) and line numbers. Clicking
// an annotation emits commitClicked(); hovering one highlights every line from that commit.
class BlameView : public QPlainTextEdit {
    Q_OBJECT
public:
    explicit BlameView(QWidget* parent = nullptr);

    void setBlame(const Blame& blame, const QString& path);
    void showMessage(const QString& message);
    const Blame& blame() const { return m_blame; }

    // 0-based line indexes into blame().lines.
    int lineAtCursor() const;
    // The selected lines when `line` lies inside the selection, otherwise just `line`.
    void selectedLineRange(int line, int* first, int* last) const;
    int topLine() const;
    void scrollToLine(int line, bool center);
    const BlameCommit* commitOfLine(int line) const;

    // Adds entries for `line` to the gutter and text context menus.
    void setMenuProvider(std::function<void(QMenu& menu, int line)> provider) { m_menuProvider = std::move(provider); }

signals:
    void commitClicked(int line);

protected:
    void resizeEvent(QResizeEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;
    void changeEvent(QEvent* event) override;

private:
    friend class BlameGutter;

    int gutterWidth() const;
    void layoutGutter();
    void paintGutter(QPaintEvent* event);
    int lineAtY(int y) const;
    void setHoverCommit(int commit);
    void gutterMouseMove(QMouseEvent* event);
    void gutterMousePress(QMouseEvent* event);
    void gutterContextMenu(QContextMenuEvent* event);
    bool gutterToolTip(const QPoint& pos, const QPoint& globalPos);
    QColor ageColor(const BlameCommit& commit) const;

    BlameGutter* m_gutter;
    Blame m_blame;
    QString m_path;
    QVector<bool> m_runStart;  // first line of a run attributed to one commit
    qint64 m_oldest = 0;
    qint64 m_newest = 0;
    int m_numberDigits = 3;
    int m_hoverCommit = -1;
    bool m_messageOnly = true;
    std::function<void(QMenu&, int)> m_menuProvider;
};

}  // namespace aslgc
