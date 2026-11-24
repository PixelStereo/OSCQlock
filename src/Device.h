#pragma once
// The device: a generator (a sine, a saw, a triangle, a square or noise) that says everything about itself in every kind of node the
// pxs-core tree has, so that a controller, an explorer or a mapper can be tried on all of them. The namespace is the contract, and it is
// made so that each top-level node (a tab of the window, see Panel) shows one case:
//
//   /generator   a node that holds nodes that hold parameters, actions, signals, or a mix: what a device is
//   /values      a parameter that holds its components, with an action and a switch under it; a colour; a point; plain groups of numbers
//   /types       one parameter of each kind: switch, whole number (any, one among a few), choice, float, double, 64 bits, text, blob
//   /actions     nodes that are only actions: with no value, with one of each kind, with several; and a parameter that holds actions
//   /signals     nodes that are only signals, of the kinds the generator does not say; one that holds another. Like the signals under
//                /generator and /structure it is shown in the drawer of the window, not in a tab
//   /access      who can do what: read only, write only, locked, never saved, critical, tagged, a trigger
//   /structure   nodes that are only nodes: empty, deep, side by side, and one that holds a parameter, an action and a signal
//   /ui          the window's own state, driven by OSC like any parameter: its tab (and the actions that move it), the drawer of the signals
#include <pxs/core/tree/tree.h>

#include <QElapsedTimer>
#include <QObject>
#include <QTimer>

#include <random>
#include <string>
#include <vector>

class Device : public QObject
{
public:
    explicit Device(QObject *parent = nullptr);

    pxs::core::tree::Tree &tree() { return m_tree; }
    // The top-level nodes that are tabs, in order (the signals are in a drawer of their own, not in a tab)
    const std::vector<std::string> &tabs() const { return m_tabs; }
    // Every top-level node, the tabs and then the signals
    std::vector<std::string> topLevel() const
    {
        std::vector<std::string> all = m_tabs;
        all.push_back("signals");
        return all;
    }
    // Every node's address in the order it was declared: how the window orders what a node holds (the tree itself is sorted by name)
    const std::vector<std::string> &declared() const { return m_declared; }

private:
    // What the generator is: every parameter of /generator reads and writes one of these
    struct State {
        bool play = false, flash = false;
        std::string type = "sine", label = "OSCQlock";
        double frequency = 1.0, amplitude = 1.0, offset = 0.0, shift = 0.0;
        int loops = 0, steps = 4, loopCount = 0;
        double pos = 0, seconds = 0, value = 0; // where it is in the loop, since when it plays, what it says
        bool above = false;
        float x = 0.5f, y = 0.5f, r = 1.0f, g = 0.5f, b = 0.0f;
        bool tint = true;
        std::string tab = "generator";
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

    double wave(double phase);
    void tick();

    pxs::core::tree::Tree m_tree;
    std::vector<std::string> m_tabs, m_declared;
    State s;
    QTimer m_timer;
    QElapsedTimer m_clock;
    std::mt19937 m_rng{std::random_device{}()};
    double m_up = 0; // seconds since it started
};
