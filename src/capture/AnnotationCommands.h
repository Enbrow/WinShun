#pragma once

#include "Annotation.h"

#include <QList>
#include <QPoint>
#include <QRect>
#include <QRectF>
#include <QString>

#include <memory>
#include <optional>
#include <vector>

// Undo and redo by differences (docs/design-capture-roadmap.md, 3.7). The
// document changes only through commands: each keeps what it needs to go
// both ways and reports what it touched, so the canvas redraws just those
// tiles and the compositor drops just those caches. A gesture (drawing,
// dragging) builds one command at the press, reverts it, changes it and
// applies it again at every move, and hands it to pushApplied() at the
// release; Esc reverts it and throws it away.
namespace ws::capture::annot {

struct Change {
    QList<quint32> ids; // the items that changed: their caches go
    QRectF dirty; // what to redraw (image px): the old bounds united with the new
    // The same area in pieces, when that is much less than `dirty`: a big
    // rectangle with no fill is only its edge, so dragging one redraws four
    // strips, not every tile under it. Empty: `dirty` as it is. Always
    // covers what `dirty` needs to, never less.
    QList<QRectF> parts;
    // The pieces to redraw: `parts`, else `dirty` (nothing when that is empty).
    QList<QRectF> areas() const;
    bool numbers = false; // what Numbers show may have changed; dirty covers all of them
    bool redactions = false; // the union of the redactions changed: every redaction is
                             // computed again, and dirty covers all of them
    bool crop = false;
    // Adds `other`: ids joined, regions united, flags or-ed.
    void merge(const Change& other);
};

class Command {
public:
    virtual ~Command() = default;
    virtual Change apply(Document& doc) = 0; // the first time, and redo
    virtual Change revert(Document& doc) = 0; // undo
    // Takes `next` (pushed right after this one) into this one; only commands
    // of the same kind on the same items do, within CommandStack::kMergeMs.
    virtual bool absorb(const Command& next)
    {
        Q_UNUSED(next);
        return false;
    }
    QString label; // the hint's "撤销 移动"
};

// New items, their ids and seqs given (the document's nextId and nextSeq,
// counting up): apply moves the counters past them, revert puts the counters
// back, so undoing everything gives the document it started from. An
// undone item's id can come back for a new item only after the undone step
// was dropped from the stack, so one id never names two items that could
// both be brought back. `at` is the first item's index (-1: the top of its
// segment), kept in its segment.
class AddItems final : public Command {
public:
    explicit AddItems(QList<Item> items, int at = -1);
    Change apply(Document& doc) override;
    Change revert(Document& doc) override;
    QList<Item> items;
    int at = -1;

private:
    quint32 m_nextId = 0;
    quint32 m_nextSeq = 0;
};

// Removes the items; apply records each one with its index, and revert puts
// them back there.
class RemoveItems final : public Command {
public:
    explicit RemoveItems(QList<quint32> ids);
    Change apply(Document& doc) override;
    Change revert(Document& doc) override;
    QList<quint32> ids;
    QList<Item> items; // as removed, by index
    QList<int> at;
};

// Moves the items by whole pixels. `merge` false keeps it a step of its own
// (a mouse drag); the arrow keys leave it true, so a run of presses is one
// step.
class MoveItems final : public Command {
public:
    MoveItems(QList<quint32> ids, QPoint delta);
    Change apply(Document& doc) override;
    Change revert(Document& doc) override;
    bool absorb(const Command& next) override;
    QList<quint32> ids;
    QPoint delta;
    bool merge = true;
};

// Replaces an item (its points, through reshaped(), or a pen stroke as it
// grows) with `after`, whose id is the same.
class Reshape final : public Command {
public:
    Reshape(quint32 id, Item before, Item after);
    Reshape(const Document& doc, Item after); // before: the document's item
    Change apply(Document& doc) override;
    Change revert(Document& doc) override;
    bool absorb(const Command& next) override;
    quint32 id = 0;
    Item before;
    Item after;
    bool merge = true;

private:
    Change put(Document& doc, const Item& item) const;
};

// New styles, one per item, in the order of `ids`.
class Restyle final : public Command {
public:
    Restyle(QList<quint32> ids, QList<Style> before, QList<Style> after);
    Restyle(const Document& doc, QList<quint32> ids, QList<Style> after); // before: the document's
    Change apply(Document& doc) override;
    Change revert(Document& doc) override;
    bool absorb(const Command& next) override;
    QList<quint32> ids;
    QList<Style> before;
    QList<Style> after;
    bool merge = true;

private:
    Change put(Document& doc, const QList<Style>& styles) const;
};

class Retext final : public Command {
public:
    Retext(quint32 id, QString before, QString after);
    Retext(const Document& doc, quint32 id, QString after);
    Change apply(Document& doc) override;
    Change revert(Document& doc) override;
    quint32 id = 0;
    QString before;
    QString after;

private:
    Change put(Document& doc, const QString& text) const;
};

// A Number's own number (empty: counted again).
class Renumber final : public Command {
public:
    Renumber(quint32 id, std::optional<int> before, std::optional<int> after);
    Renumber(const Document& doc, quint32 id, std::optional<int> after);
    Change apply(Document& doc) override;
    Change revert(Document& doc) override;
    quint32 id = 0;
    std::optional<int> before;
    std::optional<int> after;

private:
    Change put(Document& doc, std::optional<int> pinned) const;
};

// Moves an item to index `to` of the list without it (-1: the top of its
// segment), kept in its segment; apply records where it was (`from`).
class Reorder final : public Command {
public:
    Reorder(quint32 id, int to);
    Change apply(Document& doc) override;
    Change revert(Document& doc) override;
    quint32 id = 0;
    int from = -1;
    int to = -1;
};

// A series made (before empty), changed (its start) or removed (after
// empty), and the current series before and after (0: left as it is).
class SetSeries final : public Command {
public:
    SetSeries(quint32 id, std::optional<Series> before, std::optional<Series> after, quint32 currentBefore,
        quint32 currentAfter);
    // "另起一组": a new series counting from 1, current from now on.
    static std::unique_ptr<SetSeries> newSeries(const Document& doc);
    // "起始值": where series `id` starts counting.
    static std::unique_ptr<SetSeries> setStart(const Document& doc, quint32 id, int start);
    Change apply(Document& doc) override;
    Change revert(Document& doc) override;
    quint32 id = 0;
    std::optional<Series> before;
    std::optional<Series> after;
    quint32 currentBefore = 0;
    quint32 currentAfter = 0;

private:
    Change put(Document& doc, const std::optional<Series>& value, quint32 current) const;
};

// What is exported; nothing is redrawn for it (Change::crop).
class SetCrop final : public Command {
public:
    SetCrop(QRect before, QRect after);
    SetCrop(const Document& doc, QRect after);
    Change apply(Document& doc) override;
    Change revert(Document& doc) override;
    QRect before;
    QRect after;
};

// Several commands as one step (smart redaction adds many boxes at once,
// deleting a selection of several items); applied in order, reverted in
// reverse. The label is the first step's unless set.
class Batch final : public Command {
public:
    explicit Batch(std::vector<std::unique_ptr<Command>> steps);
    Change apply(Document& doc) override;
    Change revert(Document& doc) override;
    std::vector<std::unique_ptr<Command>> steps;
};

class CommandStack {
public:
    static constexpr int kLimit = 200;
    static constexpr qint64 kMergeMs = 700;
    // Applies `command` and keeps it; within kMergeMs of the last push it may
    // be absorbed into the step before (the same items moved, restyled or
    // reshaped again). Pushing drops the steps undone before it.
    Change push(std::unique_ptr<Command> command, Document& doc, qint64 nowMs);
    // The same for a command that is applied already (a gesture's, at the
    // release).
    void pushApplied(std::unique_ptr<Command> command, qint64 nowMs);
    Change undo(Document& doc);
    Change redo(Document& doc);
    bool canUndo() const;
    bool canRedo() const;
    void clear();
    int count() const; // the steps kept, done and undone
    int position() const; // the steps done
    QString undoLabel() const;
    QString redoLabel() const;

private:
    void keep(std::unique_ptr<Command> command, qint64 nowMs);
    std::vector<std::unique_ptr<Command>> m_steps;
    int m_done = 0;
    qint64 m_lastMs = 0;
    // Only the step pushed last takes in the next one: never one brought
    // back by undo or redo.
    bool m_mergeable = false;
};
}
