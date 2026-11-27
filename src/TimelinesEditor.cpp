#include "TimelinesEditor.h"

#include "Commands.h"
#include "Destinations.h"

#include <QCheckBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPushButton>
#include <QTabBar>
#include <QApplication>
#include <QDrag>
#include <QMimeData>
#include <QMouseEvent>
#include <QTimer>
#include <QToolButton>
#include <QUndoStack>
#include <QVBoxLayout>

using pxs::timelineview::TrackRow;

namespace {

// A track: on or off, what it drives (OSCQlock's own output, or a number of a device: a menu of the devices, each a menu of its numbers),
// delete. Then pxs-qt's settings (keys or a wave, its filters) and its lane
class TargetRow : public TrackRow
{
public:
    TargetRow(TimelineEditor *editor, int index) : TrackRow(editor, index), m_tl(editor) {}

    void setLive(double captured, const std::array<double, 3> &rgb, const std::array<double, 2> &xy, bool running) override
    {
        TrackRow::setLive(captured, rgb, xy, running);
        showOutput();
    }

    void fill() override
    {
        m_filling = true;
        const AnimTrack &t = track();
        m_on->setChecked(t.enabled);
        m_trackName->setText(t.name);
        Destinations *d = m_tl->device()->destinations();
        const pxs::devices::Device *dest = d && t.layer ? d->find(t.layer) : nullptr;
        QString where = t.layer == 0 ? QStringLiteral("OSCQlock") : dest ? QString::fromStdString(dest->name) : QStringLiteral("(a device not here)");
        if (dest && !d->online(dest->id)) where += QStringLiteral(" (offline)");
        m_target->setText(t.param.isEmpty() ? QStringLiteral("(choose what it drives)")
                                            : QStringLiteral("%1  ›  %2").arg(where, t.layer == 0 ? QStringLiteral("Output") : t.param));
        showOutput();
        m_filling = false;
        TrackRow::fill();
    }

protected:
    void buildHeader(QGridLayout *g) override
    {
        m_on = new QCheckBox;
        m_on->setToolTip(QStringLiteral("On: the track drives what it drives while the timeline plays"));
        m_target = new QPushButton;
        m_target->setObjectName(QStringLiteral("trackTarget"));
        m_target->setToolTip(QStringLiteral("What the track drives: OSCQlock's own output, or a number of a device of the network (Devices…)"));
        m_target->setMenu(new QMenu(m_target));
        auto *del = new QPushButton(QStringLiteral("✕"));
        del->setFixedWidth(26);
        del->setToolTip(QStringLiteral("Delete the track"));
        m_trackName = new QLineEdit;
        m_trackName->setMaxLength(32);
        m_trackName->setAccessibleName(QStringLiteral("Track name"));
        g->addWidget(m_on, 0, 0);
        g->addWidget(m_trackName, 0, 1, 1, 2);
        g->addWidget(del, 0, 3);
        g->addWidget(m_target, 1, 1, 1, 3);
        m_output = new QLabel;
        m_output->setWordWrap(true);
        g->addWidget(m_output, 2, 1, 1, 3);
        connect(m_trackName, &QLineEdit::editingFinished, this, [this] {
            const auto name = m_trackName->text().simplified();
            if (!name.isEmpty() && track().name != name) {
                track().name = name;
                changed(false);
            }
            m_trackName->setText(track().name);
        });
        connect(m_on, &QCheckBox::toggled, this, [this](bool on) {
            if (m_filling) return;
            track().enabled = on;
            changed();
        });
        connect(del, &QPushButton::clicked, this, [this] {
            QTimer::singleShot(0, m_tl, [tl = m_tl, i = m_i] { tl->deleteTrack(i); }); // not from inside this row
        });
        connect(m_target->menu(), &QMenu::aboutToShow, this, [this] { fillMenu(); });
    }

private:
    void showOutput()
    {
        if (!m_output) return;
        const auto &target = track();
        const auto *destinations = m_tl->device()->destinations();
        const auto *device = destinations ? destinations->find(target.layer) : nullptr;
        using pxs::devices::Kind;
        const auto *address = destinations ? destinations->scaledAddress(target.layer, target.param) : nullptr;
        m_output->setVisible(address != nullptr);
        if (!address) return;
        QString route, wire;
        const int maximum = pxs::core::device::nativeMaximum(device->kind, *address);
        if (device->kind == Kind::Dmx) {
            const auto [channel, width] = pxs::core::device::dmxChannels(*device, *address);
            const auto level = destinations->manager().dmxLevel(target.layer, target.param);
            route = !channel ? QStringLiteral("Invalid DMX patch")
                : width == 2 ? QStringLiteral("DMX U%1 ch %2–%3").arg(device->universe).arg(channel).arg(channel + 1)
                             : QStringLiteral("DMX U%1 ch %2").arg(device->universe).arg(channel);
            wire = level ? QString::number(*level) : QStringLiteral("—");
        } else {
            using pxs::core::midi::Kind;
            const auto message = destinations->manager().midiOutput(target.layer, target.param);
            const auto channel = QStringLiteral("MIDI ch %1 · ").arg(device->midiChannel);
            switch (address->message) {
            case Kind::ControlChange: route = channel + QStringLiteral("CC %1").arg(address->number); break;
            case Kind::Note: route = channel + QStringLiteral("Note %1").arg(address->number); break;
            case Kind::PitchBend: route = channel + QStringLiteral("Pitch bend"); break;
            case Kind::ProgramChange: route = channel + QStringLiteral("Program Change"); break;
            }
            wire = QStringLiteral("—");
            if (message.size() >= 2) {
                if (address->message == Kind::PitchBend && message.size() >= 3) wire = QString::number(int(message[1]) | (int(message[2]) << 7));
                else if (address->message == Kind::ProgramChange) wire = QString::number(message[1]);
                else if (message.size() >= 3) wire = QString::number(message[2]);
            }
        }
        const QString encoded = device->kind == Kind::Midi && address->message == pxs::core::midi::Kind::ProgramChange
            ? QStringLiteral("Program %1").arg(address->number) : QStringLiteral("0–%1").arg(maximum);
        const QString summary = QStringLiteral("%1\nOut %2 · %3").arg(route, wire, encoded);
        if (m_output->text() != summary) m_output->setText(summary);
        m_output->setToolTip(QStringLiteral("Out shows the encoded value after the track's filters."));
    }

