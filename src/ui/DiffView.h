// SPDX-License-Identifier: MIT
#pragma once

#include "git/Diff.h"

#include <QPlainTextEdit>
#include <QSet>

namespace aslgc {

class DiffGutter;

// Read-only unified diff viewer with old/new line numbers, coloured rows and
// per-hunk action buttons. Selecting lines turns hunk buttons into line actions.
class DiffView : public QPlainTextEdit {
    Q_OBJECT
public:
    enum class Mode { ReadOnly, Unstaged, Staged, Untracked, Conflict };
    enum class Action { Stage, Unstage, Discard };

    explicit DiffView(QWidget* parent = nullptr);

    void setDiff(const FileDiff& diff, Mode mode, bool keepScroll = false);
    void setConflictText(const QByteArray& content, bool keepScroll = false);
    void showMessage(const QString& message);
    void clearView();

    const FileDiff& fileDiff() const { return m_diff; }
    Mode mode() const { return m_mode; }
    // Selected +/- lines as lineKey()s; falls back to `fallbackBlock` without a selection.
    QSet<quint64> selectedChangeLines(int fallbackBlock = -1) const;
    // Read-only diffs offer "History of Selected Lines" in the context menu when enabled.
    void setLineHistoryEnabled(bool enabled) { m_lineHistory = enabled; }

signals:
    void hunkAction(int hunk, aslgc::DiffView::Action action);
    void linesAction(const QSet<quint64>& lines, aslgc::DiffView::Action action);
    // 1-based lines of the new version, or of the old one when only removed lines were picked.
    void lineHistoryRequested(int firstLine, int lastLine, bool oldSide);

protected:
    void resizeEvent(QResizeEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;
    void changeEvent(QEvent* event) override;
    bool viewportEvent(QEvent* event) override;

private:
    friend class DiffGutter;

    struct Row {
        enum Kind : quint8 { Info, Hunk, Context, Added, Removed, NoNewline, Marker, Ours, Theirs };
        Kind kind = Info;
        int hunk = -1;
        int line = -1;
        int oldNo = -1;
        int newNo = -1;
    };
    struct Button {
        QRect rect;
        int hunk = -1;
        Action action = Action::Stage;
        bool lines = false;
        QString label;
    };

    void setRows(const QString& text, QVector<Row> rows, bool keepScroll);
    QVector<Action> availableActions() const;
    QVector<Button> buttons() const;
    QString actionLabel(Action action, bool lines) const;
    int gutterWidth() const;
    void layoutGutter();
    void paintGutter(QPaintEvent* event);
    void trigger(const Button& button);
    bool lineRange(int block, int* first, int* last, bool* oldSide) const;

    DiffGutter* m_gutter;
    FileDiff m_diff;
    Mode m_mode = Mode::ReadOnly;
    QVector<Row> m_rows;
    bool m_messageOnly = true;
    int m_numberDigits = 3;
    int m_hover = -1;
    bool m_lineHistory = false;
};

}  // namespace aslgc
