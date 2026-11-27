#include "Device.h"
#include <pxs/PresetView.h>
#include <TimelineTree.h>
#include "Show.h"

#include <pxs/core/tree/parameter.h>

#include "Destinations.h"

#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>
#include <QCoreApplication>
#include <QSettings>

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>

using namespace pxs::core;
using namespace pxs::core::tree;
using CoreParameter = pxs::core::tree::Parameter;

namespace {

constexpr double kTau = 6.283185307179586;

ArgRange range(double lo, double hi)
{
    ArgRange r;
    r.min = Value(lo);
    r.max = Value(hi);
    return r;
}

void assign(double &target, const Value &v) { target = v.number().value_or(target); }
void assign(float &target, const Value &v) { target = float(v.number().value_or(target)); }
void assign(int &target, const Value &v) { target = int(v.number().value_or(target)); }
void assign(bool &target, const Value &v) { target = v.truthy(); }
void assign(std::string &target, const Value &v) { target = v.text(); }

std::size_t arity(const std::string &type) { return type == "N" ? 0 : type.size(); }

} // namespace

Node &Device::group(const char *path, const char *label, const char *description)
{
    m_declared.push_back(path);
    Node &n = m_tree.add(path);
    n.label = label;
    n.description = description;
    return n;
}

// A parameter whose value lives in `ref`
template <class T, class Configure>
Node &Device::bound(const char *path, const char *label, CoreParameter::Type type, T &ref, Configure configure)
{
    const std::string full = path;
    auto p = std::make_unique<CoreParameter>(full.substr(full.rfind('/') + 1), label, type);
    p->byDefault(Value(ref));
    p->bind([&ref] { return Value(ref); }, [&ref](const Value &v) { assign(ref, v); });
    configure(*p);
    m_declared.push_back(path);
    return m_tree.add(path, std::move(p));
}

void Device::action(const char *path, const char *type, const char *label, const char *description, std::function<bool(const ValueList &)> handler)
{
    const std::string full = path;
    const std::size_t needed = arity(type);
    m_declared.push_back(path);
    Node &n = m_tree.addAction(path, type, [this, full, needed, handler = std::move(handler)](const ValueList &v) {
        if (v.size() < needed) return false;
        if (handler && !handler(v)) return false;
        said(full, v);
        return true;
    }, description);
    n.label = label;
}

Node &Device::signal(const char *path, const char *type, const char *label, const char *description, std::vector<ArgRange> ranges,
                     std::vector<std::string> extendedType)
{
    m_declared.push_back(path);
    Node &n = m_tree.addSignal(path, type, description);
    n.label = label;
    n.range = std::move(ranges);
    n.extendedType = std::move(extendedType);
    return n;
}

void Device::said(const std::string &path, const ValueList &args)
{
    std::string text = path;
    for (const Value &v : args) text += ' ' + (v.is<Blob>() ? std::string("<blob>") : v.text());
    m_tree.fire("/timelines/last-action", {Value(text)});
}

Device::Device(QObject *parent, bool persistent) : QObject(parent), m_persistent(persistent), m_sequencer(*this, m_lock, this)
{
    m_tabs = {"timelines", "sequence", "presets"};
    if (m_persistent) {
        m_saveTimer.setSingleShot(true);
        m_saveTimer.setInterval(400);
        connect(&m_saveTimer, &QTimer::timeout, this, [this] { writeSettings(); });
        load();
        connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit, this, &Device::writeSettings);
    }
    buildTimelines();
    buildSequence();
    buildUi();
    m_clock.start();
    connect(&m_timer, &QTimer::timeout, this, [this] { tick(); });
    m_timer.start(20); // 50 Hz
}

// ---- /timelines: the timelines

namespace {

// A track of a sine of a second between 0 and 1 on OSCQlock's own output, as a new timeline starts with
AnimTrack firstTrack()
{
    AnimTrack t;
    t.layer = 0;
    t.param = "output";
    t.oscillator = true;
    t.wave = AnimWave::Sine;
    t.period = 1;
    t.center = 0.5;
    t.amplitude = 0.5;
    t.keys = {AnimKey{0, 0, 3}, AnimKey{2, 1, 3}, AnimKey{4, 0, 3}}; // what Keys starts from
    return t;
}

} // namespace

void Device::buildTimelines()
{
    group("/timelines", "Timelines",
          "The timelines, as NyxMap's: each a sub-tab, its tracks driving OSCQlock's own outputs or the devices of the network, through filters. "
          "A node that holds nodes that hold parameters, actions, signals, or a mix");
    action("/timelines/new", "N", "New timeline", "A timeline more, shown", [this](const ValueList &) {
        show(addTimeline());
        return true;
    });
    action("/timelines/new-named", "s", "New timeline, named", "A timeline more, of that name, shown", [this](const ValueList &v) {
        show(addTimeline(QString::fromStdString(v.at(0).text())));
        return true;
    });
    action("/timelines/remove", "s", "Remove a timeline", "Remove the named timeline (the last one stays)", [this](const ValueList &v) {
        for (const Timeline &t : m_timelines)
            if (t.slug == v.at(0).text()) return removeTimeline(t.anim.id);
        return false;
    });
    signal("/timelines/last-action", "s", "Last action", "The last action received, with its address and arguments");
    if (m_timelines.empty()) addTimeline();
    buildShown();
}

