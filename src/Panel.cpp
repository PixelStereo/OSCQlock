#include "Panel.h"

#include <pxs/core/tree/parameter.h>

#include <Fields.h>
#include <ColorPicker.h>
#include <pxs/SignalMeter.h>

#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QFrame>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QRegularExpressionValidator>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QTabWidget>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

using namespace pxs::core;
using namespace pxs::core::tree;

namespace {

QString fromStd(const std::string &s) { return QString::fromStdString(s); }

// What a client calls a unit, as a short suffix for a field
QString suffixOf(const std::string &unit)
{
    static const std::pair<const char *, const char *> known[] = {
        {"frequency.hertz", " Hz"}, {"angle.degree", "°"}, {"time.second", " s"}, {"time.millisecond", " ms"},
        {"ratio.percent", " %"},    {"distance.m", " m"},  {"gain.db", " dB"},
    };
    for (const auto &k : known)
        if (unit == k.first) return QString::fromUtf8(k.second);
    return {};
}

QString hexOf(const Value &v)
{
    const Blob *b = v.get<Blob>();
    if (!b) return {};
    QByteArray bytes(reinterpret_cast<const char *>(b->data()), int(b->size()));
    return QString::fromLatin1(bytes.toHex(' '));
}

Blob blobOf(const QString &text)
{
    const QByteArray bytes = QByteArray::fromHex(text.toLatin1());
    return Blob(bytes.begin(), bytes.end());
}

const char *kindName(Kind k)
{
    switch (k) {
    case Kind::Parameter: return "parameter";
    case Kind::Action: return "action";
    case Kind::Signal: return "signal";
    default: return "node";
    }
}

// The value of one argument of this tag, from what a number says
Value valueOf(char tag, double n)
{
    switch (tag) {
    case 'i': return Value(int(std::lround(n)));
    case 'h': return Value(std::int64_t(std::llround(n)));
    case 'd': return Value(n);
    default: return Value(float(n));
    }
}

// One argument of a node: its widget, and how to show a value in it
struct Editor {
    QWidget *widget = nullptr;
    std::function<void(const Value &)> show;
};

// `edited` is told what the user did. `range` may be absent (any number), `choices` are the whole numbers or keys offered, when there are some
Editor argumentEditor(char tag, const ArgRange *range, const std::string &unit, const std::function<void(const Value &)> &edited)
{
    Editor e;
    const bool bounded = range && range->min && range->max;
    switch (tag) {
    case 'T':
    case 'F': {
        auto *box = new QCheckBox;
        QObject::connect(box, &QCheckBox::toggled, box, [edited](bool on) { edited(Value(on)); });
        e.widget = box;
        e.show = [box](const Value &v) {
            const QSignalBlocker block(box);
            box->setChecked(v.truthy());
        };
        break;
    }
    case 'i':
        if (range && !range->vals.empty()) {
            auto *combo = new QComboBox;
            for (const Value &v : range->vals) combo->addItem(fromStd(v.text()), fromStd(v.text()));
            QObject::connect(combo, QOverload<int>::of(&QComboBox::activated), combo, [combo, edited](int) { edited(Value(combo->currentData().toInt())); });
            e.widget = combo;
            e.show = [combo](const Value &v) {
                const QSignalBlocker block(combo);
                combo->setCurrentIndex(std::max(0, combo->findData(fromStd(Value(int(v.number().value_or(0))).text()))));
            };
        } else if (bounded) {
            auto *field = new SliderField;
            field->setRange(range->min->number().value_or(0), range->max->number().value_or(1));
            field->setDecimals(0);
            field->setSingleStep(1);
            QObject::connect(field, &SliderField::valueEdited, field, [edited](double v) { edited(Value(int(std::lround(v)))); });
            e.widget = field;
            e.show = [field](const Value &v) {
                if (!field->isEditing()) field->setValue(v.number().value_or(0));
            };
        } else {
            auto *box = new IntBox;
            box->setRange(-1000000000, 1000000000);
            QObject::connect(box, QOverload<int>::of(&QSpinBox::valueChanged), box, [edited](int v) { edited(Value(v)); });
            e.widget = box;
            e.show = [box](const Value &v) {
                const QSignalBlocker block(box);
                box->setValue(int(v.number().value_or(0)));
            };
        }
        break;
    case 'f':
    case 'd':
        if (bounded) {
            auto *field = new SliderField;
            field->setRange(range->min->number().value_or(0), range->max->number().value_or(1));
            const double span = field->maximum() - field->minimum();
            field->setDecimals(span > 100 ? 1 : 2);
            field->setSingleStep(span / 100);
            field->setSuffix(suffixOf(unit));
            QObject::connect(field, &SliderField::valueEdited, field, [edited, tag](double v) { edited(valueOf(tag, v)); });
            e.widget = field;
            e.show = [field](const Value &v) {
                if (!field->isEditing()) field->setValue(v.number().value_or(0));
            };
        } else {
            auto *box = new NumberBox;
            box->setRange(-1e9, 1e9);
            box->setDecimals(4);
            QObject::connect(box, QOverload<double>::of(&QDoubleSpinBox::valueChanged), box, [edited, tag](double v) { edited(valueOf(tag, v)); });
            e.widget = box;
            e.show = [box](const Value &v) {
                const QSignalBlocker block(box);
                box->setValue(v.number().value_or(0));
            };
        }
        break;
    case 'h': {
        auto *edit = new QLineEdit;
        edit->setValidator(new QRegularExpressionValidator(QRegularExpression("-?[0-9]{0,18}"), edit));
        QObject::connect(edit, &QLineEdit::editingFinished, edit, [edit, edited] { edited(Value(std::int64_t(edit->text().toLongLong()))); });
        e.widget = edit;
        e.show = [edit](const Value &v) {
            if (!edit->hasFocus()) edit->setText(fromStd(v.text()));
        };
        break;
    }
    case 'b': {
        auto *edit = new QLineEdit;
        edit->setPlaceholderText("hexadecimal bytes");
        QObject::connect(edit, &QLineEdit::editingFinished, edit, [edit, edited] { edited(Value(blobOf(edit->text()))); });
        e.widget = edit;
        e.show = [edit](const Value &v) {
            if (!edit->hasFocus()) edit->setText(hexOf(v));
        };
        break;
    }
    default: { // 's' and what is not known: a text
        auto *edit = new QLineEdit;
        QObject::connect(edit, &QLineEdit::editingFinished, edit, [edit, edited] { edited(Value(edit->text().toStdString())); });
        e.widget = edit;
        e.show = [edit](const Value &v) {
            if (!edit->hasFocus()) edit->setText(fromStd(v.text()));
        };
        break;
    }
    }
    return e;
}

bool isColor(const Node &node)
{
    return !node.extendedType.empty() && node.extendedType.front().rfind("color.", 0) == 0 && (node.type == "fff" || node.type == "ffff");
}

// A node's name and what it is, to the left of its widget
QWidget *captionFor(const Node &node)
{
    auto *box = new QWidget;
    auto *layout = new QVBoxLayout(box);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(new QLabel(fromStd(node.displayLabel())));
    QString what = fromStd(std::string(kindName(node.kind())));
    if (!node.type.empty()) what += " · " + fromStd(node.type);
    if (node.kind() == Kind::Parameter) {
        if (node.access == 1) what += " · read only";
        else if (node.access == 2) what += " · write only";
    }
    if (node.locked) what += " · locked";
    auto *caption = new QLabel(what);
    caption->setObjectName("caption");
    layout->addWidget(caption);
    QString tip = "<b>" + fromStd(node.path()).toHtmlEscaped() + "</b><br>" + what;
    if (!node.description.empty()) tip += "<br>" + fromStd(node.description).toHtmlEscaped();
    if (!node.tags.empty()) {
        QStringList tags;
        for (const std::string &t : node.tags) tags << fromStd(t);
        tip += "<br>tags: " + tags.join(", ");
    }
    box->setToolTip(tip);
    return box;
}

} // namespace

