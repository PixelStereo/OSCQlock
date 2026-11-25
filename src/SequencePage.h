#pragma once
// The second tab: the cue list, NyxMap's sequencer (pxs/Sequencer.h). Each step (a row) has a text, a pre-wait after its GO, what it does
// to a timeline (play, pause, stop, rewind, seek, loop mode, speed), a post-wait, and what comes next: Wait (for GO) or Auto follow (the
// next step gets its GO once this one's action and post-wait are over). GO plays the step on standby (the red frame), Back the one
// before; a double-click on a row's number plays that step. The same as /sequence/go, back, stop, stop_all, step.
#include "Device.h"

#include <QWidget>

class QLabel;
class QPushButton;
class QTableWidget;
class QUndoStack;

class SequencePage : public QWidget
{
public:
    SequencePage(Device *device, QUndoStack *undo, QWidget *parent = nullptr);
    ~SequencePage() override;

private:
    Sequencer::Sequence sequence() const;
    void rebuild();      // the rows, from the sequence
    void refresh();      // the step played, the one on standby, the waits on their way
    void store();        // the rows, as edited, to the sequence (an undo step)
    void apply(const Sequencer::Sequence &after, const QString &text);
    void move(int by);   // the selected step up or down
    bool typing() const; // a step being edited: Space and Escape are not GO and Stop all

    Device *m_device;
    QUndoStack *m_undo;
    QTableWidget *m_table;
    QPushButton *m_go;
    QLabel *m_next, *m_state;
    bool m_filling = false;
    int m_listener = 0;
};
