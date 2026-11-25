#pragma once
// What the window does, as undo steps (Cmd/Ctrl+Z, Shift+Cmd/Ctrl+Z): a timeline edited in the editor (a drag, the arrows of a number
// merged into one step), a timeline made, removed or renamed, the sequence edited. What OSC writes is not undone: it is the show playing.
#include "Device.h"

#include <QUndoCommand>

namespace cmd {

// A timeline as the editor left it
class SetTimeline : public QUndoCommand
{
public:
    SetTimeline(Device *device, quint64 id, const Animation &before, const Animation &after, const QString &text, const QString &mergeKey)
        : QUndoCommand(text), m_device(device), m_id(id), m_before(before), m_after(after), m_key(mergeKey)
    {
    }
    void redo() override { m_device->setTimeline(m_id, m_after); }
    void undo() override { m_device->setTimeline(m_id, m_before); }
    int id() const override { return m_key.isEmpty() ? -1 : 1; }
    bool mergeWith(const QUndoCommand *other) override
    {
        const auto *o = static_cast<const SetTimeline *>(other);
        if (o->m_id != m_id || o->m_key != m_key) return false;
        m_after = o->m_after;
        return true;
    }

private:
    Device *m_device;
    quint64 m_id;
    Animation m_before, m_after;
    QString m_key;
};

// A timeline made (redo) or removed (undo), or the other way round: its whole self kept, at its place
class TimelineThere : public QUndoCommand
{
public:
    TimelineThere(Device *device, const Animation &timeline, int index, bool made)
        : QUndoCommand(made ? QStringLiteral("New Timeline") : QStringLiteral("Remove Timeline")), m_device(device), m_timeline(timeline),
          m_index(index), m_made(made)
    {
    }
    void redo() override { m_made ? put() : take(); }
    void undo() override { m_made ? take() : put(); }

private:
    void put() { m_device->restoreTimeline(m_timeline, m_index); }
    void take()
    {
        if (const Device::Timeline *t = m_device->timeline(m_timeline.id)) m_timeline = t->anim; // as it is now, to put it back so
        m_device->removeTimeline(m_timeline.id);
    }
    Device *m_device;
    Animation m_timeline;
    int m_index;
    bool m_made;
};

class RenameTimeline : public QUndoCommand
{
public:
    RenameTimeline(Device *device, quint64 id, const QString &before, const QString &after)
        : QUndoCommand(QStringLiteral("Rename Timeline")), m_device(device), m_id(id), m_before(before), m_after(after)
    {
    }
    void redo() override { m_device->renameTimeline(m_id, m_after); }
    void undo() override { m_device->renameTimeline(m_id, m_before); }

private:
    Device *m_device;
    quint64 m_id;
    QString m_before, m_after;
};

// A sequence as it was edited (a step made, removed, moved, its waits or what it does)
class SetSequence : public QUndoCommand
{
public:
    SetSequence(Device *device, int index, const Sequencer::Sequence &before, const Sequencer::Sequence &after, const QString &text)
        : QUndoCommand(text), m_device(device), m_index(index), m_before(before), m_after(after)
    {
    }
    void redo() override { m_device->sequencer().setSequence(m_index, m_after); }
    void undo() override { m_device->sequencer().setSequence(m_index, m_before); }

private:
    Device *m_device;
    int m_index;
    Sequencer::Sequence m_before, m_after;
};

} // namespace cmd