Panel::Panel(Tree &tree, const std::vector<std::string> &sections, const std::vector<std::string> &order, Mode mode, QWidget *parent)
    : QWidget(parent), m_tree(tree), m_mode(mode)
{
    for (std::size_t i = 0; i < order.size(); ++i) m_order[order[i]] = int(i);
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    const auto pageOf = [](QFormLayout *&form) {
        auto *page = new QWidget;
        form = new QFormLayout(page);
        form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
        form->setLabelAlignment(Qt::AlignLeft | Qt::AlignTop);
        auto *scroll = new QScrollArea;
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->setWidget(page);
        return scroll;
    };

    if (m_mode == Mode::Signals) { // one page: each top-level node that holds signals, with them
        QFormLayout *form = nullptr;
        outer->addWidget(pageOf(form));
        for (const std::string &name : sections) {
            const Node *node = tree.find("/" + name);
            if (node && relevant(*node)) addRow(form, *node);
        }
    } else {
        m_tabWidget = new QTabWidget;
        outer->addWidget(m_tabWidget);
        for (const std::string &name : sections) {
            const Node *node = tree.find("/" + name);
            if (!node || !relevant(*node)) continue;
            QFormLayout *form = nullptr;
            QScrollArea *scroll = pageOf(form);
            fill(form, *node);
            m_forms[name] = form;
            m_tabWidget->addTab(scroll, fromStd(node->displayLabel()));
            m_tabs.push_back(name);
        }
        // The tab is the node "/ui/tab": the user's click writes it, and a write to it (OSC, another widget) moves the tab
        if (const Node *tab = tree.find("/ui/tab")) {
            QObject::connect(m_tabWidget, &QTabWidget::currentChanged, this, [this](int index) {
                if (index >= 0 && index < int(m_tabs.size())) send(*m_tree.find("/ui/tab"), {Value(m_tabs[std::size_t(index)])});
            });
            m_show[tab].push_back([this](const ValueList &v) {
                if (v.empty()) return;
                const auto at = std::find(m_tabs.begin(), m_tabs.end(), v.front().text());
                if (at == m_tabs.end()) return;
                const QSignalBlocker block(m_tabWidget);
                m_tabWidget->setCurrentIndex(int(at - m_tabs.begin()));
            });
            if (tab->get) m_show[tab].back()(tab->get());
        }
    }

    m_observer = m_tree.addObserver([this](const Node &node, const ValueList &values) {
        const auto found = m_show.find(&node);
        if (found == m_show.end()) return;
        for (const Show &show : found->second) show(values);
    });
}