void Device::buildShown()
{
    std::vector<std::string> slugs;
    for (const Timeline &t : m_timelines) slugs.push_back(t.slug);
    if (std::find(slugs.begin(), slugs.end(), s.shown) == slugs.end()) s.shown = slugs.empty() ? std::string() : slugs.front();
    // Already there: its choices are changed where it is (a client that listens to it keeps listening)
    if (Node *shown = m_tree.find("/timelines/shown"); shown && shown->parameter()) {
        shown->parameter()->choices(slugs);
        ArgRange keys;
        for (const std::string &slug : slugs) keys.vals.emplace_back(slug);
        shown->range = {keys};
        m_tree.changed("/timelines/shown");
        return;
    }
    auto p = std::make_unique<CoreParameter>("shown", "Shown", CoreParameter::Type::Choice);
    p->choices(slugs).byDefault(Value(slugs.empty() ? std::string() : slugs.front())).transient();
    p->bind([this] { return Value(s.shown); },
            [this](const Value &v) {
                for (const Timeline &t : m_timelines)
                    if (t.slug == v.text()) {
                        s.shown = t.slug;
                        tellTimelines();
                    }
            });
    Node &n = m_tree.add("/timelines/shown", std::move(p));
    n.description = "The timeline the window shows (its sub-tab), by its slug. A parameter that holds actions";
    auto move = [this](int by) {
        if (m_timelines.empty()) return false;
        int at = 0;
        for (size_t i = 0; i < m_timelines.size(); ++i)
            if (m_timelines[i].slug == s.shown) at = int(i);
        const int n = int(m_timelines.size());
        s.shown = m_timelines[size_t((at + by + n) % n)].slug;
        m_tree.changed("/timelines/shown");
        tellTimelines();
        return true;
    };
    m_tree.addAction("/timelines/shown/next", "N", [move](const ValueList &) { return move(1); }, "The timeline after, round").label = "Next timeline";
    m_tree.addAction("/timelines/shown/previous", "N", [move](const ValueList &) { return move(-1); }, "The timeline before, round").label =
        "Previous timeline";
    m_tree.changed("/timelines/shown");
}

void Device::timelinesChanged()
{
    buildShown();
    tellTimelines();
    save();
}

Device::Timeline *Device::find(quint64 id)
{
    for (Timeline &t : m_timelines)
        if (t.anim.id == id) return &t;
    return nullptr;
}

const Device::Timeline *Device::timeline(quint64 id) const
{
    for (const Timeline &t : m_timelines)
        if (t.anim.id == id) return &t;
    return nullptr;
}

std::string Device::claimTimelineName(const QString &name) const
{
    std::set<std::string> used{"new", "new-named", "remove", "shown", "last-action"};
    for (const auto &timeline : m_timelines) used.insert(timeline.slug);
    return pxs::timeline::claimSegment(name.toStdString(), used);
}

quint64 Device::addTimeline(const QString &name)
{
    Timeline t;
    t.anim.id = m_nextTimeline++;
    t.anim.name = name.trimmed().isEmpty() ? QStringLiteral("Timeline") : name.trimmed();
    t.slug = claimTimelineName(t.anim.name);
    if (name.trimmed().isEmpty() && t.slug != "timeline")
        t.anim.name += QStringLiteral(" ") + QString::fromStdString(t.slug.substr(t.slug.rfind('-') + 1)).toUpper();
    t.anim.duration = 4;
    t.anim.setLoop(AnimLoop::Loop, 0);
    t.anim.tracks = {firstTrack()};
    m_timelines.push_back(std::move(t));
    buildTimeline(m_timelines.back());
    if (m_tree.find("/timelines/shown")) timelinesChanged();
    return m_timelines.back().anim.id;
}

void Device::restoreTimeline(const Animation &timeline, int index)
{
    if (!timeline.id || this->timeline(timeline.id)) return;
    Timeline t;
    t.anim = timeline;
    t.anim.state = AnimState::Stopped;
    t.anim.clock = 0;
    t.slug = claimTimelineName(t.anim.name);
    m_nextTimeline = std::max(m_nextTimeline, t.anim.id + 1);
    const auto at = m_timelines.begin() + std::clamp(index, 0, int(m_timelines.size()));
    buildTimeline(*m_timelines.insert(at, std::move(t)));
    timelinesChanged();
}

bool Device::removeTimeline(quint64 id)
{
    if (m_timelines.size() <= 1) return false;
    const auto it = std::find_if(m_timelines.begin(), m_timelines.end(), [id](const Timeline &t) { return t.anim.id == id; });
    if (it == m_timelines.end()) return false;
    m_tree.remove(base(*it));
    const std::string prefix = ":" + std::to_string(id) + ":";
    for (auto f = m_filters.begin(); f != m_filters.end();)
        f = f->first.contains(QString::fromStdString(prefix)) ? m_filters.erase(f) : std::next(f);
    m_timelines.erase(it);
    timelinesChanged();
    return true;
}

void Device::renameTimeline(quint64 id, const QString &name)
{
    Timeline *t = find(id);
    if (!t || name.trimmed().isEmpty() || t->anim.name == name.trimmed()) return;
    const Said before = said(*t);
    t->anim.name = name.trimmed();
    if (Node *n = m_tree.find(base(*t))) n->label = t->anim.name.toStdString();
    tell(*t, before);
    tellTimelines();
    save();
}

quint64 Device::shown() const
{
    for (const Timeline &t : m_timelines)
        if (t.slug == s.shown) return t.anim.id;
    return m_timelines.empty() ? 0 : m_timelines.front().anim.id;
}

void Device::show(quint64 id)
{
    const Timeline *t = timeline(id);
    if (!t || t->slug == s.shown) return;
    s.shown = t->slug;
    m_tree.changed("/timelines/shown");
    tellTimelines();
}

