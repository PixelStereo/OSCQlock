#pragma once
// The device: a generator (a sine, a saw, a triangle, a square or noise) that says everything about itself in every kind of node the
// pxs-core tree has, so that a controller, an explorer or a mapper can be tried on all of them. The namespace is the contract, and it is
// made so that each top-level node (a tab of the window, see Panel) shows one case:
//
//   /generator   the timelines: as many as one wants, each a sub-tab of the first tab, each a node (/generator/<slug>) that holds nodes that
//                hold parameters, actions, signals, or a mix: what a device is. /generator/shown is the sub-tab shown; /generator/new,
//                /generator/remove make and remove them. Their tracks drive OSCQlock's own outputs or the devices of the network
//                (Destinations.h), through the filters of pxs-core
//   /values      a parameter that holds its components, with an action and a switch under it; a colour; a point; plain groups of numbers
//   /types       one parameter of each kind: switch, whole number (any, one among a few), choice, float, double, 64 bits, text, blob
//   /actions     nodes that are only actions: with no value, with one of each kind, with several; and a parameter that holds actions
//   /signals     nodes that are only signals, of the kinds the generator does not say; one that holds another. Like the signals under
//                /generator and /structure it is shown in the drawer of the window, not in a tab
//   /access      who can do what: read only, write only, locked, never saved, critical, tagged, a trigger
//   /structure   nodes that are only nodes: empty, deep, side by side, and one that holds a parameter, an action and a signal
//   /ui          the window's own state, driven by OSC like any parameter: its tab (and the actions that move it), the drawer of the signals
#include <pxs/TimelineHost.h>
#include <Interactions.h>
#include <pxs/core/tree/tree.h>

#include <QElapsedTimer>
#include <QObject>
#include <QTimer>

#include <random>
#include <string>
#include <vector>

#include <QJsonArray>

#include <functional>
#include <map>

class Destinations;

// The timelines are NyxMap's (pxs-core's logic, pxs-qt's editor). Each is a node of /generator with its settings and its transport as
// parameters, and the editor of the window edits the same timeline: both say every change to the tree.
class Device : public QObject, public TimelineHost
{
public:
    // `persistent`: the timelines are read from the settings and kept there (the window's; --check runs without)
    explicit Device(QObject *parent = nullptr, bool persistent = false);
    ~Device() override;

    struct Timeline {
        Animation anim;             // its id, name, transport and tracks
        std::string slug;           // its node: /generator/<slug>
        std::vector<double> values; // each track's value now, after its filters
        int passesDone = 0;
        size_t builtTracks = 0;     // the track nodes made
    };
    const std::vector<Timeline> &timelines() const { return m_timelines; }
    const Timeline *timeline(quint64 id) const;
    quint64 addTimeline(const QString &name = {}); // with a track of a sine on its own output; its id
    bool removeTimeline(quint64 id);               // the last one stays
    void renameTimeline(quint64 id, const QString &name);
    quint64 shown() const; // the sub-tab shown (/generator/shown)
    void show(quint64 id);
    // The list changed (added, removed, renamed, another shown): the window's sub-tabs
    void onTimelinesChanged(std::function<void()> callback) { m_timelinesChanged = std::move(callback); }

    // A timeline as the editor reads it and writes it back (it keeps where it is: its state, its clock)
    void setTimeline(quint64 id, const Animation &edited);
    void control(quint64 id, AnimAction action, double time = 0);
    AnimationPlayback playback(quint64 id) const;
    // Told after each step (the window's playhead)
    void onTick(std::function<void()> callback) { m_ticked = std::move(callback); }
    void setDestinations(Destinations *destinations); // the track nodes say the devices' names
    Destinations *destinations() const { return m_destinations; }

    // TimelineHost: what a track drives. Holder 0 is OSCQlock's own output (its signal /generator/<slug>/tracks/<n>/value), the others are
    // the devices of Destinations
    std::vector<ParamInfo> animatableParams(quint64 holder) const override;
    bool animParamValue(quint64 holder, const QString &path, double *value) const override;
    bool animationFilterSignal(quint64 holder, quint64 timeline, const QString &path, double *input, std::vector<double> *stages) const override;