void Panel::addHead(const std::string &section, QWidget *widget)
{
    const auto found = m_forms.find(section);
    if (found != m_forms.end()) found->second->insertRow(0, widget);
}

Panel::~Panel()
{
    m_tree.removeObserver(m_observer);
}

// What each mode shows: the controls, everything but the signals (and the nodes that are empty to begin with: they are part of the
// structure to see); the signals, only them, and the nodes that hold some
bool Panel::relevant(const Node &node) const
{
    const bool only = m_mode == Mode::Signals;
    if (!node.isContainer()) return only ? node.kind() == Kind::Signal : node.kind() != Kind::Signal;
    bool listed = false;
    for (const auto &entry : node.children()) {
        if (entry.second->attached) continue;
        listed = true;
        if (relevant(*entry.second)) return true;
    }
    return !only && !listed;
}

// The rows of a node's children, in the order they were declared
void Panel::fill(QFormLayout *form, const Node &node)
{
    std::vector<const Node *> children;
    for (const auto &entry : node.children())
        if (!entry.second->attached && relevant(*entry.second)) children.push_back(entry.second.get());
    const auto rank = [this](const Node *n) {
        const auto found = m_order.find(n->path());
        return found == m_order.end() ? int(m_order.size()) : found->second;
    };
    std::stable_sort(children.begin(), children.end(), [&](const Node *a, const Node *b) { return rank(a) < rank(b); });
    for (const Node *child : children) addRow(form, *child);
}

void Panel::addRow(QFormLayout *form, const Node &node)
{
    bool holds = false; // it holds nodes this panel shows
    for (const auto &entry : node.children())
        if (!entry.second->attached && relevant(*entry.second)) holds = true;
    if (node.isContainer()) {
        auto *section = new QGroupBox(fromStd(node.displayLabel()));
        section->setObjectName("section");
        section->setToolTip("<b>" + fromStd(node.path()).toHtmlEscaped() + "</b><br>node" + (node.description.empty() ? QString() : "<br>" + fromStd(node.description).toHtmlEscaped()));
        auto *inner = new QFormLayout(section);
        inner->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
        inner->setLabelAlignment(Qt::AlignLeft | Qt::AlignTop);
        if (!node.description.empty()) {
            auto *about = new QLabel(fromStd(node.description));
            about->setObjectName("caption");
            about->setWordWrap(true);
            inner->addRow(about);
        } else if (!holds && !node.hasListedChildren()) {
            auto *about = new QLabel("an empty node");
            about->setObjectName("caption");
            inner->addRow(about);
        }
        fill(inner, node);
        form->addRow(section);
        return;
    }
    QWidget *widget = node.kind() == Kind::Action ? actionFor(node) : node.kind() == Kind::Signal || node.access == 1 ? viewFor(node) : editorFor(node);
    if (!node.description.empty()) { // what it is for, under its widget
        auto *both = new QWidget;
        auto *stack = new QVBoxLayout(both);
        stack->setContentsMargins(0, 0, 0, 0);
        stack->setSpacing(2);
        stack->addWidget(widget);
        auto *about = new QLabel(fromStd(node.description));
        about->setObjectName("caption");
        about->setWordWrap(true);
        stack->addWidget(about);
        widget = both;
    }
    form->addRow(captionFor(node), widget);
    if (holds) { // a value that holds nodes: they are its components, or what goes with it
        auto *section = new QGroupBox("Under " + fromStd(node.displayLabel()));
        section->setObjectName("section");
        auto *inner = new QFormLayout(section);
        inner->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
        inner->setLabelAlignment(Qt::AlignLeft | Qt::AlignTop);
        fill(inner, node);
        form->addRow(section);
    }
}