void Device::buildTimeline(Timeline &tl)
{
    const quint64 id = tl.anim.id;
    const std::string b = base(tl);
    // A parameter of this timeline, read and written through two functions; whatever it changes beside itself is told
    const auto param = [this, id](const std::string &path, const char *label, CoreParameter::Type type, std::function<Value(const Timeline &)> get,
                                  std::function<void(Timeline &, const Value &)> set, std::function<void(CoreParameter &)> configure) -> Node & {
        auto p = std::make_unique<CoreParameter>(path.substr(path.rfind('/') + 1), label, type);
        p->bind([this, id, get = std::move(get)] {
                    const Timeline *t = timeline(id);
                    return t ? get(*t) : Value();
                },
                [this, id, set = std::move(set)](const Value &v) {
                    Timeline *t = find(id);
                    if (!t) return;
                    const Said before = said(*t);
                    set(*t, v);
                    tell(*t, before);
                    save();
                });
        configure(*p);
        return m_tree.add(path, std::move(p));
    };
    const auto act = [this](const std::string &path, const char *type, const char *label, const char *description, std::function<bool(const ValueList &)> run) {
        m_tree.addAction(path, type, [this, path, type, run = std::move(run)](const ValueList &v) {
            if (v.size() < arity(type) || !run(v)) return false;
            said(path, v);
            return true;
        }, description).label = label;
    };

    Node &top = m_tree.add(b);
    top.label = tl.anim.name.toStdString();
    top.description = "A timeline: its transport, its passes, its tracks (each the value it says, after its filters)";
    param(b + "/name", "Name", CoreParameter::Type::Text, [](const Timeline &t) { return Value(t.anim.name.toStdString()); },
          [this](Timeline &t, const Value &v) {
              if (v.text().empty()) return;
              t.anim.name = QString::fromStdString(v.text());
              if (Node *n = m_tree.find(base(t))) n->label = v.text();
              tellTimelines();
          },
          [](CoreParameter &p) { p.byDefault(Value("Timeline")); });

    m_tree.add(b + "/transport").label = "Transport";
    param(b + "/transport/playing", "Playing", CoreParameter::Type::Bool, [](const Timeline &t) { return Value(t.anim.state == AnimState::Playing); },
          [](Timeline &t, const Value &v) { t.anim.control(v.truthy() ? AnimAction::Play : AnimAction::Pause); },
          [](CoreParameter &p) { p.transient().byDefault(Value(false)); }).description = "On: it plays (from where it is). Off: it pauses";
    act(b + "/transport/play", "N", "Play", "Start or resume this timeline", [this, id](const ValueList &) {
        control(id, AnimAction::Play);
        return true;
    });
    act(b + "/transport/pause", "N", "Pause", "Pause this timeline", [this, id](const ValueList &) {
        control(id, AnimAction::Pause);
        return true;
    });
    param(b + "/transport/state", "State", CoreParameter::Type::Choice,
          [](const Timeline &t) {
              return Value(t.anim.state == AnimState::Playing ? "playing" : t.anim.state == AnimState::Paused ? "paused" : "stopped");
          }, [](Timeline &, const Value &) {},
          [](CoreParameter &p) { p.choices({"playing", "paused", "stopped"}).readOnly().transient(); });
    param(b + "/transport/position", "Position", CoreParameter::Type::Float,
          [](const Timeline &t) { return Value(t.anim.position()); },
          [](Timeline &t, const Value &v) {
              t.anim.control(AnimAction::Seek, t.anim.clockAt(std::clamp(v.number().value_or(0.0), 0.0, t.anim.passDuration())));
          }, [](CoreParameter &p) { p.range(0, 3600).byDefault(Value(0.0)).transient().unit("time.second"); });
    param(b + "/transport/played", "Played", CoreParameter::Type::Float,
          [](const Timeline &t) { return Value(t.anim.played()); }, [](Timeline &, const Value &) {},
          [](CoreParameter &p) { p.range(0, 360000).readOnly().transient().unit("time.second"); });
    act(b + "/transport/stop", "N", "Stop", "Stops, back to the start", [this, id](const ValueList &) {
        control(id, AnimAction::Stop);
        return true;
    });
    act(b + "/transport/rewind", "N", "Rewind", "Back to the start of the first pass", [this, id](const ValueList &) {
        control(id, AnimAction::Rewind);
        return true;
    });
    act(b + "/transport/seek", "f", "Seek", "Seek within the current pass, in seconds", [this, id](const ValueList &v) {
        const Timeline *t = timeline(id);
        if (!t) return false;
        control(id, AnimAction::Seek, t->anim.clockAt(std::clamp(v.at(0).number().value_or(0.0), 0.0, t->anim.passDuration())));
        return true;
    });

    m_tree.add(b + "/settings").label = "Settings";
    param(b + "/settings/duration", "Duration", CoreParameter::Type::Float, [](const Timeline &t) { return Value(t.anim.duration); },
          [](Timeline &t, const Value &v) { t.anim.duration = std::clamp(v.number().value_or(4.0), pxs::core::timeline::Transport::kMinDuration, 3600.0); },
          [](CoreParameter &p) { p.range(0.05, 60).limits(0.05, 3600).step(0.05).unit("time.second").byDefault(Value(4.0)); }).description =
        "Seconds of one pass (what the keys are drawn over)";
    param(b + "/settings/loop", "Loop", CoreParameter::Type::Choice, [](const Timeline &t) { return Value(animLoopKey(t.anim.loop).toStdString()); },
          [](Timeline &t, const Value &v) { t.anim.setLoop(animLoopFromKey(QString::fromStdString(v.text())), t.anim.repeat); },
          [](CoreParameter &p) { p.choices({"once", "loop", "pingpong"}).byDefault(Value("loop")); }).description = "Once, in a loop, or back and forth";
    param(b + "/settings/passes", "Passes (0: endless)", CoreParameter::Type::Int, [](const Timeline &t) { return Value(t.anim.repeat); },
          [](Timeline &t, const Value &v) { t.anim.setLoop(t.anim.loop, int(v.number().value_or(0.0))); },
          [](CoreParameter &p) { p.range(0, 100).step(1).byDefault(Value(0)); });
    param(b + "/settings/speed", "Speed", CoreParameter::Type::Float, [](const Timeline &t) { return Value(t.anim.speed); },
          [](Timeline &t, const Value &v) { t.anim.control(AnimAction::Speed, v.number().value_or(1.0)); },
          [](CoreParameter &p) { p.range(0, 10).step(0.01).byDefault(Value(1.0)); }).description = "0: frozen where it is";
    param(b + "/transport/passes-done", "Passes done", CoreParameter::Type::Int, [](const Timeline &t) { return Value(t.passesDone); },
          [](Timeline &, const Value &) {}, [](CoreParameter &p) { p.readOnly().range(0, 100000); });

    m_tree.add(b + "/signals").label = "Signals";
    m_tree.addSignal(b + "/signals/cycle", "N", "N: a pass starts (an impulse, no value)").label = "Pass";
    m_tree.addSignal(b + "/signals/done", "N", "N: the passes asked for are done").label = "Done";
    Node &progress = m_tree.addSignal(b + "/signals/progress", "f", "f: where it is in the pass (0 to 1)");
    progress.label = "Progress";
    progress.range = {range(0, 1)};
    m_tree.addSignal(b + "/signals/seconds", "d", "d: seconds played, across the passes").label = "Played";
    act(b + "/remove", "N", "Remove", "Remove this timeline (the last one stays)", [this, id](const ValueList &) {
        if (m_timelines.size() <= 1) return false;
        QTimer::singleShot(0, this, [this, id] { removeTimeline(id); });
        return true;
    });
    buildTracks(tl);
}

