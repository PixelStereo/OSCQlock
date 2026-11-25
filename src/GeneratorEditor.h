#pragma once
// The generator's timeline in pxs-qt's editor (NyxMap's): the wave or the keys, the duration, the loop, the transport. It edits the
// Device's timeline, and the Device tells the tree what changed: an edit here is an OSC feedback like any other.
#include "Device.h"

#include <pxs/TimelineEditor.h>

class GeneratorEditor : public pxs::timelineview::AnimEditor
{
public:
    GeneratorEditor(Device *device, QWidget *parent = nullptr);
    ~GeneratorEditor() override;

protected:
    bool fetchPlayback(AnimationPlayback *a) const override;
    bool fetch(Animation *a) const override;
    void store(const Animation &before, const Animation &after, const QString &text, const QString &mergeKey) override;
    void control(AnimAction action, double time) override;

private:
    Device *m_device;
    int m_observer = 0;
};