void Panel::send(const Node &node, const ValueList &args)
{
    if (m_tree.apply(node.path(), args) > 0) return;
    // Refused (locked, or a value that does not fit): the widget goes back to what the node holds
    const auto found = m_show.find(&node);
    if (found != m_show.end() && node.get)
        for (const Show &show : found->second) show(node.get());
}

// A parameter: the widget of its kind, one per argument
QWidget *Panel::editorFor(const Node &node)
{
    auto *host = new QWidget;
    auto *row = new QHBoxLayout(host);
    row->setContentsMargins(0, 0, 0, 0);
    const Node *self = &node;
    const ParamInfo info = node.parameter() ? node.parameter()->info() : ParamInfo{};
    std::vector<Show> shows;

    if (node.parameter() && info.type == ParamInfo::Type::Choice) { // one key among a list
        auto *combo = new QComboBox;
        for (const std::string &key : info.choices) combo->addItem(fromStd(key), fromStd(key));
        QObject::connect(combo, QOverload<int>::of(&QComboBox::activated), combo, [this, self, combo](int) { send(*self, {Value(combo->currentData().toString().toStdString())}); });
        row->addWidget(combo);
        shows.push_back([combo](const ValueList &v) {
            const QSignalBlocker block(combo);
            combo->setCurrentIndex(std::max(0, combo->findData(fromStd(v.empty() ? std::string() : v.front().text()))));
        });
    } else if (isColor(node)) { // a colour: the picker of pxs-qt
        auto *picker = new ColorPicker(node.type == "ffff");
        const bool alpha = node.type == "ffff";
        QObject::connect(picker, &ColorPicker::colorEdited, picker, [this, self, alpha](const QColor &c) {
            ValueList v{Value(float(c.redF())), Value(float(c.greenF())), Value(float(c.blueF()))};
            if (alpha) v.push_back(Value(float(c.alphaF())));
            send(*self, v);
        });
        row->addWidget(picker);
        shows.push_back([picker, alpha](const ValueList &v) {
            if (v.size() < 3) return;
            QColor c = QColor::fromRgbF(float(v[0].number().value_or(0)), float(v[1].number().value_or(0)), float(v[2].number().value_or(0)),
                                        alpha && v.size() > 3 ? float(v[3].number().value_or(1)) : 1.0f);
            picker->setColor(c);
        });
    } else {
        // The tags are the type of each argument; a Bool parameter says "T"; one of several arguments is written with the others
        auto current = std::make_shared<ValueList>(node.get ? node.get() : ValueList{});
        for (std::size_t i = 0; i < node.type.size(); ++i) {
            const char tag = node.type[i];
            const ArgRange *range = i < node.range.size() ? &node.range[i] : nullptr;
            ArgRange among;
            if (node.parameter() && info.type == ParamInfo::Type::Int && !info.values.empty()) {
                for (int v : info.values) among.vals.push_back(Value(v));
                range = &among;
            }
            Editor e = argumentEditor(tag, range, i < node.unit.size() ? node.unit[i] : info.unit, [this, self, current, i](const Value &v) {
                if (current->size() <= i) current->resize(i + 1);
                (*current)[i] = v;
                send(*self, *current);
            });
            row->addWidget(e.widget);
            shows.push_back([e, current, i](const ValueList &v) {
                *current = v;
                if (i < v.size()) e.show(v[i]);
            });
        }
        if (node.type.empty()) row->addWidget(new QLabel("(no value)"));
    }

    // Every widget of the node follows what it holds (and its lock)
    QWidget *lockable = host;
    m_show[self].push_back([shows, self, lockable](const ValueList &v) {
        for (const Show &s : shows) s(v);
        lockable->setEnabled(!self->locked);
    });
    if (node.get && (node.access & 1)) m_show[self].back()(node.get());
    else lockable->setEnabled(!node.locked);
    row->addStretch(0);
    return host;
}

