#include "Device.h"

#include <pxs/core/tree/parameter.h>

#include <QDateTime>

#include <algorithm>
#include <cmath>
#include <memory>

using namespace pxs::core;
using namespace pxs::core::tree;

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
Node &Device::bound(const char *path, const char *label, Parameter::Type type, T &ref, Configure configure)
{
    const std::string full = path;
    auto p = std::make_unique<Parameter>(full.substr(full.rfind('/') + 1), label, type);
    p->byDefault(Value(ref));
    p->bind([&ref] { return Value(ref); }, [&ref](const Value &v) { assign(ref, v); });
    configure(*p);
    m_declared.push_back(path);
    return m_tree.add(path, std::move(p));
}

// A parameter that keeps its own value
template <class Configure>
Node &Device::stored(const char *path, const char *label, Parameter::Type type, Value initial, Configure configure)
{
    const std::string full = path;
    auto p = std::make_unique<Parameter>(full.substr(full.rfind('/') + 1), label, type);
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
    m_tabs = {"generator", "values", "types", "actions", "signals", "access", "structure", "ui"};
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
    group("/generator", "Generator", "A node that holds nodes: the transport, the waveform and the loops");

    group("/generator/transport", "Transport", "A parameter and actions side by side: a switch, and what moves it");
    bound("/generator/transport/play", "Play", Parameter::Type::Bool, s.play, [](Parameter &p) { p.transient(); });
    action("/generator/transport/rewind", "N", "Rewind", "Back to the start of the first loop", [this](const ValueList &) {
        s.pos = 0;
        s.loopCount = 0;
        m_tree.changed("/generator/loops/loop_count");
        return true;
    });
    action("/generator/transport/seek", "f", "Seek", "Go to a place in the loop (0 to 1)", [this](const ValueList &v) {
        s.pos = std::clamp(v.at(0).number().value_or(0.0), 0.0, 1.0);
        return true;
    });
    action("/generator/transport/jump", "i", "Jump", "Go to a loop", [this](const ValueList &v) {
        s.loopCount = std::max(0, int(v.at(0).number().value_or(0.0)));
        m_tree.changed("/generator/loops/loop_count");
        return true;
    });
    action("/generator/transport/go_to", "if", "Go to", "Go to a loop and a place in it", [this](const ValueList &v) {
        s.loopCount = std::max(0, int(v.at(0).number().value_or(0.0)));
        s.pos = std::clamp(v.at(1).number().value_or(0.0), 0.0, 1.0);
        m_tree.changed("/generator/loops/loop_count");
        return true;
    });
    action("/generator/transport/flash", "T", "Flash", "Hold the signal at its maximum while true", [this](const ValueList &v) {
        m_flash = v.at(0).truthy();
        return true;
    });

    group("/generator/wave", "Waveform", "Parameters only: a choice, floats with their range, step and unit, and a text");
    bound("/generator/wave/type", "Type", Parameter::Type::Choice, s.type,
          [](Parameter &p) { p.choices({"sine", "saw", "triangle", "square", "random"}); });
    bound("/generator/wave/frequency", "Frequency", Parameter::Type::Float, s.frequency,
          [](Parameter &p) { p.range(0.05, 20).step(0.05).unit("frequency.hertz").animatable(); });
    bound("/generator/wave/amplitude", "Amplitude", Parameter::Type::Float, s.amplitude, [](Parameter &p) { p.range(0, 1).animatable(); });
    bound("/generator/wave/offset", "Offset", Parameter::Type::Float, s.offset, [](Parameter &p) { p.range(-1, 1).animatable(); });
    bound("/generator/wave/shift", "Phase shift", Parameter::Type::Float, s.shift, [](Parameter &p) { p.range(0, 1).animatable(); }).description =
        "Moves the waveform along its loop (1 is a whole loop): the shape starts later, the loop does not";
    bound("/generator/wave/label", "Label", Parameter::Type::Text, s.label, [](Parameter &) {});

    group("/generator/loops", "Loops", "Whole numbers: any within limits, one among a few, and a state that is only read");
    bound("/generator/loops/loops", "Loops (0: endless)", Parameter::Type::Int, s.loops, [](Parameter &p) { p.range(0, 100).step(1); });
    bound("/generator/loops/steps", "Steps", Parameter::Type::Int, s.steps, [](Parameter &p) { p.values({1, 2, 4, 8, 16}); }).description =
        "Cuts the waveform into a stair of 8 x steps levels (1: smooth). A whole number that can only be one of a few";
    bound("/generator/loops/loop_count", "Loops done", Parameter::Type::Int, s.loopCount, [](Parameter &p) { p.readOnly().range(0, 100000); });

    group("/generator/signals", "Signals", "Signals only: what the generator emits, nobody writes them. The waveform is a signal that holds others");
    signal("/generator/signals/last_action", "s", "Last action", "The last action received, with its address and arguments");
    signal("/generator/signals/wave", "f", "Waveform", "The waveform, 50 times a second", {range(-1, 2)});
    signal("/generator/signals/wave/cycle", "N", "Cycle", "N: a loop starts (an impulse, no value)");
    signal("/generator/signals/wave/done", "N", "Done", "N: the loops asked for are done");
    signal("/generator/signals/wave/above", "T", "Above half", "T: the waveform crossed 0.5");
    signal("/generator/signals/wave/phase", "f", "Phase", "f: where it is in the loop", {range(0, 1)});
    signal("/generator/signals/wave/seconds", "d", "Seconds", "d: seconds since it started playing");
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
    bound("/values/position/x", "X", Parameter::Type::Float, s.x, [](Parameter &p) { p.range(0, 1).animatable().pointComponent(0); });
    bound("/values/position/y", "Y", Parameter::Type::Float, s.y, [](Parameter &p) { p.range(0, 1).animatable().pointComponent(1); });
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
    bound("/values/color/r", "Red", Parameter::Type::Float, s.r, [](Parameter &p) { p.range(0, 1).animatable().colorComponent(0); });
    bound("/values/color/g", "Green", Parameter::Type::Float, s.g, [](Parameter &p) { p.range(0, 1).animatable().colorComponent(1); });
    bound("/values/color/b", "Blue", Parameter::Type::Float, s.b, [](Parameter &p) { p.range(0, 1).animatable().colorComponent(2); });
    bound("/values/color/enable", "Enabled", Parameter::Type::Bool, s.tint, [](Parameter &p) { p.transient(); });
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
    stored("/types/switch", "Switch", Parameter::Type::Bool, Value(false), [](Parameter &) {}).description = "T / F: on or off";
    stored("/types/int", "Whole number", Parameter::Type::Int, Value(0), [](Parameter &p) { p.range(-100, 100).step(1); }).description = "i: within -100 and 100";
    stored("/types/int_among", "One among a few", Parameter::Type::Int, Value(4), [](Parameter &p) { p.values({1, 2, 4, 8, 16}); }).description = "i: only 1 2 4 8 16";
    stored("/types/choice", "Choice", Parameter::Type::Choice, Value("two"), [](Parameter &p) { p.choices({"one", "two", "three"}); }).description = "s: one key among a list";
    stored("/types/float", "Float", Parameter::Type::Float, Value(0.5), [](Parameter &p) { p.range(0, 1).step(0.01).animatable(); }).description = "f: within 0 and 1";
    stored("/types/wide", "Wide float", Parameter::Type::Float, Value(0.0), [](Parameter &p) { p.range(-1000, 1000).step(0.1); }).description = "f: within -1000 and 1000, typed rather than dragged";
    stored("/types/text", "Text", Parameter::Type::Text, Value("hello"), [](Parameter &) {}).description = "s: a string";
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
    bound("/actions/counter", "Counter", Parameter::Type::Int, s.counter, [](Parameter &p) { p.range(0, 1000).step(1); }).description =
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
    bound("/access/read_only", "Read only", Parameter::Type::Int, s.uptime, [](Parameter &p) { p.readOnly().range(0, 1000000); }).description =
        "Seconds the device has been running: it can be read, a write is refused";
    stored("/access/write_only", "Write only", Parameter::Type::Text, Value(""), [](Parameter &p) { p.writeOnly(); }).description =
        "It can be written and not read (no VALUE, no feedback)";
    stored("/access/locked", "Locked", Parameter::Type::Float, Value(0.5), [](Parameter &p) { p.range(0, 1); }).description =
        "Locked by the device: every write is refused";
    m_tree.setLocked("/access/locked", true);
    stored("/access/transient", "Never saved", Parameter::Type::Bool, Value(false), [](Parameter &p) { p.transient(); }).description =
        "A state of the show, not saved with the rest";
    stored("/access/critical", "Critical", Parameter::Type::Float, Value(0.0), [](Parameter &p) { p.range(0, 1); }).critical = true;
    m_tree.find("/access/critical")->description = "A write to it must not be lost (CRITICAL)";
    Node &tagged = stored("/access/tagged", "Tagged", Parameter::Type::Text, Value("tags"), [](Parameter &) {});
    tagged.description = "Tags to search by, and an attribute of its own (note)";
    tagged.tags = {"demo", "text"};
    tagged.attributes["note"] = [] { return std::optional<std::string>("\"an attribute of the device, not of OSCQuery\""); };

    auto trigger = std::make_unique<Parameter>("trigger", "Trigger", Parameter::Type::Trigger);
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
    stored("/structure/a/b/c/d/leaf", "Leaf", Parameter::Type::Bool, Value(false), [](Parameter &) {}).description = "The only parameter at the bottom";
    stored("/structure/a/b/sibling", "Sibling", Parameter::Type::Int, Value(0), [](Parameter &p) { p.range(0, 10).step(1); }).description =
        "Beside the node C, in B";
    group("/structure/mixed", "Mixed", "A parameter, an action and a signal side by side: each is a different kind of leaf");
    stored("/structure/mixed/value", "Value", Parameter::Type::Float, Value(0.0), [](Parameter &p) { p.range(0, 1); }).description = "A parameter";
    action("/structure/mixed/hit", "N", "Hit", "An action: it fires the pulse", [this](const ValueList &) {
        m_tree.fire("/structure/mixed/pulse");
        return true;
    });
    signal("/structure/mixed/pulse", "N", "Pulse", "A signal: an impulse, at each hit and at each loop");
}

