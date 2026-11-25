#include "Device.h"

#include <pxs/core/tree/parameter.h>

#include <QDateTime>

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
    m_tree.fire("/generator/signals/last_action", {Value(text)});
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

Device::Device(QObject *parent) : QObject(parent)
{
    m_tabs = {"generator", "values", "types", "actions", "access", "structure", "ui"};
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

// ---- /generator: what a device is

void Device::buildGenerator()
{
    // The timeline: one pass of 4 s, in a loop, a sine of a second between 0 and 1
    m_timeline.id = 1;
    m_timeline.name = "Generator";
    AnimTrack t;
    t.param = "output";
    t.oscillator = true;
    t.wave = AnimWave::Sine;
    t.period = 1;
    t.center = 0.5;
    t.amplitude = 0.5;
    t.keys = {AnimKey{0, 0, 3}, AnimKey{2, 1, 3}, AnimKey{4, 0, 3}}; // what Keys starts from
    m_timeline.tracks = {t};

    // A parameter read and written through two functions; whatever it changes beside itself is told
    const auto param = [this](const char *path, const char *label, CoreParameter::Type type, std::function<Value()> get, std::function<void(const Value &)> set,
                              std::function<void(CoreParameter &)> configure) -> Node & {
        const std::string full = path;
        auto p = std::make_unique<CoreParameter>(full.substr(full.rfind('/') + 1), label, type);
        p->bind(std::move(get), [this, set = std::move(set)](const Value &v) {
            const Said before = said();
            set(v);
            tell(before);
        });
        configure(*p);
        m_declared.push_back(path);
        return m_tree.add(path, std::move(p));
    };
    const auto doing = [this](AnimAction a, double time = 0) {
        control(a, time);
        return true;
    };

    group("/generator", "Generator", "A timeline of one track, as NyxMap's: a wave (or keys), played over passes. The editor above edits the same");

    group("/generator/transport", "Transport", "Parameters and actions side by side: where the timeline is, and what moves it");
    param("/generator/transport/play", "Play", CoreParameter::Type::Bool, [this] { return Value(m_timeline.state == AnimState::Playing); },
          [this](const Value &v) { m_timeline.control(v.truthy() ? AnimAction::Play : AnimAction::Pause); },
          [](CoreParameter &p) { p.transient().byDefault(Value(false)); }).description = "On: it plays (from where it is). Off: it pauses";
    bound("/generator/transport/flash", "Flash", CoreParameter::Type::Bool, s.flash, [](CoreParameter &p) { p.transient(); }).description =
        "Holds the output at its maximum while it is on: send true, then false";
    action("/generator/transport/rewind", "N", "Rewind", "Back to the start of the first pass", [doing](const ValueList &) { return doing(AnimAction::Rewind); });
    action("/generator/transport/seek", "f", "Seek", "Go to a place in the pass it is in (0 to 1)", [this, doing](const ValueList &v) {
        const double at = std::clamp(v.at(0).number().value_or(0.0), 0.0, 1.0);
        return doing(AnimAction::Seek, m_timeline.clockAt(at * m_timeline.passDuration()));
    });
    action("/generator/transport/jump", "i", "Jump", "Go to the start of a pass (0: the first)", [this, doing](const ValueList &v) {
        return doing(AnimAction::Seek, std::max(0.0, v.at(0).number().value_or(0.0)) * m_timeline.passDuration());
    });
    action("/generator/transport/go_to", "if", "Go to", "Go to a pass and a place in it (0 to 1)", [this, doing](const ValueList &v) {
        const double d = m_timeline.passDuration();
        return doing(AnimAction::Seek, std::max(0.0, v.at(0).number().value_or(0.0)) * d + std::clamp(v.at(1).number().value_or(0.0), 0.0, 1.0) * d);
    });

    group("/generator/wave", "Wave", "Parameters only: the track, a wave of its own period or keys drawn over the pass");
    param("/generator/wave/keys", "Keys", CoreParameter::Type::Bool, [this] { return Value(!track().oscillator); },
          [this](const Value &v) { track().oscillator = !v.truthy(); },
          [](CoreParameter &p) { p.byDefault(Value(false)); }).description = "Off: the wave below. On: the keys drawn in the editor, over the pass";
    param("/generator/wave/type", "Wave", CoreParameter::Type::Choice, [this] { return Value(animWaveKeys().at(int(track().wave)).toStdString()); },
          [this](const Value &v) {
              const int k = int(animWaveKeys().indexOf(QString::fromStdString(v.text())));
              if (k >= 0) track().wave = AnimWave(k);
          },
          [](CoreParameter &p) {
              std::vector<std::string> keys;
              for (const QString &k : animWaveKeys()) keys.push_back(k.toStdString());
              p.choices(keys).byDefault(Value("sine"));
          });
    param("/generator/wave/frequency", "Frequency", CoreParameter::Type::Float, [this] { return Value(1.0 / std::max(0.01, track().period)); },
          [this](const Value &v) { track().period = 1.0 / std::clamp(v.number().value_or(1.0), 0.05, 20.0); },
          [](CoreParameter &p) { p.range(0.05, 20).step(0.05).unit("frequency.hertz").byDefault(Value(1.0)).animatable(); }).description =
        "Periods a second (the wave's period is its inverse)";
    param("/generator/wave/amplitude", "Amplitude", CoreParameter::Type::Float, [this] { return Value(track().amplitude); },
          [this](const Value &v) { track().amplitude = v.number().value_or(0.5); },
          [](CoreParameter &p) { p.range(0, 1).byDefault(Value(0.5)).animatable(); }).description = "How far it goes from its center, each way";
    param("/generator/wave/center", "Center", CoreParameter::Type::Float, [this] { return Value(track().center); },
          [this](const Value &v) { track().center = v.number().value_or(0.5); },
          [](CoreParameter &p) { p.range(0, 1).byDefault(Value(0.5)).animatable(); }).description = "What it goes around";
    param("/generator/wave/phase", "Phase", CoreParameter::Type::Float, [this] { return Value(track().phase); },
          [this](const Value &v) { track().phase = v.number().value_or(0.0); },
          [](CoreParameter &p) { p.range(0, 1).byDefault(Value(0.0)).animatable(); }).description =
        "Where the wave starts in its period (1 is a whole period)";
    bound("/generator/wave/label", "Label", CoreParameter::Type::Text, s.label, [](CoreParameter &) {});

    group("/generator/timeline", "Timeline", "The passes: how long one is, how they follow one another, how many, how fast");
    param("/generator/timeline/duration", "Duration", CoreParameter::Type::Float, [this] { return Value(m_timeline.duration); },
          [this](const Value &v) { m_timeline.duration = std::clamp(v.number().value_or(4.0), pxs::core::timeline::Transport::kMinDuration, 3600.0); },
          [](CoreParameter &p) { p.range(0.05, 60).limits(0.05, 3600).step(0.05).unit("time.second").byDefault(Value(4.0)); }).description =
        "Seconds of one pass (what the keys are drawn over)";
    param("/generator/timeline/loop", "Loop", CoreParameter::Type::Choice, [this] { return Value(animLoopKey(m_timeline.loop).toStdString()); },
          [this](const Value &v) { m_timeline.setLoop(animLoopFromKey(QString::fromStdString(v.text())), m_timeline.repeat); },
          [](CoreParameter &p) { p.choices({"once", "loop", "pingpong"}).byDefault(Value("loop")); }).description =
        "Once, in a loop, or back and forth";
    param("/generator/timeline/passes", "Passes (0: endless)", CoreParameter::Type::Int, [this] { return Value(m_timeline.repeat); },
          [this](const Value &v) { m_timeline.setLoop(m_timeline.loop, int(v.number().value_or(0.0))); },
          [](CoreParameter &p) { p.range(0, 100).step(1).byDefault(Value(0)); });
    param("/generator/timeline/speed", "Speed", CoreParameter::Type::Float, [this] { return Value(m_timeline.speed); },
          [this](const Value &v) { m_timeline.control(AnimAction::Speed, v.number().value_or(1.0)); },
          [](CoreParameter &p) { p.range(0, 10).step(0.01).byDefault(Value(1.0)); }).description = "0: frozen where it is";
    bound("/generator/timeline/passes_done", "Passes done", CoreParameter::Type::Int, s.passesDone, [](CoreParameter &p) { p.readOnly().range(0, 100000); });

    group("/generator/signals", "Signals", "Signals only: what the generator emits, nobody writes them. The output is a signal that holds others");
    signal("/generator/signals/last_action", "s", "Last action", "The last action received, with its address and arguments");
    signal("/generator/signals/wave", "f", "Output", "The track's value, 50 times a second", {range(-1, 2)});
    signal("/generator/signals/wave/cycle", "N", "Pass", "N: a pass starts (an impulse, no value)");
    signal("/generator/signals/wave/done", "N", "Done", "N: the passes asked for are done");
    signal("/generator/signals/wave/above", "T", "Above center", "T: the output crossed its center");
    signal("/generator/signals/wave/phase", "f", "Progress", "f: where it is in the pass (0 to 1)", {range(0, 1)});
    signal("/generator/signals/wave/seconds", "d", "Played", "d: seconds played, across the passes");
}

// ---- the timeline: the editor's, the transport

Device::Said Device::said() const
{
    Said d;
    d.play = m_timeline.state == AnimState::Playing;
    d.keys = !track().oscillator;
    d.wave = int(track().wave);
    d.frequency = 1.0 / std::max(0.01, track().period);
    d.amplitude = track().amplitude;
    d.center = track().center;
    d.phase = track().phase;
    d.duration = m_timeline.duration;
    d.loop = int(m_timeline.loop);
    d.repeat = m_timeline.repeat;
    d.speed = m_timeline.speed;
    d.passesDone = passesDone();
    return d;
}

void Device::tell(const Said &before)
{
    const Said now = said();
    s.passesDone = now.passesDone;
    const auto told = [this](bool differs, const char *path) {
        if (differs) m_tree.changed(path);
    };
    told(before.play != now.play, "/generator/transport/play");
    told(before.keys != now.keys, "/generator/wave/keys");
    told(before.wave != now.wave, "/generator/wave/type");
    told(before.frequency != now.frequency, "/generator/wave/frequency");
    told(before.amplitude != now.amplitude, "/generator/wave/amplitude");
    told(before.center != now.center, "/generator/wave/center");
    told(before.phase != now.phase, "/generator/wave/phase");
    told(before.duration != now.duration, "/generator/timeline/duration");
    told(before.loop != now.loop, "/generator/timeline/loop");
    told(before.repeat != now.repeat, "/generator/timeline/passes");
    told(before.speed != now.speed, "/generator/timeline/speed");
    told(before.passesDone != now.passesDone, "/generator/timeline/passes_done");
}

int Device::passesDone() const
{
    if (m_timeline.state == AnimState::Stopped && m_timeline.clock <= 0) return 0;
    return int(std::floor(std::min(m_timeline.clock, m_timeline.length()) / m_timeline.passDuration() + 1e-9));
}

void Device::setTimeline(const Animation &edited)
{
    const Said before = said();
    // What it is comes from the editor, where it is stays (as NyxMap keeps a timeline that is edited while it plays)
    Animation next = edited;
    next.id = m_timeline.id;
    next.state = m_timeline.state;
    next.clock = m_timeline.clock;
    next.oscillatorOffset = m_timeline.oscillatorOffset;
    next.reversed = m_timeline.reversed;
    next.loop = m_timeline.loop;
    next.repeat = m_timeline.repeat;
    next.setLoop(edited.loop, edited.repeat);
    if (next.tracks.empty()) next.tracks = m_timeline.tracks; // the generator always has its track
    next.tracks.resize(1);
    sortAnimKeys(next.tracks.front());
    m_timeline = next;
    tell(before);
}

void Device::control(AnimAction action, double time)
{
    const Said before = said();
    m_timeline.control(action, time);
    tell(before);
}

AnimationPlayback Device::playback() const
{
    AnimationPlayback p;
    p.id = m_timeline.id;
    p.name = m_timeline.name;
    p.state = m_timeline.state;
    p.loop = m_timeline.loop;
    p.position = m_timeline.position();
    p.duration = m_timeline.duration;
    p.clock = m_timeline.clock;
    p.played = m_timeline.played();
    p.speed = m_timeline.speed;
    p.length = m_timeline.length();
    p.backwards = m_timeline.backwardsAt(m_timeline.clock);
    p.tracks = 1;
    p.pass = m_timeline.pass();
    p.captured = {std::numeric_limits<double>::quiet_NaN()};
    return p;
}

std::vector<::ParamInfo> Device::animatableParams(quint64) const
{
    ::ParamInfo p;
    p.path = "output";
    p.label = "Generator › Output";
    p.min = 0;
    p.max = 1;
    p.lo = -1;
    p.hi = 2;
    p.animatable = true;
    return {p};
}

bool Device::animParamValue(quint64, const QString &path, double *value) const
{
    if (path != "output") return false;
    *value = s.value;
    return true;
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
    if (m_timeline.state == AnimState::Playing) {
        const Said before = said();
        const int pass = m_timeline.pass();
        const bool more = m_timeline.advance(dt);
        if (m_timeline.pass() != pass) { // a new pass
            m_tree.fire("/generator/signals/wave/cycle");
            m_tree.fire("/structure/mixed/pulse");
            const std::string text = "pass " + std::to_string(m_timeline.pass());
            m_tree.fire("/signals/text", {Value(text)});
            m_tree.fire("/signals/text/length", {Value(int(text.size()))});
        }
        if (!more) { // its passes are done: it stops where it ends, its last value held
            m_timeline.state = AnimState::Stopped;
            m_tree.fire("/generator/signals/wave/done");
        }
        tell(before);
    }
    const double v = track().valueAt(m_timeline.position(), m_timeline.played());
    s.value = s.flash ? track().center + track().amplitude : std::isfinite(v) ? v : track().center;
    m_tree.fire("/generator/signals/wave", {Value(s.value)});
    m_tree.fire("/generator/signals/wave/phase", {Value(m_timeline.progress())});
    m_tree.fire("/generator/signals/wave/seconds", {Value(m_timeline.played())});
    m_tree.fire("/signals/xy", {Value(float(std::clamp(s.value, 0.0, 1.0))), Value(float(std::clamp(1 - s.value, 0.0, 1.0)))});
    m_tree.fire("/signals/rgb", {Value(float(std::clamp(s.value * s.r, 0.0, 1.0))), Value(float(std::clamp(s.value * s.g, 0.0, 1.0))),
                                      Value(float(std::clamp(s.value * s.b, 0.0, 1.0)))});
    m_tree.fire("/signals/stamp", {Value(std::int64_t(QDateTime::currentMSecsSinceEpoch()))});
    const std::uint8_t level = std::uint8_t(std::clamp(s.value, 0.0, 1.0) * 255);
    m_tree.fire("/signals/digest", {Value(Blob{level, std::uint8_t(255 - level), level, std::uint8_t(255 - level)})});
    const bool above = s.value > track().center;
    if (above != s.above) {
        s.above = above;
        m_tree.fire("/generator/signals/wave/above", {Value(above)});
    }
    if (m_ticked) m_ticked();
}
