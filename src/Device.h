#pragma once
// OSCQlock: timelines that drive the devices of the network (OSC and OSCQuery), played by hand, by a cue list or by OSC. Everything it is
// and does is a node of its own tree (an OSCQuery device itself), so that a controller drives it as it drives anything else:
//
//   /generator   the timelines: as many as one wants, each a sub-tab of the first tab and a node /generator/<slug> (its name, transport,
//                passes, tracks, signals). /generator/shown is the sub-tab shown; /generator/new, new_named, remove make and remove them.
//                Their tracks drive OSCQlock's own outputs or the devices of the network (Destinations.h), through the filters of pxs-core
//   /cues        the cue list: go, back, stop_all, fire (a cue by its number); standby (the next one), its name, the one fired
//   /ui          the window's own state: its tab (and the actions that move it)
//
// A show (the timelines, the cues, the devices made by hand) is saved in a file (show()/loadShow()) and kept between launches.
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
#include <QByteArray>
#include <QJsonObject>
#include <QStringList>

#include <algorithm>
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
    void restoreTimeline(const Animation &timeline, int index); // one removed, back at its place (undo), with its id
    void renameTimeline(quint64 id, const QString &name);
    quint64 shown() const; // the sub-tab shown (/generator/shown)
    void show(quint64 id);
    // The list changed (added, removed, renamed, another shown): the window's sub-tabs, the cues' timelines. An id for forgetTimelinesListener
    int addTimelinesListener(std::function<void()> callback)
    {
        m_timelinesListeners.emplace_back(++m_lastListener, std::move(callback));
        return m_lastListener;
    }
    void forgetTimelinesListener(int id)
    {
        m_timelinesListeners.erase(std::remove_if(m_timelinesListeners.begin(), m_timelinesListeners.end(), [id](const auto &l) { return l.first == id; }),
                                   m_timelinesListeners.end());
    }

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

    // ---- the cue list: GO fires the cue on standby (after its wait), and the next one too when it continues by itself

    enum class CueAction { Start = 0, Play, Pause, Stop, StopAll }; // Start: from the beginning
    struct Cue {
        quint64 id = 0;
        QString name;
        double wait = 0;          // seconds between GO and what it does
        quint64 timeline = 0;     // what it acts on (StopAll: every one)
        CueAction action = CueAction::Start;
        bool autoContinue = false; // GO goes on to the next cue at once (its own wait counts from there)
    };
    const std::vector<Cue> &cues() const { return m_cues; }
    void setCues(const std::vector<Cue> &cues); // as the window edits them (ids kept; a new one has id 0)
    int standby() const { return m_standby; }   // the index of the next cue GO fires (cues().size(): past the end)
    void setStandby(int index);
    void go();
    void back();     // the cue before is on standby
    void stopAll();  // every timeline stopped, nothing pending
    bool fire(int index); // that cue, now (after its wait), standby unchanged
    int pending() const { return int(m_pending.size()); } // cues waiting to act
    static QStringList cueActionNames(); // "Start", "Play"…
    // The list, the standby, a cue fired: the window's
    void onCuesChanged(std::function<void()> callback) { m_cuesChanged = std::move(callback); }

    // ---- a show: the timelines, the cues, and (with Destinations) the devices made by hand

    QJsonObject show() const;
    bool loadShow(const QJsonObject &show, QString *error = nullptr); // replaces the timelines and the cues
    // What a show file holds (the timelines, the cues, the devices) differs from when it was last saved or opened: a play or a seek does
    // not count
    bool modified() const;
    void setSaved(); // what it holds now is what is saved

    pxs::core::tree::Tree &tree() { return m_tree; }
    // The top-level nodes that are tabs, in order (the signals are in a drawer of their own, not in a tab)
    const std::vector<std::string> &tabs() const { return m_tabs; }
    // Every node's address in the order it was declared: how the window orders what a node holds (the tree itself is sorted by name)
    const std::vector<std::string> &declared() const { return m_declared; }

private:
    // What the generator is: every parameter of /generator reads and writes one of these
    struct State {
        std::string tab = "generator";
        std::string shown; // the timeline shown, its slug
    };

    void buildGenerator();
    void buildCues();
    void buildUi();

    // A container, said: its label and what it is for
    pxs::core::tree::Node &group(const char *path, const char *label, const char *description = "");
    template <class T, class Configure>
    pxs::core::tree::Node &bound(const char *path, const char *label, pxs::core::tree::Parameter::Type type, T &ref, Configure configure);
    pxs::core::tree::Node &signal(const char *path, const char *type, const char *label, const char *description, std::vector<pxs::core::tree::ArgRange> ranges = {},
                                  std::vector<std::string> extendedType = {});
    void action(const char *path, const char *type, const char *label, const char *description, std::function<bool(const pxs::core::ValueList &)> handler);
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
    void stepCues(double dt);
    void cuesChanged();       // the nodes of /cues told, the window, the settings
    void act(const Cue &cue); // what a cue does, now
    QJsonArray timelinesJson() const;
    QJsonArray cuesJson() const;
    void readTimelines(const QJsonArray &list); // into m_timelines, with their nodes
    void readCues(const QJsonArray &list);
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
    std::function<void()> m_ticked, m_cuesChanged;
    std::vector<std::pair<int, std::function<void()>>> m_timelinesListeners;
    int m_lastListener = 0;
    void tellTimelines() const
    {
        for (const auto &l : std::vector(m_timelinesListeners)) l.second();
    }
    std::vector<Cue> m_cues;
    int m_standby = 0;
    quint64 m_nextCue = 1;
    std::vector<std::pair<double, Cue>> m_pending; // seconds left, the cue
    std::string m_fired;                           // the name of the last cue fired
    QByteArray snapshot() const; // what a show file holds, compact
    QByteArray m_saved;          // the snapshot when it was last saved or opened
    mutable bool m_stale = true, m_differs = false;
    QTimer m_timer;
    QElapsedTimer m_clock;
    std::mt19937 m_rng{std::random_device{}()};
    double m_up = 0; // seconds since it started
};