void Device::buildTracks(Timeline &t)
{
    const std::string b = base(t) + "/tracks";
    m_tree.remove(b);
    t.values.resize(t.anim.tracks.size(), std::numeric_limits<double>::quiet_NaN());
    const auto id = t.anim.id;
    t.trackOutputs = pxs::timeline::declareTrackTree(m_tree, b, t.anim, [this, id]() -> const Animation * {
        const auto *timeline = this->timeline(id);
        return timeline ? &timeline->anim : nullptr;
    }, [this, id](size_t index, AnimTrack track) {
        const auto *timeline = this->timeline(id);
        if (!timeline || index >= timeline->anim.tracks.size()) return;
        auto edited = timeline->anim;
        edited.tracks[index] = std::move(track);
        setTimeline(id, edited, false, true);
    }, [this](const AnimTrack &track) {
        const auto name = m_destinations && track.layer ? m_destinations->nameOf(track.layer) : QString();
        const auto target = track.param.isEmpty() ? QStringLiteral("Unassigned") : track.param;
        return (name.isEmpty() ? target : name + QStringLiteral(" ") + target).toStdString();
    }, [this, id](size_t index) {
        const auto *timeline = this->timeline(id);
        return timeline && index < timeline->values.size() ? timeline->values[index] : std::numeric_limits<double>::quiet_NaN();
    });
}

void Device::setDestinations(Destinations *destinations)
{
    if (m_destinations) disconnect(m_destinations, nullptr, this, nullptr);
    m_destinations = destinations;
    rebuildPresetTargets();
    for (Timeline &t : m_timelines) buildTracks(t);
    if (destinations) connect(destinations, &Destinations::changed, this, [this] {
        rebuildPresetTargets();
        m_stale = true;
    }); // a device made, edited: the show too
}

void Device::rebuildPresetTargets()
{
    m_devicePresetTree.remove("/devices");
    if (!m_destinations) return;
    for (const auto *destination : m_destinations->all()) {
        const quint64 id = destination->id;
        if (m_destinations->isLocalService(*destination)) continue;
        const auto prefix = m_destinations->presetPath(id);
        if (prefix.empty()) continue;
        m_devicePresetTree.add(prefix).label = destination->name;
        for (const auto &parameter : m_destinations->params(id)) {
            const QString path = parameter.path;
            if (!path.startsWith('/')) continue;
            // Peer apps can publish their own device proxies. Import only their native controls.
            if (destination->kind == pxs::devices::Kind::OscQuery &&
                (path == QStringLiteral("/devices") || path.startsWith(QStringLiteral("/devices/")))) continue;
            auto &node = m_devicePresetTree.add(prefix + path.toStdString());
            node.type = "d";
            node.access = 3;
            node.label = parameter.label.toStdString();
            node.get = [this, id, path] {
                double value;
                if (m_destinations && m_destinations->value(id, path, &value)) return ValueList{Value(value)};
                return ValueList{};
            };
            node.set = [this, id, path](const ValueList &values) {
                if (!m_destinations || values.size() != 1) return false;
                const auto value = values.front().number();
                return value && m_destinations->send(id, path, *value);
            };
        }
    }
}

// ---- the timelines: the editor's, the transport

