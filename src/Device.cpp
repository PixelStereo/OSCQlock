#include "Device.h"

#include <pxs/core/tree/parameter.h>

#include "Destinations.h"

#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>
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

// A value of the kind the old one is, within the range (OSC says the type of a node once: what is stored keeps it)
bool like(Value &held, const Value &incoming, const ArgRange *limits)
{
    if (held.is<Blob>()) {
        if (!incoming.is<Blob>()) return false;
        held = incoming;
        return true;
    }
    if (held.is<std::string>()) {
        held = Value(incoming.text());
        return true;
    }
    if (held.is<bool>()) {
        held = Value(incoming.truthy());
        return true;
    }
    std::optional<double> n = incoming.number();
    if (!n) return false;
    if (limits && limits->min && limits->max)
        n = std::clamp(*n, limits->min->number().value_or(*n), limits->max->number().value_or(*n));
    if (held.is<int>()) held = Value(int(*n));
    else if (held.is<std::int64_t>()) held = Value(std::int64_t(*n));
    else if (held.is<float>()) held = Value(float(*n));
    else held = Value(*n);
    return true;
}

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

// A parameter that keeps its own value
template <class Configure>
Node &Device::stored(const char *path, const char *label, CoreParameter::Type type, Value initial, Configure configure)
{
    const std::string full = path;
    auto p = std::make_unique<CoreParameter>(full.substr(full.rfind('/') + 1), label, type);
    auto cell = std::make_shared<Value>(initial);
    p->byDefault(initial);
    p->bind([cell] { return *cell; }, [cell](const Value &v) { like(*cell, v, nullptr); });
    configure(*p);
    m_declared.push_back(path);
    return m_tree.add(path, std::move(p));
}

// A node of several arguments (or of a type no Parameter says), with its range and the defaults a reset writes
Node &Device::raw(const char *path, const char *type, const char *label, const char *description, std::vector<ArgRange> ranges, ValueList initial,
                  std::vector<std::string> extendedType)
{
    m_declared.push_back(path);
    Node &n = m_tree.add(path);
    n.type = type;
    n.access = 3;
    n.label = label;
    n.description = description;
    n.range = ranges;
    n.clip = "both";
    n.extendedType = std::move(extendedType);
    auto cell = std::make_shared<ValueList>(initial);
    n.get = [cell] { return *cell; };
    n.set = [cell, ranges](const ValueList &v) {
        if (v.size() < cell->size()) return false;
        ValueList next = *cell;
        for (std::size_t i = 0; i < next.size(); ++i)
            if (!like(next[i], v[i], i < ranges.size() ? &ranges[i] : nullptr)) return false;
        *cell = next;
        return true;
    };
    n.defaultValue = [initial] { return initial; };
    return n;
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
    m_tree.fire("/generator/last_action", {Value(text)});
}

void Device::relate(const std::string &parent, const std::vector<std::string> &children)
{
    Node *p = m_tree.find(parent);
    if (!p || !p->set) return;
    p->set = [this, set = p->set, children](const ValueList &v) {
        if (!set(v)) return false;
        for (const std::string &c : children) m_tree.changed(c);
        return true;
    };
    for (const std::string &childPath : children) {
        Node *c = m_tree.find(childPath);
        if (!c || !c->set) continue;
        c->set = [this, set = c->set, parent](const ValueList &v) {
            if (!set(v)) return false;
            m_tree.changed(parent);
            return true;
        };
    }
}

Device::Device(QObject *parent, bool persistent) : QObject(parent), m_persistent(persistent)
{
    m_tabs = {"generator", "values", "types", "actions", "access", "structure", "ui"};
    if (m_persistent) {
        m_saveTimer.setSingleShot(true);
        m_saveTimer.setInterval(400);
        connect(&m_saveTimer, &QTimer::timeout, this, [this] { writeSettings(); });
        load();
    }
    buildGenerator();
    buildValues();
    buildTypes();
    buildActions();
    buildSignals();
    buildAccess();
    buildStructure();
    buildUi();
    m_clock.start();
    connect(&m_timer, &QTimer::timeout, this, [this] { tick(); });
    m_timer.start(20); // 50 Hz
}

// ---- /generator: the timelines

