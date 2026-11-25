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
    m_tree.fire("/generator/last_action", {Value(text)});
}

Device::Device(QObject *parent, bool persistent) : QObject(parent), m_persistent(persistent), m_sequencer(*this, m_lock, this)
{
    m_tabs = {"generator", "sequence"};
    if (m_persistent) {
        m_saveTimer.setSingleShot(true);
        m_saveTimer.setInterval(400);
        connect(&m_saveTimer, &QTimer::timeout, this, [this] { writeSettings(); });
        load();
    }
    buildGenerator();
    buildSequence();
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
    std::vector<std::string> slugs;
    for (const Timeline &t : m_timelines) slugs.push_back(t.slug);
    if (std::find(slugs.begin(), slugs.end(), s.shown) == slugs.end()) s.shown = slugs.empty() ? std::string() : slugs.front();
    // Already there: its choices are changed where it is (a client that listens to it keeps listening)
    if (Node *shown = m_tree.find("/generator/shown"); shown && shown->parameter()) {
        shown->parameter()->choices(slugs);
        ArgRange keys;
        for (const std::string &slug : slugs) keys.vals.emplace_back(slug);
        shown->range = {keys};
        m_tree.changed("/generator/shown");
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
        tellTimelines();
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

void Device::restoreTimeline(const Animation &timeline, int index)
{
    if (!timeline.id || this->timeline(timeline.id)) return;
    Timeline t;
    t.anim = timeline;
    t.anim.state = AnimState::Stopped;
    t.anim.clock = 0;
    t.slug = slugOf(t.anim.id);
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
    m_tree.changed("/generator/shown");
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
              tellTimelines();
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
    if (destinations) connect(destinations, &Destinations::changed, this, [this] { m_stale = true; }); // a device made, edited: the show too
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

QJsonArray Device::timelinesJson() const
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
    return list;
}

void Device::readTimelines(const QJsonArray &list)
{
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
}

void Device::readSequence(const QJsonObject &o)
{
    if (o.value("sequences").isArray()) {
        m_sequencer.fromJson(o.value("sequences").toArray(), std::max(0, o.value("sequence").toInt()));
    } else {
        // An OSCQlock before NyxMap's sequencer kept "cues": each one a step (start: rewind then play; stop all: every timeline stopped)
        m_sequencer.clear();
        Sequencer::Sequence seq;
        seq.name = QStringLiteral("Sequence 1");
        for (const QJsonValue &v : o.value("cues").toArray()) {
            const QJsonObject c = v.toObject();
            Sequencer::Step st;
            st.description = c.value("name").toString();
            st.preWait = std::max(0.0, c.value("wait").toDouble());
            st.next = c.value("continue").toBool() ? Sequencer::StepContinue::AutoFollow : Sequencer::StepContinue::Wait;
            const quint64 timeline = quint64(c.value("timeline").toDouble());
            const QString action = c.value("action").toString();
            const auto act = [](quint64 id, AnimAction a) {
                Sequencer::Step x;
                x.timeline = id;
                x.action = a;
                return x;
            };
            if (action == QLatin1String("start")) st.actions = {act(timeline, AnimAction::Rewind), act(timeline, AnimAction::Play)};
            else if (action == QLatin1String("stop_all"))
                for (const Timeline &t : m_timelines) st.actions.push_back(act(t.anim.id, AnimAction::Stop));
            else {
                st.timeline = timeline;
                st.action = action == QLatin1String("pause") ? AnimAction::Pause : action == QLatin1String("stop") ? AnimAction::Stop : AnimAction::Play;
            }
            seq.steps.push_back(st);
        }
        m_sequencer.add(seq);
    }
    if (m_sequencer.count() == 0) {
        Sequencer::Sequence first;
        first.name = QStringLiteral("Sequence 1");
        m_sequencer.add(first);
    }
    m_sequencer.setCurrent(std::clamp(m_sequencer.current(), 0, m_sequencer.count() - 1));
}

QJsonObject Device::show() const
{
    return QJsonObject{{"timelines", timelinesJson()}, {"sequences", m_sequencer.toJson()}, {"sequence", m_sequencer.current()},
                       {"shown", QString::fromStdString(s.shown)}};
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
    timelinesChanged();
    sequenceChanged();
    writeSettings();
    setSaved();
    return true;
}

QByteArray Device::snapshot() const
{
    std::vector<quint64> used;
    for (const Timeline &t : m_timelines)
        for (const AnimTrack &track : t.anim.tracks)
            if (track.layer) used.push_back(track.layer);
    return QJsonDocument(QJsonObject{{"timelines", timelinesJson()},
                                     {"sequences", m_sequencer.toJson()},
                                     {"devices", m_destinations ? m_destinations->showJson(used) : QJsonArray()}})
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
    settings.setValue(QStringLiteral("timelines"), QJsonDocument(timelinesJson()).toJson(QJsonDocument::Compact));
    settings.setValue(QStringLiteral("sequences"), QJsonDocument(m_sequencer.toJson()).toJson(QJsonDocument::Compact));
    settings.remove(QStringLiteral("cues")); // an OSCQlock before NyxMap's sequencer: read once, then as sequences
    settings.setValue(QStringLiteral("shown"), QString::fromStdString(s.shown));
}

void Device::load()
{
    QSettings settings;
    readTimelines(QJsonDocument::fromJson(settings.value(QStringLiteral("timelines")).toByteArray()).array());
    QJsonObject kept;
    if (settings.contains(QStringLiteral("sequences")))
        kept["sequences"] = QJsonDocument::fromJson(settings.value(QStringLiteral("sequences")).toByteArray()).array();
    else
        kept["cues"] = QJsonDocument::fromJson(settings.value(QStringLiteral("cues")).toByteArray()).array();
    readSequence(kept);
    s.shown = settings.value(QStringLiteral("shown")).toString().toStdString();
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
    action("/ui/tab/next", "N", "Next tab", "The tab after, round", [move](const ValueList &) { return move(1); });
    action("/ui/tab/previous", "N", "Previous tab", "The tab before, round", [move](const ValueList &) { return move(-1); });
}

// ---- the generator itself

void Device::tick()
{
    const double dt = m_clock.restart() / 1000.0;
    m_up += dt;
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
    if (m_ticked) m_ticked();
}

Device::~Device()
{
    if (m_saveTimer.isActive()) { // an edit not kept yet
        m_saveTimer.stop();
        writeSettings();
    }
}