Device::Said Device::said(const Timeline &t) const
{
    Said d;
    d.state = t.anim.state;
    d.position = t.anim.position();
    d.played = t.anim.played();
    d.duration = t.anim.duration;
    d.loop = int(t.anim.loop);
    d.repeat = t.anim.repeat;
    d.speed = t.anim.speed;
    d.passesDone = passesDone(t);
    d.name = t.anim.name.toStdString();
    return d;
}

void Device::tell(const Timeline &t, const Said &before)
{
    const Said now = said(t);
    if (Timeline *x = find(t.anim.id)) x->passesDone = now.passesDone;
    const std::string b = base(t);
    const auto told = [this, &b](bool differs, const char *path) {
        if (differs) m_tree.changed(b + path);
    };
    told((before.state == AnimState::Playing) != (now.state == AnimState::Playing), "/transport/playing");
    told(before.state != now.state, "/transport/state");
    told(before.position != now.position, "/transport/position");
    told(before.played != now.played, "/transport/played");
    told(before.duration != now.duration, "/settings/duration");
    told(before.loop != now.loop, "/settings/loop");
    told(before.repeat != now.repeat, "/settings/passes");
    told(before.speed != now.speed, "/settings/speed");
    told(before.passesDone != now.passesDone, "/transport/passes-done");
    told(before.name != now.name, "/name");
}

int Device::passesDone(const Timeline &t) const
{
    if (t.anim.state == AnimState::Stopped && t.anim.clock <= 0) return 0;
    return int(std::floor(std::min(t.anim.clock, t.anim.length()) / t.anim.passDuration() + 1e-9));
}

void Device::setTimeline(quint64 id, const Animation &edited, bool restoreTransport, bool deferTrackTree)
{
    Timeline *t = find(id);
    if (!t) return;
    const Said before = said(*t);
    // What it is comes from the editor, where it is stays (as NyxMap keeps a timeline that is edited while it plays)
    Animation next = edited;
    next.id = t->anim.id;
    if (!restoreTransport) {
        next.state = t->anim.state;
        next.clock = t->anim.clock;
        next.oscillatorOffset = t->anim.oscillatorOffset;
        next.reversed = t->anim.reversed;
        next.loop = t->anim.loop;
        next.repeat = t->anim.repeat;
        next.setLoop(edited.loop, edited.repeat);
    } else {
        const auto prefix = QStringLiteral(":%1:").arg(id);
        for (auto it = m_filters.begin(); it != m_filters.end();)
            it = it->first.contains(prefix) ? m_filters.erase(it) : std::next(it);
    }
    for (auto &track : next.tracks) {
        for (auto &key : track.keys) key.t = std::clamp(key.t, 0.0, next.duration);
        for (auto &axis : track.axes)
            for (auto &key : axis.keys) key.t = std::clamp(key.t, 0.0, next.duration);
        sortAnimKeys(track);
    }
    const bool rebuild = !pxs::timeline::sameTrackTree(next, t->anim);
    t->anim = std::move(next);
    if (auto *node = m_tree.find(base(*t))) node->label = t->anim.name.toStdString();
    if (rebuild && deferTrackTree) {
        QTimer::singleShot(0, this, [this, id] { if (auto *timeline = find(id)) buildTracks(*timeline); });
    } else if (rebuild) {
        buildTracks(*t);
    }
    tell(*t, before);
    save();
}

void Device::control(quint64 id, AnimAction action, double time)
{
    Timeline *t = find(id);
    if (!t) return;
    const Said before = said(*t);
    t->anim.control(action, time);
    tell(*t, before);
    save();
}

AnimationPlayback Device::playback(quint64 id) const
{
    AnimationPlayback p;
    const Timeline *t = timeline(id);
    if (!t) return p;
    const Animation &a = t->anim;
    p.id = a.id;
    p.name = a.name;
    p.state = a.state;
    p.loop = a.loop;
    p.position = a.position();
    p.duration = a.duration;
    p.clock = a.clock;
    p.played = a.played();
    p.speed = a.speed;
    p.length = a.length();
    p.backwards = a.backwardsAt(a.clock);
    p.tracks = int(a.tracks.size());
    p.pass = a.pass();
    p.captured.assign(a.tracks.size(), std::numeric_limits<double>::quiet_NaN());
    p.capturedRgb.assign(a.tracks.size(), {0, 0, 0});
    p.capturedXy.assign(a.tracks.size(), {0, 0});
    return p;
}

std::vector<::ParamInfo> Device::animatableParams(quint64 holder) const
{
    if (holder && m_destinations) return m_destinations->params(holder);
    ::ParamInfo p;
    p.path = "output";
    p.label = "Output (its own signal, /timelines/<timeline>/tracks/<n>)";
    p.min = 0;
    p.max = 1;
    p.lo = -1;
    p.hi = 2;
    p.animatable = true;
    return {p};
}

bool Device::animParamValue(quint64 holder, const QString &path, double *value) const
{
    if (holder) return m_destinations && m_destinations->value(holder, path, value);
    if (path != "output") return false;
    *value = 0.5;
    return true;
}

bool Device::animationFilterSignal(quint64 holder, quint64 timeline, const QString &path, double *input, std::vector<double> *stages) const
{
    const auto it = m_filters.find(QString::number(holder) + ":" + QString::number(timeline) + ":" + path);
    if (it == m_filters.end() || !it->second.processor.level()) return false;
    if (input) *input = *it->second.processor.level();
    if (stages) *stages = it->second.processor.stageOutputs();
    return true;
}

