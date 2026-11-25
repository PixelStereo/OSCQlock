#include "GeneratorEditor.h"

GeneratorEditor::GeneratorEditor(Device *device, QWidget *parent)
    : pxs::timelineview::AnimEditor(device, nullptr, Layout::Stacked, false, parent), m_device(device)
{
    reload();
    // What OSC or the parameters change is shown at once; the playhead moves with the generator's steps
    m_observer = device->tree().addObserver([this](const pxs::core::tree::Node &node, const pxs::core::ValueList &) {
        if (!isCommitting() && node.kind() == pxs::core::tree::Kind::Parameter && node.path().rfind("/generator/", 0) == 0) reload();
    });
    device->onTick([this] { poll(); });
}

GeneratorEditor::~GeneratorEditor()
{
    m_device->tree().removeObserver(m_observer);
    m_device->onTick(nullptr);
}

bool GeneratorEditor::fetchPlayback(AnimationPlayback *a) const
{
    *a = m_device->playback();
    return true;
}

bool GeneratorEditor::fetch(Animation *a) const
{
    *a = m_device->timeline();
    return true;
}

void GeneratorEditor::store(const Animation &, const Animation &after, const QString &, const QString &)
{
    m_device->setTimeline(after);
}

void GeneratorEditor::control(AnimAction action, double time)
{
    m_device->control(action, time);
}