// ---- /ui: the window's own state

void Device::buildUi()
{
    group("/ui", "Interface", "What the window shows: the tab is a parameter like the others, and moves by OSC");
    bound("/ui/tab", "Tab", Parameter::Type::Choice, s.tab, [this](Parameter &p) { p.choices(m_tabs); }).description =
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

double Device::wave(double phase)
{
    phase -= std::floor(phase);
    const std::string &t = s.type;
    double v;
    if (t == "saw") v = phase;
    else if (t == "triangle") v = phase < 0.5 ? phase * 2 : 2 - phase * 2;
    else if (t == "square") v = phase < 0.5 ? 1 : 0;
    else if (t == "random") v = std::uniform_real_distribution<double>(0, 1)(m_rng);
    else v = 0.5 + 0.5 * std::sin(kTau * phase);
    if (s.steps > 1 && t != "random") v = std::round(v * (s.steps * 8)) / (s.steps * 8); // the steps: a stair, finer when it is more
    return std::clamp(s.offset + s.amplitude * v, -1.0, 2.0);
}

void Device::tick()
{
    const double dt = m_clock.restart() / 1000.0;
    m_up += dt;
    if (int(m_up) != s.uptime) {
        s.uptime = int(m_up);
        m_tree.changed("/access/read_only");
    }
    if (s.play) {
        s.seconds += dt;
        s.pos += s.frequency * dt;
        if (s.pos >= 1.0) {
            s.pos -= std::floor(s.pos);
            ++s.loopCount;
            m_tree.fire("/generator/signals/wave/cycle");
            m_tree.fire("/structure/mixed/pulse");
            const std::string text = "loop " + std::to_string(s.loopCount);
            m_tree.fire("/signals/text", {Value(text)});
            m_tree.fire("/signals/text/length", {Value(int(text.size()))});
            m_tree.changed("/generator/loops/loop_count");
            if (s.loops > 0 && s.loopCount >= s.loops) {
                s.play = false;
                m_tree.changed("/generator/transport/play");
                m_tree.fire("/generator/signals/wave/done");
            }
        }
    }
    s.value = m_flash ? 1.0 : wave(s.pos + s.shift);
    m_tree.fire("/generator/signals/wave", {Value(s.value)});
    m_tree.fire("/generator/signals/wave/phase", {Value(s.pos)});
    m_tree.fire("/generator/signals/wave/seconds", {Value(s.seconds)});
    m_tree.fire("/signals/xy", {Value(float(std::clamp(s.value, 0.0, 1.0))), Value(float(std::clamp(1 - s.value, 0.0, 1.0)))});
    m_tree.fire("/signals/rgb", {Value(float(std::clamp(s.value * s.r, 0.0, 1.0))), Value(float(std::clamp(s.value * s.g, 0.0, 1.0))),
                                      Value(float(std::clamp(s.value * s.b, 0.0, 1.0)))});
    m_tree.fire("/signals/stamp", {Value(std::int64_t(QDateTime::currentMSecsSinceEpoch()))});
    const std::uint8_t level = std::uint8_t(std::clamp(s.value, 0.0, 1.0) * 255);
    m_tree.fire("/signals/digest", {Value(Blob{level, std::uint8_t(255 - level), level, std::uint8_t(255 - level)})});
    const bool above = s.value > 0.5;
    if (above != s.above) {
        s.above = above;
        m_tree.fire("/generator/signals/wave/above", {Value(above)});
    }
}