// A track's value through its filters (pxs-core's, as NyxMap runs those of its timelines)
double Device::filtered(const Timeline &t, size_t index, double raw, double dt)
{
    const AnimTrack &track = t.anim.tracks[index];
    if (track.dataFilters.isEmpty() || !std::isfinite(raw)) return raw;
    FilterState &state = m_filters[QString::number(track.layer) + ":" + QString::number(t.anim.id) + ":" + track.param];
    const double played = t.anim.played();
    if (played < state.played) state.processor.reset(); // it went back: the filters from scratch
    state.played = played;
    if (state.definition != track.dataFilters) {
        state.config.filters = Interaction::filtersFromJson(QJsonObject{{"filters", track.dataFilters}});
        state.definition = track.dataFilters;
    }
    state.processor.input(state.config, raw);
    return state.processor.step(state.config, dt).value_or(raw);
}

// ---- the cue list: NyxMap's sequencer, the Device its host

void Device::buildSequence()
{
    if (m_sequencer.count() == 0) {
        Sequencer::Sequence first;
        first.name = QStringLiteral("Sequence 1");
        m_sequencer.add(first);
    }
    if (m_sequencer.current() < 0) m_sequencer.setCurrent(0);
    group("/sequence", "Sequence", "The cue list (NyxMap's): GO plays the next step, after its pre-wait; a step that follows gives the next one its GO");
    action("/sequence/go", "N", "GO", "Plays the step on standby", [this](const ValueList &) { return m_sequencer.go(); });
    action("/sequence/back", "N", "GO BACK", "Plays the step before, at once", [this](const ValueList &) { return m_sequencer.back(); });
    action("/sequence/stop", "N", "Stop", "The waits and follows of the sequence stopped (the timelines go on)", [this](const ValueList &) {
        m_sequencer.stop();
        return true;
    });
    action("/sequence/stop_all", "N", "Stop all", "Every wait and follow stopped, and every timeline", [this](const ValueList &) {
        stopAll();
        return true;
    });
    action("/sequence/step", "i", "Step", "Plays the step of that number (1: the first)", [this](const ValueList &v) {
        return m_sequencer.goTo(int(v.at(0).number().value_or(0.0)) - 1);
    });
    const auto readOnly = [this](const char *path, const char *label, CoreParameter::Type type, std::function<Value()> get, const char *description) {
        auto p = std::make_unique<CoreParameter>(std::string(path).substr(std::string(path).rfind('/') + 1), label, type);
        p->readOnly().bind(std::move(get), [](const Value &) {});
        if (type == CoreParameter::Type::Int) p->range(0, 10000);
        m_declared.push_back(path);
        m_tree.add(path, std::move(p)).description = description;
    };
    readOnly("/sequence/position", "Played", CoreParameter::Type::Int, [this] { return Value(m_sequencer.position() + 1); },
             "The number of the step played last (0: none yet)");
    readOnly("/sequence/next", "Next", CoreParameter::Type::Text, [this] {
        const int n = m_sequencer.next();
        const Sequencer::Sequence s = m_sequencer.sequence(std::max(0, m_sequencer.current()));
        return Value(n >= 0 && n < int(s.steps.size()) ? s.steps[size_t(n)].description.toStdString() : std::string());
    }, "The step GO plays (its text; empty: none)");
    readOnly("/sequence/steps", "Steps", CoreParameter::Type::Int, [this] {
        return Value(int(m_sequencer.sequence(std::max(0, m_sequencer.current())).steps.size()));
    }, "How many steps");
    signal("/sequence/played", "s", "Played", "s: the text of a step that was played");
    connect(&m_sequencer, &Sequencer::positionChanged, this, [this] {
        sequenceChanged();
        const Sequencer::Sequence s = m_sequencer.sequence(std::max(0, m_sequencer.current()));
        const int at = m_sequencer.position();
        if (at >= 0 && at < int(s.steps.size())) m_tree.fire("/sequence/played", {Value(s.steps[size_t(at)].description.toStdString())});
    });
    connect(&m_sequencer, &Sequencer::changed, this, [this] {
        sequenceChanged();
        save();
    });
}

void Device::sequenceChanged()
{
    m_tree.changed("/sequence/position");
    m_tree.changed("/sequence/next");
    m_tree.changed("/sequence/steps");
}

void Device::stopAll()
{
    m_presetPlayback.clear();
    m_devicePlayback.clear();
    m_sequencer.stopAll();
    for (Timeline &t : m_timelines)
        if (t.anim.state != AnimState::Stopped) control(t.anim.id, AnimAction::Stop);
}

void Device::controlAnimation(quint64 id, AnimAction action, double value, AnimLoop loop, int repeat)
{
    Timeline *t = find(id);
    if (!t) return;
    if (action == AnimAction::LoopMode) {
        const Said before = said(*t);
        t->anim.setLoop(loop, repeat);
        tell(*t, before);
        return;
    }
    control(id, action, value);
}

// What Play plays, in seconds: its passes from where it is (0: endless, nothing to wait for; infinity: frozen, speed 0)
double Device::animationPlayTime(quint64 id) const
{
    const Timeline *t = timeline(id);
    if (!t) return 0;
    const double length = t->anim.length();
    if (!std::isfinite(length)) return 0;
    if (t->anim.speed <= 1e-9) return std::numeric_limits<double>::infinity();
    return (t->anim.state == AnimState::Stopped ? length : std::max(0.0, length - t->anim.clock)) / t->anim.speed;
}

