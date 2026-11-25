#pragma once
// The first tab: the timelines, a sub-tab each (+ makes one, double-click renames, ✕ removes; the sub-tab shown is /generator/shown, so
// OSC moves it too), and the one shown in pxs-qt's editor (NyxMap's): its transport, its passes, and its tracks, each with what it drives
// (OSCQlock's own output, or a number of a device of the network: Destinations.h) and its filters. The editor edits the Device's
// timeline, and the Device tells the tree what changed: an edit here is an OSC feedback like any other.
#include "Device.h"

#include <pxs/TimelineEditor.h>

class QLabel;
class QTabBar;

class TimelineEditor : public pxs::timelineview::AnimEditor
{
public:
    TimelineEditor(Device *device, QWidget *parent = nullptr);
    ~TimelineEditor() override;
    void setTimeline(quint64 id); // the one shown
    quint64 timelineId() const { return m_id; }
    Device *device() const { return m_device; }
    void deleteTrack(int index);

protected:
    bool fetchPlayback(AnimationPlayback *a) const override;
    bool fetch(Animation *a) const override;
    void store(const Animation &before, const Animation &after, const QString &text, const QString &mergeKey) override;
    void control(AnimAction action, double time) override;
    pxs::timelineview::TrackRow *makeRow(int index) override;

private:
    Device *m_device;
    quint64 m_id = 0;
    int m_observer = 0;
};

class GeneratorPage : public QWidget
{
public:
    GeneratorPage(Device *device, QWidget *parent = nullptr);
    ~GeneratorPage() override;

private:
    void sync(); // the sub-tabs from the Device's timelines, the one shown in the editor

    Device *m_device;
    QTabBar *m_tabs;
    QLabel *m_status;
    TimelineEditor *m_editor;
    bool m_syncing = false;
};