    void fillMenu()
    {
        QMenu *menu = m_target->menu();
        menu->clear();
        const AnimTrack &t = track();
        QAction *own = menu->addAction(QStringLiteral("OSCQlock  ›  Output (its own signal)"), this, [this] { select(0, QStringLiteral("output")); });
        own->setCheckable(true);
        own->setChecked(t.layer == 0 && t.param == QLatin1String("output"));
        menu->addSeparator();
        Destinations *d = m_tl->device()->destinations();
        const auto all = d ? d->all() : std::vector<const pxs::devices::Device *>{};
        if (all.empty()) menu->addAction(QStringLiteral("No device yet: open Devices… to find or make one"))->setEnabled(false);
        for (const pxs::devices::Device *dest : all) {
            const QString kind = QString::fromLatin1(pxs::core::device::name(dest->kind));
            QMenu *sub = menu->addMenu(QStringLiteral("%1   (%2%3)").arg(QString::fromStdString(dest->name), kind, d->online(dest->id) ? QString() : QStringLiteral(", offline")));
            const std::vector<ParamInfo> params = d->params(dest->id);
            if (params.empty()) {
                sub->addAction(dest->kind == pxs::devices::Kind::Osc ? QStringLiteral("No address: declare them in Devices…")
                                                                    : QStringLiteral("Nothing to drive (its namespace is not read yet)"))
                    ->setEnabled(false);
                continue;
            }
            const quint64 id = dest->id;
            pxs::timelineview::addParamMenu(sub, params, [&](QMenu *into, const ParamInfo &p, const QString &text) {
                QAction *a = into->addAction(text, this, [this, id, path = p.path] { select(id, path); });
                a->setCheckable(true);
                a->setChecked(t.layer == id && t.param == p.path);
            });
        }
    }