    pxs::core::tree::Tree &tree() { return m_tree; }
    // The top-level nodes that are tabs, in order (the signals are in a drawer of their own, not in a tab)
    const std::vector<std::string> &tabs() const { return m_tabs; }
    // The top-level nodes the signals drawer shows (not the generator: its timelines show theirs)
    std::vector<std::string> topLevel() const
    {
        std::vector<std::string> all(m_tabs.begin() + 1, m_tabs.end());
        all.push_back("signals");
        return all;
    }
    // Every node's address in the order it was declared: how the window orders what a node holds (the tree itself is sorted by name)
    const std::vector<std::string> &declared() const { return m_declared; }

private:
    // What the generator is: every parameter of /generator reads and writes one of these
    struct State {
        double value = 0; // what the first track of the first timeline says now (the test signals follow it)
        float x = 0.5f, y = 0.5f, r = 1.0f, g = 0.5f, b = 0.0f;
        bool tint = true;
        std::string tab = "generator";
        std::string shown; // the timeline shown, its slug
        bool drawer = false;
        int uptime = 0, counter = 0;
    };

    void buildGenerator();
    void buildValues();
    void buildTypes();
    void buildActions();
    void buildSignals();
    void buildAccess();
    void buildStructure();
    void buildUi();

    // A container, said: its label and what it is for
    pxs::core::tree::Node &group(const char *path, const char *label, const char *description = "");
    template <class T, class Configure>
    pxs::core::tree::Node &bound(const char *path, const char *label, pxs::core::tree::Parameter::Type type, T &ref, Configure configure);
    template <class Configure>
    pxs::core::tree::Node &stored(const char *path, const char *label, pxs::core::tree::Parameter::Type type, pxs::core::Value initial, Configure configure);
    pxs::core::tree::Node &raw(const char *path, const char *type, const char *label, const char *description, std::vector<pxs::core::tree::ArgRange> ranges,
                               pxs::core::ValueList initial, std::vector<std::string> extendedType = {});
    pxs::core::tree::Node &signal(const char *path, const char *type, const char *label, const char *description, std::vector<pxs::core::tree::ArgRange> ranges = {},
                                  std::vector<std::string> extendedType = {});
    void action(const char *path, const char *type, const char *label, const char *description, std::function<bool(const pxs::core::ValueList &)> handler);
    // A parameter and the nodes under it that are views of it: writing one tells the others
    void relate(const std::string &parent, const std::vector<std::string> &children);
    void said(const std::string &path, const pxs::core::ValueList &args); // the last action, on /generator/signals/last_action

    // What the parameters of a timeline say, to tell the tree which changed
    struct Said {
        bool play = false;
        int loop = 0, repeat = 0, passesDone = 0;
        double duration = 0, speed = 0;
        std::string name;
    };
    Timeline *find(quint64 id);
    Said said(const Timeline &t) const;
    void tell(const Timeline &t, const Said &before); // the tree told of what differs from `before`
    int passesDone(const Timeline &t) const;
    std::string base(const Timeline &t) const { return "/generator/" + t.slug; }
    void buildTimeline(Timeline &t);  // its nodes
    void buildTracks(Timeline &t);    // its tracks' nodes, made again when they are not as many
    void buildShown();                // /generator/shown: a choice of the timelines, made again when they change
    void timelinesChanged();
    double filtered(const Timeline &t, size_t index, double raw, double dt);
    void tick();
    void load();
    void save();          // soon (the settings are written a moment after the last edit)
    void writeSettings(); // now

    pxs::core::tree::Tree m_tree;
    std::vector<std::string> m_tabs, m_declared;
    State s;
    std::vector<Timeline> m_timelines;
    quint64 m_nextTimeline = 1;
    struct FilterState {
        InteractionProcessor processor;
        Interaction config;
        QJsonArray definition;
        double played = 0;
    };
    std::map<QString, FilterState> m_filters; // "<holder>:<timeline>:<path>", as the editor asks for them
    Destinations *m_destinations = nullptr;
    bool m_persistent = false;
    QTimer m_saveTimer;
    std::function<void()> m_ticked, m_timelinesChanged;
    QTimer m_timer;
    QElapsedTimer m_clock;
    std::mt19937 m_rng{std::random_device{}()};
    double m_up = 0; // seconds since it started
};