// An action: the arguments it takes, and the button that sends it
QWidget *Panel::actionFor(const Node &node)
{
    auto *host = new QWidget;
    auto *row = new QHBoxLayout(host);
    row->setContentsMargins(0, 0, 0, 0);
    const Node *self = &node;
    auto arguments = std::make_shared<ValueList>();
    const std::size_t count = node.type == "N" ? 0 : node.type.size();
    arguments->resize(count);
    for (std::size_t i = 0; i < count; ++i) {
        const char tag = node.type[i];
        // What is sent when nothing was typed: a neutral value of the kind
        (*arguments)[i] = tag == 'i' ? Value(0) : tag == 'h' ? Value(std::int64_t(0)) : tag == 'd' ? Value(0.0) : tag == 'f' ? Value(0.0f)
                          : tag == 'T' ? Value(false) : tag == 'b' ? Value(Blob{}) : Value(std::string());
        const ArgRange *range = i < node.range.size() ? &node.range[i] : nullptr;
        ArgRange unit01;
        if (!range && (tag == 'f' || tag == 'd')) {
            unit01.min = Value(0.0);
            unit01.max = Value(1.0);
            range = &unit01;
        }
        Editor e = argumentEditor(tag, range, std::string(), [arguments, i](const Value &v) { (*arguments)[i] = v; });
        row->addWidget(e.widget);
    }
    auto *button = new QPushButton(count ? "Send" : fromStd(node.displayLabel()));
    QObject::connect(button, &QPushButton::clicked, button, [this, self, arguments] { send(*self, *arguments); });
    row->addWidget(button);
    row->addStretch(0);
    return host;
}

// What a signal or a read only parameter shows: a meter for a number with a range, a lamp for an impulse, the numbers or the text otherwise
QWidget *Panel::viewFor(const Node &node)
{
    const Node *self = &node;
    const std::string &type = node.type;
    const ArgRange *range = !node.range.empty() ? &node.range.front() : nullptr;
    const bool bounded = range && range->min && range->max;
    QWidget *widget = nullptr;
    Show show;
    if (type == "N") { // an impulse: a lamp that stays lit a moment
        auto *lamp = new QLabel(QString::fromUtf8("○"));
        auto *off = new QTimer(lamp);
        off->setSingleShot(true);
        off->setInterval(150);
        QObject::connect(off, &QTimer::timeout, lamp, [lamp] { lamp->setText(QString::fromUtf8("○")); });
        widget = lamp;
        show = [lamp, off](const ValueList &) {
            lamp->setText(QString::fromUtf8("●"));
            off->start();
        };
    } else if (type == "T") {
        auto *lamp = new QLabel(QString::fromUtf8("○ off"));
        widget = lamp;
        show = [lamp](const ValueList &v) { lamp->setText(!v.empty() && v.front().truthy() ? QString::fromUtf8("● on") : QString::fromUtf8("○ off")); };
    } else if ((type == "f" || type == "d" || type == "i") && bounded) {
        auto *meter = new pxs::filterview::SignalMeter;
        meter->setRange(0, 1000);
        const double lo = range->min->number().value_or(0), hi = range->max->number().value_or(1);
        meter->setTextVisible(true);
        meter->setMinimumWidth(160);
        widget = meter;
        const int decimals = type == "i" ? 0 : 3;
        show = [meter, lo, hi, decimals](const ValueList &v) {
            if (v.empty()) return;
            const double n = v.front().number().value_or(lo);
            meter->setValue(int(std::clamp((n - lo) / (hi - lo), 0.0, 1.0) * 1000));
            meter->setFormat(QString::number(n, 'f', decimals));
        };
    } else if (isColor(node)) {
        auto *swatch = new QFrame;
        swatch->setFixedSize(48, 18);
        swatch->setAutoFillBackground(true);
        widget = swatch;
        show = [swatch](const ValueList &v) {
            if (v.size() < 3) return;
            QPalette p = swatch->palette();
            p.setColor(QPalette::Window, QColor::fromRgbF(float(v[0].number().value_or(0)), float(v[1].number().value_or(0)), float(v[2].number().value_or(0))));
            swatch->setPalette(p);
        };
    } else { // numbers and text, as they come
        auto *text = new QLabel;
        text->setTextInteractionFlags(Qt::TextSelectableByMouse);
        widget = text;
        show = [text](const ValueList &v) {
            QStringList parts;
            for (const Value &a : v) parts << (a.is<Blob>() ? hexOf(a) : a.is<float>() || a.is<double>() ? QString::number(a.number().value_or(0), 'f', 3) : fromStd(a.text()));
            const QString now = parts.join("  ");
            if (text->text() != now) text->setText(now);
        };
    }
    m_show[self].push_back(show);
    if (node.get && (node.access & 1)) show(node.get());
    return widget;
}