// What a timeline told to play still plays: 0 once over (or stopped, gone, endless), infinity while it does not move
double Device::animationRemaining(quint64 id) const
{
    const Timeline *t = timeline(id);
    if (!t) return 0;
    const double length = t->anim.length();
    if (t->anim.state == AnimState::Stopped || !std::isfinite(length)) return 0;
    if (t->anim.state == AnimState::Paused || t->anim.speed <= 1e-9) return std::numeric_limits<double>::infinity();
    return std::max(0.0, length - t->anim.clock) / t->anim.speed;
}

// ---- a show, and what is kept between launches: the timelines (their transport, their tracks and what each drives) and the cues

QJsonArray Device::timelinesJson(bool includeTransport) const
{
    QJsonArray list;
    for (const Timeline &t : m_timelines) {
        auto data = includeTransport ? animationSnapshotToJson(t.anim) : animationDefinitionToJson(t.anim);
        data["slug"] = QString::fromStdString(t.slug);
        list.append(data);
    }
    return list;
}

void Device::readTimelines(const QJsonArray &list)
{
    for (const QJsonValue &v : list) {
        const auto animation = animationSnapshotFromJson(v.toObject());
        if (!animation || timeline(animation->id)) continue;
        Timeline t;
        t.anim = *animation;
        t.slug = claimTimelineName(v.toObject().value("slug").toString());
        m_nextTimeline = std::max(m_nextTimeline, t.anim.id + 1);
        m_timelines.push_back(std::move(t));
        buildTimeline(m_timelines.back());
    }
}


void Device::readSequence(const QJsonObject &o)
{
    m_sequencer.fromJson(o.value("sequences").toArray(), std::max(0, o.value("sequence").toInt()));
    if (m_sequencer.count() == 0) {
        Sequencer::Sequence first;
        first.name = QStringLiteral("Sequence 1");
        m_sequencer.add(first);
    }
    m_sequencer.setCurrent(std::clamp(m_sequencer.current(), 0, m_sequencer.count() - 1));
}

QJsonArray Device::presetsJson() const
{
    QJsonArray presetsArr;
    for (const auto &p : m_presets) presetsArr.append(QString::fromStdString(p.toJson()));
    return presetsArr;
}

QJsonObject Device::show() const
{
    return QJsonObject{{"timelines", timelinesJson()}, {"sequences", m_sequencer.toJson()}, {"sequence", m_sequencer.current()},
                       {"shown", QString::fromStdString(s.shown)}, {"presets", presetsJson()}};
}

void Device::setPresets(const std::vector<pxs::core::tree::Preset> &presets)
{
    m_presets = presets;
    save();
    tellPresets();
}

void Device::readPresets(const QJsonArray &presets)
{
    m_presets.clear();
    for (const auto &value : presets) {
        auto preset = pxs::core::tree::Preset::fromJson(value.toString().toStdString());
        if (preset.id.empty()) continue;
        m_presets.push_back(std::move(preset));
    }
}


int Device::indexOfPreset(quint64 id) const
{
    if (!id) return -1;
    const auto key = std::to_string(id);
    const auto it = std::find_if(m_presets.begin(), m_presets.end(), [&](const auto &p) { return p.id == key; });
    return it == m_presets.end() ? -1 : int(it - m_presets.begin());
}

void Device::recallPreset(int index, double fade)
{
    recallPreset(index, fade, {});
}

void Device::recallPreset(int index, double fade, const pxs::core::tree::Preset &overrides)
{
    applyPreset(index, fade, overrides);
}

bool Device::applyPreset(int index, double fade, const pxs::core::tree::Preset &overrides)
{
    if (index < 0 || index >= int(m_presets.size())) return false;
    const auto preset = m_presets[index].resolved(overrides, fade);
    const auto reject = [&](const std::string &path) {
        const auto message = QStringLiteral("Preset %1 · unavailable target %2")
                                 .arg(QString::fromStdString(preset.name), QString::fromStdString(path));
        if (m_presetRejected) m_presetRejected(message);
        qWarning().noquote() << message;
    };
    for (const auto &[path, value] : preset.values) {
        const auto &tree = path.rfind("/devices/", 0) == 0 ? m_devicePresetTree : m_tree;
        const auto *node = tree.find(path);
        if (!node || node->locked) {
            reject(path);
            return false;
        }
        if (path.rfind("/devices/", 0) == 0) {
            const auto device = m_destinations ? m_destinations->presetDevice(path) : 0;
            if (!m_destinations || !m_destinations->online(device)) {
                reject(path);
                return false;
            }
        }
    }
    const auto definitions = pxs::presetview::prepareTimelines(preset, [this](quint64 id) { return timeline(id) != nullptr; });
    if (!definitions) {
        reject("timeline");
        return false;
    }
    for (const auto &animation : *definitions) setTimeline(animation.id, animation, true);
    if (!definitions->empty()) timelinesChanged();
    auto local = preset, devices = preset;
    local.values.clear();
    devices.values.clear();
    devices.actions.clear();
    for (const auto &[path, value] : preset.values)
        (path.rfind("/devices/", 0) == 0 ? devices : local).values.emplace(path, value);
    m_presetPlayback.start(m_tree, local);
    m_devicePlayback.start(m_devicePresetTree, devices);
    return true;
}

double Device::presetDuration(quint64 id, double fade, const pxs::core::tree::Preset &overrides) const
{
    const int index = indexOfPreset(id);
    return index < 0 ? 0 : m_presets[index].resolved(overrides, fade).duration();
}