namespace {

std::string slugOf(quint64 id) { return "timeline" + std::to_string(id); }

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

void Device::buildGenerator()
{
    group("/generator", "Generator",
          "The timelines, as NyxMap's: each a sub-tab, its tracks driving OSCQlock's own outputs or the devices of the network, through filters. "
          "A node that holds nodes that hold parameters, actions, signals, or a mix");
    action("/generator/new", "N", "New timeline", "A timeline more, shown", [this](const ValueList &) {
        show(addTimeline());
        return true;
    });
    action("/generator/new_named", "s", "New timeline, named", "A timeline more, of that name, shown", [this](const ValueList &v) {
        show(addTimeline(QString::fromStdString(v.at(0).text())));
        return true;
    });
    action("/generator/remove", "s", "Remove a timeline", "The timeline of that slug (timeline2…) goes (the last one stays)", [this](const ValueList &v) {
        for (const Timeline &t : m_timelines)
            if (t.slug == v.at(0).text()) return removeTimeline(t.anim.id);
        return false;
    });
    signal("/generator/last_action", "s", "Last action", "The last action received, with its address and arguments");
    if (m_timelines.empty()) addTimeline();
    buildShown();
}

void Device::buildShown()
{
    m_tree.remove("/generator/shown");
    std::vector<std::string> slugs;
    for (const Timeline &t : m_timelines) slugs.push_back(t.slug);
    if (std::find(slugs.begin(), slugs.end(), s.shown) == slugs.end()) s.shown = slugs.empty() ? std::string() : slugs.front();
    auto p = std::make_unique<CoreParameter>("shown", "Shown", CoreParameter::Type::Choice);
    p->choices(slugs).byDefault(Value(slugs.empty() ? std::string() : slugs.front())).transient();
    p->bind([this] { return Value(s.shown); },
            [this](const Value &v) {
                for (const Timeline &t : m_timelines)
                    if (t.slug == v.text()) {
                        s.shown = t.slug;
                        if (m_timelinesChanged) m_timelinesChanged();
                    }
            });
    Node &n = m_tree.add("/generator/shown", std::move(p));
    n.description = "The timeline the window shows (its sub-tab), by its slug. A parameter that holds actions";
    auto move = [this](int by) {
        if (m_timelines.empty()) return false;
        int at = 0;
        for (size_t i = 0; i < m_timelines.size(); ++i)
            if (m_timelines[i].slug == s.shown) at = int(i);
        const int n = int(m_timelines.size());
        s.shown = m_timelines[size_t((at + by + n) % n)].slug;
        m_tree.changed("/generator/shown");
        if (m_timelinesChanged) m_timelinesChanged();
        return true;
    };
    m_tree.addAction("/generator/shown/next", "N", [move](const ValueList &) { return move(1); }, "The timeline after, round").label = "Next timeline";
    m_tree.addAction("/generator/shown/previous", "N", [move](const ValueList &) { return move(-1); }, "The timeline before, round").label =
        "Previous timeline";
    m_tree.changed("/generator/shown");
}

void Device::timelinesChanged()
{
    buildShown();
    if (m_timelinesChanged) m_timelinesChanged();
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

quint64 Device::addTimeline(const QString &name)
{
    Timeline t;
    t.anim.id = m_nextTimeline++;
    t.anim.name = name.trimmed().isEmpty() ? QStringLiteral("Timeline %1").arg(t.anim.id) : name.trimmed();
    t.anim.duration = 4;
    t.anim.setLoop(AnimLoop::Loop, 0);
    t.anim.tracks = {firstTrack()};
    t.slug = slugOf(t.anim.id);
    m_timelines.push_back(std::move(t));
    buildTimeline(m_timelines.back());
    if (m_tree.find("/generator/shown")) timelinesChanged();
    return m_timelines.back().anim.id;
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
    if (m_timelinesChanged) m_timelinesChanged();
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
    m_tree.changed("/generator/shown");
    if (m_timelinesChanged) m_timelinesChanged();
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
        m_tree.addAction(path, type, [this, path, run = std::move(run)](const ValueList &v) {
            if (!run(v)) return false;
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
              if (m_timelinesChanged) m_timelinesChanged();
          },
          [](CoreParameter &p) { p.byDefault(Value("Timeline")); });

    m_tree.add(b + "/transport").label = "Transport";
    param(b + "/transport/play", "Play", CoreParameter::Type::Bool, [](const Timeline &t) { return Value(t.anim.state == AnimState::Playing); },
          [](Timeline &t, const Value &v) { t.anim.control(v.truthy() ? AnimAction::Play : AnimAction::Pause); },
          [](CoreParameter &p) { p.transient().byDefault(Value(false)); }).description = "On: it plays (from where it is). Off: it pauses";
    act(b + "/transport/stop", "N", "Stop", "Stops, back to the start", [this, id](const ValueList &) {
        control(id, AnimAction::Stop);
        return true;
    });
    act(b + "/transport/rewind", "N", "Rewind", "Back to the start of the first pass", [this, id](const ValueList &) {
        control(id, AnimAction::Rewind);
        return true;
    });
    act(b + "/transport/seek", "f", "Seek", "Go to a place in the pass it is in (0 to 1)", [this, id](const ValueList &v) {
        const Timeline *t = timeline(id);
        if (!t) return false;
        control(id, AnimAction::Seek, t->anim.clockAt(std::clamp(v.at(0).number().value_or(0.0), 0.0, 1.0) * t->anim.passDuration()));
        return true;
    });

    m_tree.add(b + "/timeline").label = "Passes";
    param(b + "/timeline/duration", "Duration", CoreParameter::Type::Float, [](const Timeline &t) { return Value(t.anim.duration); },
          [](Timeline &t, const Value &v) { t.anim.duration = std::clamp(v.number().value_or(4.0), pxs::core::timeline::Transport::kMinDuration, 3600.0); },
          [](CoreParameter &p) { p.range(0.05, 60).limits(0.05, 3600).step(0.05).unit("time.second").byDefault(Value(4.0)); }).description =
        "Seconds of one pass (what the keys are drawn over)";
    param(b + "/timeline/loop", "Loop", CoreParameter::Type::Choice, [](const Timeline &t) { return Value(animLoopKey(t.anim.loop).toStdString()); },
          [](Timeline &t, const Value &v) { t.anim.setLoop(animLoopFromKey(QString::fromStdString(v.text())), t.anim.repeat); },
          [](CoreParameter &p) { p.choices({"once", "loop", "pingpong"}).byDefault(Value("loop")); }).description = "Once, in a loop, or back and forth";
    param(b + "/timeline/passes", "Passes (0: endless)", CoreParameter::Type::Int, [](const Timeline &t) { return Value(t.anim.repeat); },
          [](Timeline &t, const Value &v) { t.anim.setLoop(t.anim.loop, int(v.number().value_or(0.0))); },
          [](CoreParameter &p) { p.range(0, 100).step(1).byDefault(Value(0)); });
    param(b + "/timeline/speed", "Speed", CoreParameter::Type::Float, [](const Timeline &t) { return Value(t.anim.speed); },
          [](Timeline &t, const Value &v) { t.anim.control(AnimAction::Speed, v.number().value_or(1.0)); },
          [](CoreParameter &p) { p.range(0, 10).step(0.01).byDefault(Value(1.0)); }).description = "0: frozen where it is";
    param(b + "/timeline/passes_done", "Passes done", CoreParameter::Type::Int, [](const Timeline &t) { return Value(t.passesDone); },
          [](Timeline &, const Value &) {}, [](CoreParameter &p) { p.readOnly().range(0, 100000); });

    m_tree.add(b + "/signals").label = "Signals";
    m_tree.addSignal(b + "/signals/cycle", "N", "N: a pass starts (an impulse, no value)").label = "Pass";
    m_tree.addSignal(b + "/signals/done", "N", "N: the passes asked for are done").label = "Done";
    Node &progress = m_tree.addSignal(b + "/signals/progress", "f", "f: where it is in the pass (0 to 1)");
    progress.label = "Progress";
    progress.range = {range(0, 1)};
    m_tree.addSignal(b + "/signals/seconds", "d", "d: seconds played, across the passes").label = "Played";
    act(b + "/remove", "N", "Remove", "This timeline goes (the last one stays)", [this, id](const ValueList &) { return removeTimeline(id); });
    buildTracks(tl);
}

void Device::buildTracks(Timeline &t)
{
    const std::string b = base(t) + "/tracks";
    m_tree.remove(b);
    t.builtTracks = t.anim.tracks.size();
    t.values.assign(t.anim.tracks.size(), std::numeric_limits<double>::quiet_NaN());
    Node &group = m_tree.add(b);
    group.label = "Tracks";
    group.description = "What each track says now, after its filters (whatever it drives)";
    for (size_t i = 0; i < t.anim.tracks.size(); ++i) {
        const AnimTrack &track = t.anim.tracks[i];
        const QString where = m_destinations && track.layer ? m_destinations->nameOf(track.layer) : QStringLiteral("OSCQlock");
        Node &v = m_tree.addSignal(b + "/" + std::to_string(i + 1), "f", (where + QStringLiteral(" › ") + track.param).toStdString());
        v.label = QStringLiteral("Track %1").arg(i + 1).toStdString();
    }
}

void Device::setDestinations(Destinations *destinations)
{
    m_destinations = destinations;
    for (Timeline &t : m_timelines) buildTracks(t);
}

// ---- the timelines: the editor's, the transport

Device::Said Device::said(const Timeline &t) const
{
    Said d;
    d.play = t.anim.state == AnimState::Playing;
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
    told(before.play != now.play, "/transport/play");
    told(before.duration != now.duration, "/timeline/duration");
    told(before.loop != now.loop, "/timeline/loop");
    told(before.repeat != now.repeat, "/timeline/passes");
    told(before.speed != now.speed, "/timeline/speed");
    told(before.passesDone != now.passesDone, "/timeline/passes_done");
    told(before.name != now.name, "/name");
}

int Device::passesDone(const Timeline &t) const
{
    if (t.anim.state == AnimState::Stopped && t.anim.clock <= 0) return 0;
    return int(std::floor(std::min(t.anim.clock, t.anim.length()) / t.anim.passDuration() + 1e-9));
}

void Device::setTimeline(quint64 id, const Animation &edited)
{
    Timeline *t = find(id);
    if (!t) return;
    const Said before = said(*t);
    // What it is comes from the editor, where it is stays (as NyxMap keeps a timeline that is edited while it plays)
    Animation next = edited;
    next.id = t->anim.id;
    next.state = t->anim.state;
    next.clock = t->anim.clock;
    next.oscillatorOffset = t->anim.oscillatorOffset;
    next.reversed = t->anim.reversed;
    next.loop = t->anim.loop;
    next.repeat = t->anim.repeat;
    next.setLoop(edited.loop, edited.repeat);
    for (AnimTrack &track : next.tracks) sortAnimKeys(track);
    bool targets = next.tracks.size() != t->anim.tracks.size();
    for (size_t i = 0; !targets && i < next.tracks.size(); ++i)
        targets = next.tracks[i].layer != t->anim.tracks[i].layer || next.tracks[i].param != t->anim.tracks[i].param;
    t->anim = next;
    if (targets) buildTracks(*t);
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
    p.label = "Output (its own signal, /generator/<timeline>/tracks/<n>)";
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
    *value = s.value;
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

// ---- kept between launches: the timelines (their transport, their tracks and what each drives)

void Device::save()
{
    if (m_persistent) m_saveTimer.start();
}

void Device::writeSettings()
{
    QJsonArray list;
    for (const Timeline &t : m_timelines) {
        QJsonObject o{{"id", double(t.anim.id)}, {"name", t.anim.name}};
        animTransportToJson(t.anim, o);
        QJsonArray tracks;
        for (const AnimTrack &track : t.anim.tracks) {
            QJsonObject j;
            animTrackToJson(track, j);
            j["holder"] = double(track.layer);
            j["path"] = track.param;
            j["enabled"] = track.enabled;
            tracks.append(j);
        }
        o["tracks"] = tracks;
        list.append(o);
    }
    QSettings settings;
    settings.setValue(QStringLiteral("timelines"), QJsonDocument(list).toJson(QJsonDocument::Compact));
    settings.setValue(QStringLiteral("shown"), QString::fromStdString(s.shown));
}

void Device::load()
{
    const QJsonArray list = QJsonDocument::fromJson(QSettings().value(QStringLiteral("timelines")).toByteArray()).array();
    for (const QJsonValue &v : list) {
        const QJsonObject o = v.toObject();
        Timeline t;
        t.anim.id = quint64(o.value("id").toDouble());
        if (!t.anim.id || timeline(t.anim.id)) continue;
        t.anim.name = o.value("name").toString(QStringLiteral("Timeline"));
        animTransportFromJson(t.anim, o);
        for (const QJsonValue &x : o.value("tracks").toArray()) {
            const QJsonObject j = x.toObject();
            AnimTrack track;
            animTrackFromJson(track, j, t.anim.duration);
            track.layer = quint64(j.value("holder").toDouble());
            track.param = j.value("path").toString();
            track.enabled = j.value("enabled").toBool(true);
            t.anim.tracks.push_back(track);
        }
        t.slug = slugOf(t.anim.id);
        m_nextTimeline = std::max(m_nextTimeline, t.anim.id + 1);
        m_timelines.push_back(std::move(t));
        buildTimeline(m_timelines.back());
    }
    s.shown = QSettings().value(QStringLiteral("shown")).toString().toStdString();
}

// ---- /values: a parameter that holds its components

void Device::buildValues()
{
    group("/values", "Values", "A value that holds its components: a point, a colour. Writing the whole or a component says it to the other");

    Node &position = raw("/values/position", "ff", "Position", "A point: a value (ff) that holds x and y", {range(0, 1), range(0, 1)},
                         {Value(0.5f), Value(0.5f)}, {"position.cartesian.x", "position.cartesian.y"});
    position.get = [this] { return ValueList{Value(s.x), Value(s.y)}; };
    position.set = [this](const ValueList &v) {
        if (v.size() < 2) return false;
        s.x = float(std::clamp(v[0].number().value_or(s.x), 0.0, 1.0));
        s.y = float(std::clamp(v[1].number().value_or(s.y), 0.0, 1.0));
        return true;
    };
    bound("/values/position/x", "X", CoreParameter::Type::Float, s.x, [](CoreParameter &p) { p.range(0, 1).animatable().pointComponent(0); });
    bound("/values/position/y", "Y", CoreParameter::Type::Float, s.y, [](CoreParameter &p) { p.range(0, 1).animatable().pointComponent(1); });
    relate("/values/position", {"/values/position/x", "/values/position/y"});

    Node &color = raw("/values/color", "fff", "Color", "A colour: a value (fff) that holds r, g and b, a switch and an action", {range(0, 1), range(0, 1), range(0, 1)},
                      {Value(1.0f), Value(0.5f), Value(0.0f)}, {"color.rgb.r", "color.rgb.g", "color.rgb.b"});
    color.get = [this] { return ValueList{Value(s.r), Value(s.g), Value(s.b)}; };
    color.set = [this](const ValueList &v) {
        if (v.size() < 3) return false;
        s.r = float(std::clamp(v[0].number().value_or(s.r), 0.0, 1.0));
        s.g = float(std::clamp(v[1].number().value_or(s.g), 0.0, 1.0));
        s.b = float(std::clamp(v[2].number().value_or(s.b), 0.0, 1.0));
        return true;
    };
    bound("/values/color/r", "Red", CoreParameter::Type::Float, s.r, [](CoreParameter &p) { p.range(0, 1).animatable().colorComponent(0); });
    bound("/values/color/g", "Green", CoreParameter::Type::Float, s.g, [](CoreParameter &p) { p.range(0, 1).animatable().colorComponent(1); });
    bound("/values/color/b", "Blue", CoreParameter::Type::Float, s.b, [](CoreParameter &p) { p.range(0, 1).animatable().colorComponent(2); });
    bound("/values/color/enable", "Enabled", CoreParameter::Type::Bool, s.tint, [](CoreParameter &p) { p.transient(); });
    relate("/values/color", {"/values/color/r", "/values/color/g", "/values/color/b"});
    action("/values/color/set", "fff", "Set the color", "An action under a value: sets the three components", [this](const ValueList &v) {
        s.r = float(std::clamp(v.at(0).number().value_or(0.0), 0.0, 1.0));
        s.g = float(std::clamp(v.at(1).number().value_or(0.0), 0.0, 1.0));
        s.b = float(std::clamp(v.at(2).number().value_or(0.0), 0.0, 1.0));
        for (const char *p : {"/values/color", "/values/color/r", "/values/color/g", "/values/color/b"}) m_tree.changed(p);
        return true;
    });

    raw("/values/rgba", "ffff", "Color with alpha", "A colour with its opacity (ffff): a value with no components of its own",
        {range(0, 1), range(0, 1), range(0, 1), range(0, 1)}, {Value(0.2f), Value(0.6f), Value(1.0f), Value(1.0f)},
        {"color.rgba.r", "color.rgba.g", "color.rgba.b", "color.rgba.a"});
    raw("/values/origin", "fff", "Origin", "Three numbers with no meaning of their own (fff), no components", {range(-10, 10), range(-10, 10), range(-10, 10)},
        {Value(0.0f), Value(0.0f), Value(0.0f)});
}

// ---- /types: one parameter of each kind

void Device::buildTypes()
{
    group("/types", "Types", "One parameter of each kind OSC and OSCQuery have: the widget that edits it is the one for its type");
    stored("/types/switch", "Switch", CoreParameter::Type::Bool, Value(false), [](CoreParameter &) {}).description = "T / F: on or off";
    stored("/types/int", "Whole number", CoreParameter::Type::Int, Value(0), [](CoreParameter &p) { p.range(-100, 100).step(1); }).description = "i: within -100 and 100";
    stored("/types/int_among", "One among a few", CoreParameter::Type::Int, Value(4), [](CoreParameter &p) { p.values({1, 2, 4, 8, 16}); }).description = "i: only 1 2 4 8 16";
    stored("/types/choice", "Choice", CoreParameter::Type::Choice, Value("two"), [](CoreParameter &p) { p.choices({"one", "two", "three"}); }).description = "s: one key among a list";
    stored("/types/float", "Float", CoreParameter::Type::Float, Value(0.5), [](CoreParameter &p) { p.range(0, 1).step(0.01).animatable(); }).description = "f: within 0 and 1";
    stored("/types/wide", "Wide float", CoreParameter::Type::Float, Value(0.0), [](CoreParameter &p) { p.range(-1000, 1000).step(0.1); }).description = "f: within -1000 and 1000, typed rather than dragged";
    stored("/types/text", "Text", CoreParameter::Type::Text, Value("hello"), [](CoreParameter &) {}).description = "s: a string";
    raw("/types/double", "d", "Double", "d: a double", {range(-10, 10)}, {Value(3.141592653589793)});
    raw("/types/int64", "h", "64-bit number", "h: a 64-bit whole number", {range(0, 9e15)}, {Value(std::int64_t(1234567890123))});
    raw("/types/blob", "b", "Blob", "b: bytes (shown in hexadecimal)", {ArgRange{}}, {Value(Blob{0xde, 0xad, 0xbe, 0xef})});
}

// ---- /actions: only actions

void Device::buildActions()
{
    group("/actions", "Actions", "Nodes that are only actions: they hold no value and do something when they are received. The last one is told on /signals");
    action("/actions/none", "N", "No value", "N: nothing is sent, nothing is kept", nullptr);
    action("/actions/float", "f", "Float", "f: one number", nullptr);
    action("/actions/int", "i", "Whole number", "i: one whole number", nullptr);
    action("/actions/text", "s", "Text", "s: one text", nullptr);
    action("/actions/switch", "T", "Switch", "T: on or off", nullptr);
    action("/actions/int64", "h", "64-bit number", "h: one 64-bit whole number", nullptr);
    action("/actions/double", "d", "Double", "d: one double", nullptr);
    action("/actions/blob", "b", "Blob", "b: bytes", nullptr);
    action("/actions/point", "ff", "Point", "ff: two numbers", nullptr);
    action("/actions/color", "fff", "Color", "fff: three numbers", nullptr);
    action("/actions/several", "if", "Several", "if: a whole number and a number", nullptr);
    action("/actions/mixed", "sfT", "Mixed", "sfT: a text, a number and a switch", nullptr);

    // A parameter that holds actions
    bound("/actions/counter", "Counter", CoreParameter::Type::Int, s.counter, [](CoreParameter &p) { p.range(0, 1000).step(1); }).description =
        "A parameter that holds actions: they move it";
    action("/actions/counter/increment", "N", "Increment", "+1", [this](const ValueList &) {
        s.counter = std::min(1000, s.counter + 1);
        m_tree.changed("/actions/counter");
        return true;
    });
    action("/actions/counter/decrement", "N", "Decrement", "-1", [this](const ValueList &) {
        s.counter = std::max(0, s.counter - 1);
        m_tree.changed("/actions/counter");
        return true;
    });
    action("/actions/counter/reset", "N", "Reset", "Back to 0", [this](const ValueList &) {
        s.counter = 0;
        m_tree.changed("/actions/counter");
        return true;
    });
}

// ---- /signals: only signals

void Device::buildSignals()
{
    group("/signals", "Signals", "Nodes that are only signals, of the kinds the generator does not need: the device emits them, nobody writes them");
    signal("/signals/xy", "ff", "Waveform and opposite", "ff: two numbers that move together", {range(0, 1), range(0, 1)});
    signal("/signals/rgb", "fff", "Color", "fff: a colour that follows the waveform", {range(0, 1), range(0, 1), range(0, 1)},
           {"color.rgb.r", "color.rgb.g", "color.rgb.b"});
    signal("/signals/text", "s", "Text", "s: a text with the loop count. A signal that holds another");
    signal("/signals/text/length", "i", "Text length", "i: how long that text is");
    signal("/signals/stamp", "h", "Stamp", "h: milliseconds since 1970 (64 bits)");
    signal("/signals/digest", "b", "Digest", "b: four bytes that follow the waveform");
}

// ---- /access: who can do what

void Device::buildAccess()
{
    group("/access", "Access", "Who can read and who can write: what a controller may do with each");
    bound("/access/read_only", "Read only", CoreParameter::Type::Int, s.uptime, [](CoreParameter &p) { p.readOnly().range(0, 1000000); }).description =
        "Seconds the device has been running: it can be read, a write is refused";
    stored("/access/write_only", "Write only", CoreParameter::Type::Text, Value(""), [](CoreParameter &p) { p.writeOnly(); }).description =
        "It can be written and not read (no VALUE, no feedback)";
    stored("/access/locked", "Locked", CoreParameter::Type::Float, Value(0.5), [](CoreParameter &p) { p.range(0, 1); }).description =
        "Locked by the device: every write is refused";
    m_tree.setLocked("/access/locked", true);
    stored("/access/transient", "Never saved", CoreParameter::Type::Bool, Value(false), [](CoreParameter &p) { p.transient(); }).description =
        "A state of the show, not saved with the rest";
    stored("/access/critical", "Critical", CoreParameter::Type::Float, Value(0.0), [](CoreParameter &p) { p.range(0, 1); }).critical = true;
    m_tree.find("/access/critical")->description = "A write to it must not be lost (CRITICAL)";
    Node &tagged = stored("/access/tagged", "Tagged", CoreParameter::Type::Text, Value("tags"), [](CoreParameter &) {});
    tagged.description = "Tags to search by, and an attribute of its own (note)";
    tagged.tags = {"demo", "text"};
    tagged.attributes["note"] = [] { return std::optional<std::string>("\"an attribute of the device, not of OSCQuery\""); };

    auto trigger = std::make_unique<CoreParameter>("trigger", "Trigger", CoreParameter::Type::Trigger);
    trigger->bind(nullptr, [this](const Value &) { said("/access/trigger", {}); });
    m_tree.add("/access/trigger", std::move(trigger)).description = "A parameter that is an action without a value";
}

// ---- /structure: only nodes

void Device::buildStructure()
{
    group("/structure", "Structure", "Nodes that only hold nodes: empty, deep, side by side");
    group("/structure/empty", "Empty", "A node with nothing in it");
    group("/structure/a", "A", "Four levels down");
    group("/structure/a/b", "B", "");
    group("/structure/a/b/c", "C", "");
    group("/structure/a/b/c/d", "D", "");
    stored("/structure/a/b/c/d/leaf", "Leaf", CoreParameter::Type::Bool, Value(false), [](CoreParameter &) {}).description = "The only parameter at the bottom";
    stored("/structure/a/b/sibling", "Sibling", CoreParameter::Type::Int, Value(0), [](CoreParameter &p) { p.range(0, 10).step(1); }).description =
        "Beside the node C, in B";
    group("/structure/mixed", "Mixed", "A parameter, an action and a signal side by side: each is a different kind of leaf");
    stored("/structure/mixed/value", "Value", CoreParameter::Type::Float, Value(0.0), [](CoreParameter &p) { p.range(0, 1); }).description = "A parameter";
    action("/structure/mixed/hit", "N", "Hit", "An action: it fires the pulse", [this](const ValueList &) {
        m_tree.fire("/structure/mixed/pulse");
        return true;
    });
    signal("/structure/mixed/pulse", "N", "Pulse", "A signal: an impulse, at each hit and at each loop");
}

// ---- /ui: the window's own state

void Device::buildUi()
{
    group("/ui", "Interface", "What the window shows: the tab and the drawer of the signals are parameters like the others, and move by OSC");
    bound("/ui/tab", "Tab", CoreParameter::Type::Choice, s.tab, [this](CoreParameter &p) { p.choices(m_tabs); }).description =
        "The tab of the window (the name of a top-level node). A parameter that holds actions";
    auto move = [this](int by) {
        const auto at = std::find(m_tabs.begin(), m_tabs.end(), s.tab);
        const int n = int(m_tabs.size());
        s.tab = m_tabs[std::size_t((int(at - m_tabs.begin()) + by + n) % n)];
        m_tree.changed("/ui/tab");
        return true;
    };
    bound("/ui/signals", "Signals drawer", CoreParameter::Type::Bool, s.drawer, [](CoreParameter &p) { p.transient(); }).description =
        "The drawer that shows the signals: open or closed";
    action("/ui/tab/next", "N", "Next tab", "The tab after, round", [move](const ValueList &) { return move(1); });
    action("/ui/tab/previous", "N", "Previous tab", "The tab before, round", [move](const ValueList &) { return move(-1); });
}

// ---- the generator itself

void Device::tick()
{
    const double dt = m_clock.restart() / 1000.0;
    m_up += dt;
    if (int(m_up) != s.uptime) {
        s.uptime = int(m_up);
        m_tree.changed("/access/read_only");
    }
    for (Timeline &t : m_timelines) {
        const std::string b = base(t);
        if (t.anim.state == AnimState::Playing) {
            const Said before = said(t);
            const int pass = t.anim.pass();
            const bool more = t.anim.advance(dt);
            if (t.anim.pass() != pass) { // a new pass
                m_tree.fire(b + "/signals/cycle");
                if (&t == &m_timelines.front()) {
                    m_tree.fire("/structure/mixed/pulse");
                    const std::string text = "pass " + std::to_string(t.anim.pass());
                    m_tree.fire("/signals/text", {Value(text)});
                    m_tree.fire("/signals/text/length", {Value(int(text.size()))});
                }
            }
            if (!more) { // its passes are done: it stops where it ends, its last value held
                t.anim.state = AnimState::Stopped;
                m_tree.fire(b + "/signals/done");
            }
            tell(t, before);
        }
        if (t.builtTracks != t.anim.tracks.size()) buildTracks(t);
        const double position = t.anim.position(), played = t.anim.played();
        for (size_t i = 0; i < t.anim.tracks.size(); ++i) {
            const AnimTrack &track = t.anim.tracks[i];
            if (!track.enabled || track.param.isEmpty() || track.event || track.color || track.isPoint()) continue;
            const double v = filtered(t, i, track.valueAt(position, played), dt);
            if (!std::isfinite(v)) continue;
            t.values[i] = v;
            m_tree.fire(b + "/tracks/" + std::to_string(i + 1), {Value(float(v))});
            if (track.layer && m_destinations) m_destinations->send(track.layer, track.param, v);
        }
        m_tree.fire(b + "/signals/progress", {Value(t.anim.progress())});
        m_tree.fire(b + "/signals/seconds", {Value(t.anim.played())});
    }
    // The test signals follow the first track of the first timeline
    const Timeline &first = m_timelines.front();
    if (!first.values.empty() && std::isfinite(first.values.front())) s.value = first.values.front();
    m_tree.fire("/signals/xy", {Value(float(std::clamp(s.value, 0.0, 1.0))), Value(float(std::clamp(1 - s.value, 0.0, 1.0)))});
    m_tree.fire("/signals/rgb", {Value(float(std::clamp(s.value * s.r, 0.0, 1.0))), Value(float(std::clamp(s.value * s.g, 0.0, 1.0))),
                                      Value(float(std::clamp(s.value * s.b, 0.0, 1.0)))});
    m_tree.fire("/signals/stamp", {Value(std::int64_t(QDateTime::currentMSecsSinceEpoch()))});
    const std::uint8_t level = std::uint8_t(std::clamp(s.value, 0.0, 1.0) * 255);
    m_tree.fire("/signals/digest", {Value(Blob{level, std::uint8_t(255 - level), level, std::uint8_t(255 - level)})});
    if (m_ticked) m_ticked();
}

Device::~Device()
{
    if (m_saveTimer.isActive()) { // an edit not kept yet
        m_saveTimer.stop();
        writeSettings();
    }
}