    void select(quint64 holder, const QString &path)
    {
        AnimTrack &t = track();
        t.layer = holder;
        t.param = path;
        t.color = t.event = false;
        t.axes.clear();
        double v = 0.5;
        m_host->animParamValue(holder, path, &v);
        m_host->waveAround(holder, path, &t.center, &t.amplitude);
        t.keys = {AnimKey{0, v, 3}};
        fill();
        changed();
    }

    TimelineEditor *m_tl;
    QCheckBox *m_on = nullptr;
    QLineEdit *m_trackName = nullptr;
    QLabel *m_output = nullptr;
    QPushButton *m_target = nullptr;
};

} // namespace

// ---- the editor of one timeline

TimelineEditor::TimelineEditor(Device *device, QUndoStack *undo, QWidget *parent)
    : pxs::timelineview::AnimEditor(device, undo, Layout::Side, true, parent), m_device(device)
{
    auto *controls = timelineControlsLayout();
    auto *panel = controls->parentWidget();
    panel->setObjectName(QStringLiteral("timelineControls"));
    panel->setStyleSheet(QStringLiteral(
        "QWidget#timelineControls { border: 1px solid #43464e; border-radius: 7px; }"));
    controls->setContentsMargins(10, 8, 10, 8);

    auto *add = new QPushButton(QStringLiteral("+ Track"));
    add->setObjectName(QStringLiteral("accent"));
    add->setAccessibleName(QStringLiteral("Add track"));
    add->setToolTip(QStringLiteral("Add a track to this timeline"));
    timelineTransportLayout()->addWidget(add);
    connect(add, &QPushButton::clicked, this, [this] {
        if (!m_has) return;
        AnimTrack t;
        t.layer = 0;
        t.param = QStringLiteral("output");
        t.oscillator = true;
        t.period = 1;
        t.center = t.amplitude = 0.5;
        t.keys = {AnimKey{0, 0.5, 3}};
        m_edit.tracks.push_back(t);
        commit(QStringLiteral("Add Track"));
        rebuildRows();
        applyView();
    });

    // What OSC or the parameters change is shown at once; the playhead moves with the device's steps
    m_observer = device->tree().addObserver([this](const pxs::core::tree::Node &node, const pxs::core::ValueList &) {
        if (isCommitting() || node.kind() != pxs::core::tree::Kind::Parameter || node.access != 3 || !m_id) return;
        const Device::Timeline *t = m_device->timeline(m_id);
        if (!t) return;
        const auto prefix = "/timelines/" + t->slug + "/";
        if (node.path().rfind(prefix, 0) == 0 && node.path().rfind(prefix + "transport/", 0) != 0) reload();
    });
}

TimelineEditor::~TimelineEditor()
{
    m_device->tree().removeObserver(m_observer);
}

void TimelineEditor::setTimeline(quint64 id)
{
    if (id == m_id) return;
    m_id = id;
    forgetView();
    forgetParams();
    reload();
}

void TimelineEditor::deleteTrack(int index)
{
    if (index < 0 || index >= int(m_edit.tracks.size())) return;
    m_edit.tracks.erase(m_edit.tracks.begin() + index);
    if (index < int(m_laneViews.size())) m_laneViews.erase(m_laneViews.begin() + index);
    commit(QStringLiteral("Delete Track"));
    rebuildRows();
    applyView();
}

bool TimelineEditor::fetchPlayback(AnimationPlayback *a) const
{
    if (!m_device->timeline(m_id)) return false;
    *a = m_device->playback(m_id);
    return true;
}

bool TimelineEditor::fetch(Animation *a) const
{
    const Device::Timeline *t = m_device->timeline(m_id);
    if (!t) return false;
    *a = t->anim;
    return true;
}

void TimelineEditor::store(const Animation &before, const Animation &after, const QString &text, const QString &mergeKey)
{
    if (m_undo) m_undo->push(new cmd::SetTimeline(m_device, m_id, before, after, text, mergeKey));
    else m_device->setTimeline(m_id, after);
    const auto *stored = m_device->timeline(m_id);
    if (!stored || stored->anim.tracks.size() != m_edit.tracks.size()) return;
    for (size_t i = 0; i < m_edit.tracks.size(); ++i) {
        m_edit.tracks[i].name = stored->anim.tracks[i].name;
        m_edit.tracks[i].slug = stored->anim.tracks[i].slug;
    }
}

