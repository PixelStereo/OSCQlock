#pragma once
// The window of the device, made from its tree: every top-level node is a tab, every container below it a section, and every leaf a row with
// the widget of its kind (the fields of pxs-qt for the parameters, buttons for the actions, meters and readouts for the signals). Nothing here
// knows the device: whatever the tree holds is shown, so the window cannot drift from the namespace.
//
// A widget writes the way an OSC message does (Tree::apply), and is told of every change by the tree's observer, from OSC, from another
// widget or from the device itself: nothing is polled.
#include <pxs/core/tree/tree.h>

#include <QWidget>

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

class QFormLayout;
class QTabWidget;

class Panel : public QWidget
{
public:
    // Controls: a tab for each top-level node that has something to set or to send (the signals are not here). Signals: one page with
    // only the signals, grouped by the nodes that hold them
    enum class Mode { Controls, Signals };

    // `sections`: the names of the top-level nodes, in order. In Controls mode the node "/ui/tab" (a choice of those names, if the tree has
    // one) is the current tab. `order`: the addresses of the nodes in the order they should be shown (what is not in it comes after, by name)
    Panel(pxs::core::tree::Tree &tree, const std::vector<std::string> &sections, const std::vector<std::string> &order = {},
          Mode mode = Mode::Controls, QWidget *parent = nullptr);
    ~Panel() override;
    // A widget of the application's own at the top of a tab (Controls mode): `section` is the top-level node's name
    void addHead(const std::string &section, QWidget *widget);

private:
    using Show = std::function<void(const pxs::core::ValueList &)>;

    bool relevant(const pxs::core::tree::Node &node) const; // is there something of this panel in it
    void fill(QFormLayout *form, const pxs::core::tree::Node &node);
    void addRow(QFormLayout *form, const pxs::core::tree::Node &node);
    QWidget *editorFor(const pxs::core::tree::Node &node);
    QWidget *actionFor(const pxs::core::tree::Node &node);
    QWidget *viewFor(const pxs::core::tree::Node &node);
    void send(const pxs::core::tree::Node &node, const pxs::core::ValueList &args);

    pxs::core::tree::Tree &m_tree;
    Mode m_mode;
    std::vector<std::string> m_tabs; // the tabs that were made, by name
    std::unordered_map<std::string, int> m_order;
    std::unordered_map<std::string, QFormLayout *> m_forms; // each tab's rows, by its name
    QTabWidget *m_tabWidget = nullptr;
    int m_observer = 0;
    std::unordered_map<const pxs::core::tree::Node *, std::vector<Show>> m_show;
};
