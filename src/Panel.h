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
    // `tabs`: the names of the top-level nodes, in the order of the tabs. The node "/ui/tab" (a choice of those names, if the tree has one) is the current tab
    // `order`: the addresses of the nodes in the order they should be shown (what is not in it comes after, by name)
    Panel(pxs::core::tree::Tree &tree, const std::vector<std::string> &tabs, const std::vector<std::string> &order = {}, QWidget *parent = nullptr);
    ~Panel() override;

private:
    using Show = std::function<void(const pxs::core::ValueList &)>;

    void fill(QFormLayout *form, const pxs::core::tree::Node &node);
    void addRow(QFormLayout *form, const pxs::core::tree::Node &node);
    QWidget *editorFor(const pxs::core::tree::Node &node);
    QWidget *actionFor(const pxs::core::tree::Node &node);
    QWidget *viewFor(const pxs::core::tree::Node &node);
    void send(const pxs::core::tree::Node &node, const pxs::core::ValueList &args);

    pxs::core::tree::Tree &m_tree;
    std::vector<std::string> m_tabs;
    std::unordered_map<std::string, int> m_order;
    QTabWidget *m_tabWidget = nullptr;
    int m_observer = 0;
    std::unordered_map<const pxs::core::tree::Node *, std::vector<Show>> m_show;
};