void TimelineEditor::control(AnimAction action, double time)
{
    m_device->control(m_id, action, time);
}

QString TimelineEditor::trackLabel(const AnimTrack &track) const
{
    return track.name;
}

TrackRow *TimelineEditor::makeRow(int index) { return new TargetRow(this, index); }

// ---- the page: the sub-tabs and the editor

namespace {
class TimelineTabs : public QTabBar {
protected:
    void mousePressEvent(QMouseEvent *event) override
    {
        m_start = event->position().toPoint();
        QTabBar::mousePressEvent(event);
    }
    void mouseMoveEvent(QMouseEvent *event) override
    {
        const int tab = tabAt(m_start);
        if (tab >= 0 && (event->buttons() & Qt::LeftButton) &&
            (m_start - event->position().toPoint()).manhattanLength() >= QApplication::startDragDistance()) {
            const quint64 id = tabData(tab).toULongLong();
            if (id) {
                auto *mime = new QMimeData;
                mime->setData("application/x-pxs-timeline", QByteArray::number(id));
                QDrag drag(this);
                drag.setMimeData(mime);
                drag.exec(Qt::CopyAction);
                return;
            }
        }
        QTabBar::mouseMoveEvent(event);
    }
private:
    QPoint m_start;
};
}

TimelinesPage::TimelinesPage(Device *device, QUndoStack *undo, QWidget *parent) : QWidget(parent), m_device(device), m_undo(undo)
{
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 6, 0, 0);
    outer->setSpacing(6);

    auto *bar = new QHBoxLayout;
    bar->setSpacing(4);
    m_tabs = new TimelineTabs;
    m_tabs->setObjectName(QStringLiteral("timelines"));
    m_tabs->setDocumentMode(true);
    m_tabs->setExpanding(false);
    m_tabs->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
    m_tabs->setUsesScrollButtons(true);
    m_tabs->setToolTip(QStringLiteral("The timelines (/timelines/shown). Double-click: rename"));
    m_tabs->setDrawBase(false);
    setStyleSheet(QStringLiteral(
        "QTabBar#timelines::tab { background: #24262b; color: #a9adb6; border: 1px solid #33363d; border-radius: 13px; padding: 4px 10px 4px 14px;"
        " margin-right: 6px; min-height: 18px; }"
        "QTabBar#timelines::tab:selected { background: #ff8a3d; color: #17181b; border-color: #ff8a3d; font-weight: 600; }"
        "QTabBar#timelines::tab:hover:!selected { color: #e6e8ec; border-color: #4a4e57; }"
        "QToolButton#newTimeline { background: #2b2e34; color: #ff8a3d; border: 1px solid #ff8a3d; border-radius: 13px; font-size: 16px;"
        " font-weight: 700; min-width: 26px; min-height: 26px; }"
        "QToolButton#newTimeline:hover { background: #ff8a3d; color: #17181b; }"
        "QToolButton#closeTimeline { border: none; background: transparent; color: inherit; font-size: 14px; padding: 0 0 1px 4px; }"
        "QToolButton#closeTimeline:hover { color: #ffffff; }"));
    bar->addWidget(m_tabs);
    auto *add = new QToolButton;
    add->setText(QStringLiteral("+"));
    add->setObjectName(QStringLiteral("newTimeline"));
    add->setAccessibleName(QStringLiteral("Add timeline"));
    add->setToolTip(QStringLiteral("Add a timeline"));
    bar->addWidget(add);
    bar->addStretch();
    outer->addLayout(bar);

    m_editor = new TimelineEditor(device, undo);
    outer->addWidget(m_editor, 1);

    connect(add, &QToolButton::clicked, this, [this] {
        const quint64 id = m_device->addTimeline();
        const auto &all = m_device->timelines();
        const auto at = std::find_if(all.begin(), all.end(), [id](const Device::Timeline &t) { return t.anim.id == id; });
        if (at != all.end()) m_undo->push(new cmd::TimelineThere(m_device, *at, int(at - all.begin()), true));
        m_device->show(id);
    });
    connect(m_tabs, &QTabBar::tabBarClicked, this, [this](int index) {
        const auto id = m_tabs->tabData(index).toULongLong();
        if (id && m_inspectorSink) m_inspectorSink(id);
    });
    connect(m_tabs, &QTabBar::currentChanged, this, [this](int index) {
        if (m_syncing || index < 0) return;
        m_device->show(m_tabs->tabData(index).toULongLong());
    });
    connect(m_tabs, &QTabBar::tabBarDoubleClicked, this, [this](int index) {
        if (index < 0) return;
        const quint64 id = m_tabs->tabData(index).toULongLong();
        const Device::Timeline *t = m_device->timeline(id);
        if (!t) return;
        bool ok = false;
        const QString name = QInputDialog::getText(this, QStringLiteral("Rename"), QStringLiteral("Name of the timeline"), QLineEdit::Normal, t->anim.name, &ok);
        if (ok && !name.trimmed().isEmpty() && name.trimmed() != t->anim.name)
            m_undo->push(new cmd::RenameTimeline(m_device, id, t->anim.name, name.trimmed()));
    });

    m_listener = device->addTimelinesListener([this] { sync(); });
    sync();
}