bool Device::loadShow(const QJsonObject &show, QString *error)
{
    if (!show.value("timelines").isArray()) {
        if (error) *error = QStringLiteral("no timelines in it");
        return false;
    }
    for (const Timeline &t : m_timelines) m_tree.remove(base(t));
    m_timelines.clear();
    m_filters.clear();
    m_sequencer.stopAll();
    readTimelines(show.value("timelines").toArray());
    if (m_timelines.empty()) addTimeline();
    s.shown = show.value("shown").toString().toStdString();
    readSequence(show);
    m_presetPlayback.clear();
    m_devicePlayback.clear();
    readPresets(show.value("presets").toArray());
    timelinesChanged();
    sequenceChanged();
    tellPresets();
    writeSettings();
    setSaved();
    return true;
}

QByteArray Device::snapshot() const
{
    return QJsonDocument(QJsonObject{{"timelines", timelinesJson(false)},
                                     {"sequences", m_sequencer.toJson()},
                                     {"presets", presetsJson()},
                                     {"devices", m_destinations ? m_destinations->showJson() : QJsonArray()}})
        .toJson(QJsonDocument::Compact);
}

bool Device::modified() const
{
    if (m_stale) {
        m_differs = snapshot() != m_saved;
        m_stale = false;
    }
    return m_differs;
}

void Device::setSaved()
{
    m_saved = snapshot();
    m_stale = true;
}

void Device::save()
{
    m_stale = true;
    if (m_persistent) m_saveTimer.start();
}

void Device::writeSettings()
{
    if (!m_persistent) return;
    QSettings settings;
    settings.beginGroup(show::settingsGroup());
    settings.setValue(QStringLiteral("timelines"), QJsonDocument(timelinesJson()).toJson(QJsonDocument::Compact));
    settings.setValue(QStringLiteral("sequences"), QJsonDocument(m_sequencer.toJson()).toJson(QJsonDocument::Compact));
    settings.setValue(QStringLiteral("presets"), QJsonDocument(presetsJson()).toJson(QJsonDocument::Compact));
    settings.setValue(QStringLiteral("shown"), QString::fromStdString(s.shown));
}

void Device::load()
{
    QSettings settings;
    settings.beginGroup(show::settingsGroup());
    readTimelines(QJsonDocument::fromJson(settings.value(QStringLiteral("timelines")).toByteArray()).array());
    QJsonObject kept{{"sequences", QJsonDocument::fromJson(settings.value(QStringLiteral("sequences")).toByteArray()).array()}};
    readPresets(QJsonDocument::fromJson(settings.value(QStringLiteral("presets")).toByteArray()).array());
    readSequence(kept);
    s.shown = settings.value(QStringLiteral("shown")).toString().toStdString();
}

// ---- /ui: the window's own state

void Device::buildUi()
{
    group("/ui", "Interface", "What the window shows: the tab and the drawer of the signals are parameters like the others, and move by OSC");
    auto &tab = bound("/ui/tab", "Tab", CoreParameter::Type::Choice, s.tab, [this](CoreParameter &p) { p.choices(m_tabs); });
    tab.description = "The active section of the window";
    auto move = [this](int by) {
        const auto at = std::find(m_tabs.begin(), m_tabs.end(), s.tab);
        const int n = int(m_tabs.size());
        s.tab = m_tabs[std::size_t((int(at - m_tabs.begin()) + by + n) % n)];
        m_tree.changed("/ui/tab");
        return true;
    };
    action("/ui/tab/next", "N", "Next tab", "The tab after, round", [move](const ValueList &) { return move(1); });
    action("/ui/tab/previous", "N", "Previous tab", "The tab before, round", [move](const ValueList &) { return move(-1); });
}

// ---- timeline playback

void Device::tick()
{
    const double dt = m_clock.restart() / 1000.0;
    m_up += dt;
    m_presetPlayback.advance(m_tree, dt);
    m_devicePlayback.advance(m_devicePresetTree, dt);
    m_sequencer.advance(dt); // its waits, on this thread (fadesManual)
    for (Timeline &t : m_timelines) {
        const std::string b = base(t);
        if (t.anim.state == AnimState::Playing) {
            const Said before = said(t);
            const int pass = t.anim.pass();
            const bool more = t.anim.advance(dt);
            if (t.anim.pass() != pass) { // a new pass
                m_tree.fire(b + "/signals/cycle");
            }
            if (!more) { // its passes are done: it stops where it ends, its last value held
                t.anim.state = AnimState::Stopped;
                m_tree.fire(b + "/signals/done");
            }
            tell(t, before);
        }
        if (t.trackOutputs.size() != t.anim.tracks.size()) buildTracks(t);
        const double position = t.anim.position(), played = t.anim.played();
        for (size_t i = 0; i < t.anim.tracks.size(); ++i) {
            const AnimTrack &track = t.anim.tracks[i];
            if (!track.enabled || track.param.isEmpty() || track.event || track.color || track.isPoint()) continue;
            const double v = filtered(t, i, track.valueAt(position, played), dt);
            if (!std::isfinite(v)) continue;
            if (t.values[i] != v) {
                t.values[i] = v;
                m_tree.changed(t.trackOutputs[i]);
            }
            if (track.layer && m_destinations) m_destinations->send(track.layer, track.param, v);
        }
        m_tree.fire(b + "/signals/progress", {Value(t.anim.progress())});
        m_tree.fire(b + "/signals/seconds", {Value(t.anim.played())});
    }
    if (m_ticked) m_ticked();
}

Device::~Device()
{
    if (m_saveTimer.isActive()) { // an edit not kept yet
        m_saveTimer.stop();
        writeSettings();
    }
}
