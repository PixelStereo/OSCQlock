#include "GeneratorEditor.h"

#include "Destinations.h"

#include <QCheckBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QMenu>
#include <QPushButton>
#include <QTabBar>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

using pxs::timelineview::TrackRow;

namespace {

// A track: on or off, what it drives (OSCQlock's own output, or a number of a device: a menu of the devices, each a menu of its numbers),
// delete. Then pxs-qt's settings (keys or a wave, its filters) and its lane
class TargetRow : public TrackRow
{
public:
    TargetRow(TimelineEditor *editor, int index) : TrackRow(editor, index), m_tl(editor) {}

    void fill() override
    {
        m_filling = true;
        const AnimTrack &t = track();
        m_on->setChecked(t.enabled);
        Destinations *d = m_tl->device()->destinations();
        const Destination *dest = d && t.layer ? d->find(t.layer) : nullptr;
        QString where = t.layer == 0 ? QStringLiteral("OSCQlock") : dest ? dest->name : QStringLiteral("(a device not here)");
        if (dest && !dest->online) where += QStringLiteral(" (offline)");
        m_target->setText(t.param.isEmpty() ? QStringLiteral("(choose what it drives)")
                                            : QStringLiteral("%1  ›  %2").arg(where, t.layer == 0 ? QStringLiteral("Output") : t.param));
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
        g->addWidget(m_on, 0, 0);
        g->addWidget(m_target, 0, 1, 1, 2);
        g->addWidget(del, 0, 3);
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
        const auto all = d ? d->all() : std::vector<const Destination *>{};
        if (all.empty()) menu->addAction(QStringLiteral("No device yet: open Devices… to find or make one"))->setEnabled(false);
        for (const Destination *dest : all) {
            const QString kind = dest->kind == Destination::Kind::Osc ? QStringLiteral("OSC") : QStringLiteral("OSCQuery");
            QMenu *sub = menu->addMenu(QStringLiteral("%1   (%2%3)").arg(dest->name, kind, dest->online ? QString() : QStringLiteral(", offline")));
            const std::vector<ParamInfo> params = d->params(dest->id);
            if (params.empty()) {
                sub->addAction(dest->kind == Destination::Kind::Osc ? QStringLiteral("No address: declare them in Devices…")
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
        t.keys = {AnimKey{0, v, 3}};
        m_host->waveAround(holder, path, &t.center, &t.amplitude);
        fill();
        changed();
    }

    TimelineEditor *m_tl;
    QCheckBox *m_on = nullptr;
    QPushButton *m_target = nullptr;
};

} // namespace

// ---- the editor of one timeline

TimelineEditor::TimelineEditor(Device *device, QWidget *parent)
    : pxs::timelineview::AnimEditor(device, nullptr, Layout::Side, true, parent), m_device(device)
{
    auto *bottom = new QHBoxLayout;
    auto *add = new QPushButton(QStringLiteral("+ Track"));
    add->setObjectName(QStringLiteral("accent"));
    add->setToolTip(QStringLiteral("A track more: a sine on OSCQlock's own output, to point at what it should drive"));
    bottom->addWidget(add);
    auto *how = new QLabel(QStringLiteral("Curve lane: click or draw keys · double-click deletes · right-click for the curve and the value.  "
                                          "Each track: what it drives (OSCQlock, or a device of Devices…), keys or a wave, and its filters."));
    how->setWordWrap(true);
    how->setObjectName(QStringLiteral("caption"));
    bottom->addWidget(how, 1);
    mainLayout()->addLayout(bottom);
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
        if (isCommitting() || node.kind() != pxs::core::tree::Kind::Parameter || !m_id) return;
        const Device::Timeline *t = m_device->timeline(m_id);
        if (t && node.path().rfind("/generator/" + t->slug + "/", 0) == 0) reload();
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

void TimelineEditor::store(const Animation &, const Animation &after, const QString &, const QString &)
{
    m_device->setTimeline(m_id, after);
}

void TimelineEditor::control(AnimAction action, double time)
{
    m_device->control(m_id, action, time);
}

TrackRow *TimelineEditor::makeRow(int index) { return new TargetRow(this, index); }

// ---- the page: the sub-tabs and the editor

GeneratorPage::GeneratorPage(Device *device, QWidget *parent) : QWidget(parent), m_device(device)
{
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 6, 0, 0);
    outer->setSpacing(6);

    auto *bar = new QHBoxLayout;
    bar->setSpacing(4);
    m_tabs = new QTabBar;
    m_tabs->setObjectName(QStringLiteral("timelines"));
    m_tabs->setDocumentMode(true);
    m_tabs->setExpanding(false);
    m_tabs->setUsesScrollButtons(true);
    m_tabs->setToolTip(QStringLiteral("The timelines (/generator/shown). Double-click: rename"));
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
    bar->addWidget(m_tabs, 1);
    auto *add = new QToolButton;
    add->setText(QStringLiteral("+"));
    add->setObjectName(QStringLiteral("newTimeline"));
    add->setToolTip(QStringLiteral("A new timeline (/generator/new)"));
    bar->addWidget(add);
    m_status = new QLabel;
    m_status->setObjectName(QStringLiteral("caption"));
    bar->addWidget(m_status);
    outer->addLayout(bar);

    m_editor = new TimelineEditor(device);
    outer->addWidget(m_editor, 1);

    connect(add, &QToolButton::clicked, this, [this] { m_device->show(m_device->addTimeline()); });
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
        if (ok) m_device->renameTimeline(id, name);
    });

    m_listener = device->addTimelinesListener([this] { sync(); });
    sync();
}

// A step of the device: the playhead, ▶ on the sub-tabs of the timelines that play, what was sent
void GeneratorPage::tick()
{
    m_editor->poll();
    // What each timeline does, on its sub-tab: ▶ while it plays
    for (int i = 0; i < m_tabs->count(); ++i)
        if (const Device::Timeline *t = m_device->timeline(m_tabs->tabData(i).toULongLong())) {
            const QString text = (t->anim.state == AnimState::Playing ? QStringLiteral("▶  ") : QString()) + t->anim.name;
            if (m_tabs->tabText(i) != text) m_tabs->setTabText(i, text);
        }
    if (Destinations *d = m_device->destinations()) m_status->setText(QStringLiteral("%1 sent").arg(d->sent()));
}

GeneratorPage::~GeneratorPage()
{
    m_device->forgetTimelinesListener(m_listener);
    m_device->onTick(nullptr);
}

void GeneratorPage::sync()
{
    m_syncing = true;
    const auto &all = m_device->timelines();
    while (m_tabs->count() > int(all.size())) m_tabs->removeTab(m_tabs->count() - 1);
    for (size_t i = 0; i < all.size(); ++i) {
        if (int(i) >= m_tabs->count()) m_tabs->addTab(QString());
        m_tabs->setTabText(int(i), all[i].anim.name);
        m_tabs->setTabData(int(i), QVariant::fromValue<qulonglong>(all[i].anim.id));
        m_tabs->setTabToolTip(int(i), QStringLiteral("/generator/%1  ·  double-click to rename").arg(QString::fromStdString(all[i].slug)));
        // A small ✕ to remove it (/generator/<slug>/remove); the last one stays, without one
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
            QTimer::singleShot(0, this, [this, id] { m_device->removeTimeline(id); });
        });
        close->setVisible(all.size() > 1);
    }
    const quint64 shown = m_device->shown();
    for (int i = 0; i < m_tabs->count(); ++i)
        if (m_tabs->tabData(i).toULongLong() == shown) m_tabs->setCurrentIndex(i);
    m_syncing = false;
    m_editor->setTimeline(shown);
}