// Refresh the playhead and the playing indicator on each tab.
void TimelinesPage::tick()
{
    m_editor->poll();
    // What each timeline does, on its sub-tab: ▶ while it plays
    for (int i = 0; i < m_tabs->count(); ++i)
        if (const Device::Timeline *t = m_device->timeline(m_tabs->tabData(i).toULongLong())) {
            const QString text = (t->anim.state == AnimState::Playing ? QStringLiteral("▶  ") : QString()) + t->anim.name;
            if (m_tabs->tabText(i) != text) m_tabs->setTabText(i, text);
        }
}

TimelinesPage::~TimelinesPage()
{
    m_device->forgetTimelinesListener(m_listener);
    m_device->onTick(nullptr);
}

void TimelinesPage::sync()
{
    m_syncing = true;
    const auto &all = m_device->timelines();
    while (m_tabs->count() > int(all.size())) m_tabs->removeTab(m_tabs->count() - 1);
    for (size_t i = 0; i < all.size(); ++i) {
        if (int(i) >= m_tabs->count()) m_tabs->addTab(QString());
        m_tabs->setTabText(int(i), all[i].anim.name);
        m_tabs->setTabData(int(i), QVariant::fromValue<qulonglong>(all[i].anim.id));
        m_tabs->setTabToolTip(int(i), QStringLiteral("/timelines/%1  ·  double-click to rename").arg(QString::fromStdString(all[i].slug)));
        // A small ✕ to remove it (/timelines/<slug>/remove); the last one stays, without one
        auto *close = qobject_cast<QToolButton *>(m_tabs->tabButton(int(i), QTabBar::RightSide));
        if (!close) {
            close = new QToolButton;
            close->setObjectName(QStringLiteral("closeTimeline"));
            close->setText(QStringLiteral("×"));
            close->setToolTip(QStringLiteral("Remove this timeline"));
            close->setAutoRaise(true);
            m_tabs->setTabButton(int(i), QTabBar::RightSide, close);
        }
        close->disconnect(this);
        connect(close, &QToolButton::clicked, this, [this, id = all[i].anim.id] {
            QTimer::singleShot(0, this, [this, id] {
                const auto &all = m_device->timelines();
                const auto at = std::find_if(all.begin(), all.end(), [id](const Device::Timeline &t) { return t.anim.id == id; });
                if (at == all.end() || all.size() <= 1) return;
                m_undo->push(new cmd::TimelineThere(m_device, *at, int(at - all.begin()), false));
            });
        });
        close->setVisible(all.size() > 1);
    }
    const quint64 shown = m_device->shown();
    for (int i = 0; i < m_tabs->count(); ++i)
        if (m_tabs->tabData(i).toULongLong() == shown) m_tabs->setCurrentIndex(i);
    m_syncing = false;
    m_editor->setTimeline(shown);
}
