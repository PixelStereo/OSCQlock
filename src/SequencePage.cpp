#include "SequencePage.h"

#include "Commands.h"

#include <QApplication>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QShortcut>
#include <QSignalBlocker>
#include <QTableWidget>
#include <QTimer>
#include <QUndoStack>
#include <QVBoxLayout>

namespace {

enum Column { TextColumn = 0, PreWaitColumn, ActionColumn, TimelineColumn, ValueColumn, PostWaitColumn, NextColumn, ColumnCount };

const QColor kStandby(232, 76, 61); // the step GO plays (NyxMap's)
const QColor kPlayed(76, 217, 100); // the step played

// What a step does to a timeline, in the order of AnimAction
const QStringList kActions = {QStringLiteral("Play"),   QStringLiteral("Pause"),     QStringLiteral("Stop"), QStringLiteral("Rewind"),
                              QStringLiteral("Seek"),   QStringLiteral("Loop mode"), QStringLiteral("Speed")};

QDoubleSpinBox *secondsBox(double v, const QString &suffix = QStringLiteral(" s"), double max = 3600)
{
    auto *box = new QDoubleSpinBox;
    box->setRange(0, max);
    box->setDecimals(2);
    box->setSingleStep(0.1);
    box->setSuffix(suffix);
    box->setValue(v);
    box->setButtonSymbols(QAbstractSpinBox::NoButtons);
    return box;
}

} // namespace

SequencePage::SequencePage(Device *device, QUndoStack *undo, QWidget *parent) : QWidget(parent), m_device(device), m_undo(undo)
{
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 8, 0, 0);
    outer->setSpacing(10);

    // The transport of the list: GO, the step on standby, Back, Stop, Stop all
    auto *top = new QHBoxLayout;
    top->setSpacing(10);
    m_go = new QPushButton(QStringLiteral("GO"));
    m_go->setObjectName(QStringLiteral("go"));
    m_go->setToolTip(QStringLiteral("Plays the step on standby (space bar; /sequence/go)"));
    m_go->setMinimumSize(140, 64);
    m_go->setStyleSheet(QStringLiteral("QPushButton#go { background: #2fa45a; color: #ffffff; font-size: 26px; font-weight: 800; letter-spacing: 2px;"
                                       " border: none; border-radius: 10px; }"
                                       "QPushButton#go:hover { background: #38bd69; }"
                                       "QPushButton#go:pressed { background: #23864a; }"
                                       "QPushButton#go:disabled { background: #2b2e34; color: #6b6f78; }"));
    top->addWidget(m_go);
    auto *standby = new QVBoxLayout;
    auto *standbyTitle = new QLabel(QStringLiteral("STANDBY"));
    standbyTitle->setStyleSheet(QStringLiteral("color: #8a8e97; font-size: 10px; letter-spacing: 1.5px;"));
    m_next = new QLabel;
    m_next->setStyleSheet(QStringLiteral("font-size: 20px; font-weight: 600;"));
    m_state = new QLabel;
    m_state->setObjectName(QStringLiteral("caption"));
    standby->addWidget(standbyTitle);
    standby->addWidget(m_next);
    standby->addWidget(m_state);
    top->addLayout(standby, 1);
    auto *back = new QPushButton(QStringLiteral("◀  GO BACK"));
    back->setToolTip(QStringLiteral("The step before, at once (/sequence/back)"));
    auto *stop = new QPushButton(QStringLiteral("Stop"));
    stop->setToolTip(QStringLiteral("The waits and follows on their way stopped; the timelines go on (/sequence/stop)"));
    auto *stopAll = new QPushButton(QStringLiteral("■  Stop all"));
    stopAll->setToolTip(QStringLiteral("Every wait and follow, and every timeline, stopped (Escape; /sequence/stop_all)"));
    stopAll->setStyleSheet(QStringLiteral("QPushButton { color: #ff6b6b; }"));
    top->addWidget(back);
    top->addWidget(stop);
    top->addWidget(stopAll);
    outer->addLayout(top);

    m_table = new QTableWidget(0, ColumnCount);
    m_table->setHorizontalHeaderLabels({QStringLiteral("Step"), QStringLiteral("Pre-wait"), QStringLiteral("Does"), QStringLiteral("Timeline"),
                                        QStringLiteral("Value"), QStringLiteral("Post-wait"), QStringLiteral("Then")});
    m_table->horizontalHeader()->setSectionResizeMode(TextColumn, QHeaderView::Stretch);
    m_table->horizontalHeader()->setSectionResizeMode(TimelineColumn, QHeaderView::Stretch);
    for (const auto &[c, w] : std::initializer_list<std::pair<int, int>>{{PreWaitColumn, 84}, {ActionColumn, 110}, {ValueColumn, 84}, {PostWaitColumn, 84}, {NextColumn, 120}})
        m_table->setColumnWidth(c, w);
    m_table->verticalHeader()->setDefaultSectionSize(34);
    m_table->verticalHeader()->setToolTip(QStringLiteral("Double-click: play this step (/sequence/step)"));
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    outer->addWidget(m_table, 1);

    auto *bottom = new QHBoxLayout;
    auto *add = new QPushButton(QStringLiteral("+ Step"));
    add->setObjectName(QStringLiteral("accent"));
    auto *remove = new QPushButton(QStringLiteral("− Step"));
    auto *up = new QPushButton(QStringLiteral("↑"));
    auto *down = new QPushButton(QStringLiteral("↓"));
    up->setFixedWidth(34);
    down->setFixedWidth(34);
    auto *hint = new QLabel(QStringLiteral("NyxMap's sequencer: a step's pre-wait after its GO, what it does to a timeline, its post-wait, and whether the "
                                           "next one waits for GO or follows."));
    hint->setObjectName(QStringLiteral("caption"));
    hint->setWordWrap(true);
    bottom->addWidget(add);
    bottom->addWidget(remove);
    bottom->addWidget(up);
    bottom->addWidget(down);
    bottom->addSpacing(10);
    bottom->addWidget(hint, 1);
    outer->addLayout(bottom);

    Sequencer &seq = m_device->sequencer();
    connect(m_go, &QPushButton::clicked, this, [&seq] { seq.go(); });
    connect(back, &QPushButton::clicked, this, [&seq] { seq.back(); });
    connect(stop, &QPushButton::clicked, this, [&seq] { seq.stop(); });
    connect(stopAll, &QPushButton::clicked, this, [this] { m_device->stopAll(); });
    auto *space = new QShortcut(QKeySequence(Qt::Key_Space), this);
    space->setContext(Qt::WidgetWithChildrenShortcut);
    connect(space, &QShortcut::activated, this, [this, &seq] {
        if (!typing()) seq.go();
    });
    auto *escape = new QShortcut(QKeySequence(Qt::Key_Escape), this);
    escape->setContext(Qt::WidgetWithChildrenShortcut);
    connect(escape, &QShortcut::activated, this, [this] {
        if (!typing()) m_device->stopAll();
    });
    connect(add, &QPushButton::clicked, this, [this] {
        Sequencer::Sequence s = sequence();
        Sequencer::Step st;
        st.description = QStringLiteral("Step %1").arg(s.steps.size() + 1);
        st.timeline = m_device->shown();
        st.action = AnimAction::Play;
        const int at = m_table->currentRow() >= 0 ? m_table->currentRow() + 1 : int(s.steps.size());
        s.steps.insert(s.steps.begin() + at, st);
        apply(s, QStringLiteral("New Step"));
        m_table->selectRow(at);
    });
    connect(remove, &QPushButton::clicked, this, [this] {
        const int row = m_table->currentRow();
        Sequencer::Sequence s = sequence();
        if (row < 0 || row >= int(s.steps.size())) return;
        s.steps.erase(s.steps.begin() + row);
        apply(s, QStringLiteral("Remove Step"));
    });
    connect(up, &QPushButton::clicked, this, [this] { move(-1); });
    connect(down, &QPushButton::clicked, this, [this] { move(1); });
    connect(m_table->verticalHeader(), &QHeaderView::sectionDoubleClicked, this, [&seq](int row) { seq.goTo(row); });
    connect(m_table, &QTableWidget::itemChanged, this, [this] {
        if (!m_filling) store();
    });

    connect(&seq, &Sequencer::changed, this, [this] {
        if (int(sequence().steps.size()) != m_table->rowCount()) rebuild();
        else refresh();
    });
    connect(&seq, &Sequencer::positionChanged, this, [this] { refresh(); });
    m_listener = m_device->addTimelinesListener([this] { rebuild(); }); // the timelines a step can drive
    auto *poll = new QTimer(this); // the waits on their way
    poll->start(100);
    connect(poll, &QTimer::timeout, this, [this] { refresh(); });
    rebuild();
}

SequencePage::~SequencePage()
{
    m_device->forgetTimelinesListener(m_listener);
}

Sequencer::Sequence SequencePage::sequence() const
{
    const Sequencer &seq = m_device->sequencer();
    return seq.sequence(std::max(0, seq.current()));
}

// A step being edited: Space is a space and Escape cancels, not GO and Stop all
bool SequencePage::typing() const
{
    const QWidget *focus = QApplication::focusWidget();
    return qobject_cast<const QLineEdit *>(focus) || qobject_cast<const QAbstractSpinBox *>(focus) || qobject_cast<const QComboBox *>(focus);
}

void SequencePage::rebuild()
{
    m_filling = true;
    const Sequencer::Sequence s = sequence();
    const auto &timelines = m_device->timelines();
    m_table->setRowCount(int(s.steps.size()));
    for (int row = 0; row < int(s.steps.size()); ++row) {
        const Sequencer::Step &st = s.steps[size_t(row)];
        const bool group = !st.actions.empty();
        m_table->setItem(row, TextColumn, new QTableWidgetItem(st.description));

        auto *pre = secondsBox(st.preWait);
        connect(pre, &QDoubleSpinBox::editingFinished, this, [this] { store(); });
        m_table->setCellWidget(row, PreWaitColumn, pre);
        auto *post = secondsBox(st.postWait);
        connect(post, &QDoubleSpinBox::editingFinished, this, [this] { store(); });
        m_table->setCellWidget(row, PostWaitColumn, post);

        auto *action = new QComboBox;
        auto *timeline = new QComboBox;
        auto *value = secondsBox(st.action == AnimAction::Speed ? st.speed : st.seekTime, st.action == AnimAction::Speed ? QStringLiteral(" ×") : QStringLiteral(" s"));
        if (group) { // made of several actions (an OSCQlock before NyxMap's sequencer): shown, not edited here
            action->addItem(QStringLiteral("%1 actions").arg(st.actions.size()));
            action->setEnabled(false);
            timeline->addItem(QStringLiteral("—"));
            timeline->setEnabled(false);
            value->setEnabled(false);
        } else {
            action->addItems(kActions);
            action->setCurrentIndex(std::clamp(int(st.action), 0, int(kActions.size()) - 1));
            timeline->addItem(QStringLiteral("—"), QVariant::fromValue<qulonglong>(0));
            for (const Device::Timeline &t : timelines) timeline->addItem(t.anim.name, QVariant::fromValue<qulonglong>(t.anim.id));
            timeline->setCurrentIndex(std::max(0, timeline->findData(QVariant::fromValue<qulonglong>(st.timeline))));
            value->setEnabled(st.action == AnimAction::Seek || st.action == AnimAction::Speed);
            if (st.action == AnimAction::Speed) value->setRange(0, 10);
            connect(action, &QComboBox::currentIndexChanged, this, [this] { store(); });
            connect(timeline, &QComboBox::currentIndexChanged, this, [this] { store(); });
            connect(value, &QDoubleSpinBox::editingFinished, this, [this] { store(); });
        }
        m_table->setCellWidget(row, ActionColumn, action);
        m_table->setCellWidget(row, TimelineColumn, timeline);
        m_table->setCellWidget(row, ValueColumn, value);

        auto *next = new QComboBox;
        next->addItems({QStringLiteral("Wait for GO"), QStringLiteral("Auto follow")});
        next->setCurrentIndex(st.next == Sequencer::StepContinue::AutoFollow ? 1 : 0);
        next->setToolTip(QStringLiteral("Auto follow: the next step gets its GO once this one's action and post-wait are over"));
        connect(next, &QComboBox::currentIndexChanged, this, [this] { store(); });
        m_table->setCellWidget(row, NextColumn, next);
    }
    m_filling = false;
    refresh();
}

void SequencePage::refresh()
{
    const Sequencer &seq = m_device->sequencer();
    const Sequencer::Sequence s = sequence();
    const int next = seq.next(), played = seq.position();
    for (int row = 0; row < m_table->rowCount(); ++row) {
        auto *head = m_table->verticalHeaderItem(row);
        if (!head) {
            head = new QTableWidgetItem;
            m_table->setVerticalHeaderItem(row, head);
        }
        head->setText(row == next ? QStringLiteral("▶ %1").arg(row + 1) : QString::number(row + 1));
        head->setForeground(row == next ? kStandby : row == played ? kPlayed : QColor(0x8a, 0x8e, 0x97));
        if (QTableWidgetItem *text = m_table->item(row, TextColumn)) {
            const QSignalBlocker block(m_table);
            QFont f = text->font();
            f.setBold(row == next);
            text->setFont(f);
            text->setForeground(row == next ? kStandby : row == played ? kPlayed : QColor(0xe6, 0xe8, 0xec));
        }
    }
    m_go->setEnabled(next >= 0);
    m_next->setText(next >= 0 && next < int(s.steps.size()) ? QStringLiteral("%1   %2").arg(next + 1).arg(s.steps[size_t(next)].description)
                                                            : QStringLiteral("— end of the sequence —"));
    const auto runs = seq.runs();
    QString state = QStringLiteral("%1 step(s)").arg(s.steps.size());
    if (!runs.empty()) {
        const Sequencer::Run &r = runs.back();
        const char *phase = r.elapsed < r.preWait ? "pre-wait" : r.elapsed < r.preWait + r.duration ? "action" : "post-wait";
        state = QStringLiteral("step %1: %2, %3 s").arg(r.step + 1).arg(QString::fromLatin1(phase)).arg(r.elapsed, 0, 'f', 1);
    }
    m_state->setText(state);
}

void SequencePage::store()
{
    if (m_filling) return;
    Sequencer::Sequence s = sequence();
    for (int row = 0; row < m_table->rowCount() && row < int(s.steps.size()); ++row) {
        Sequencer::Step &st = s.steps[size_t(row)];
        if (const QTableWidgetItem *text = m_table->item(row, TextColumn)) st.description = text->text();
        if (auto *pre = qobject_cast<QDoubleSpinBox *>(m_table->cellWidget(row, PreWaitColumn))) st.preWait = pre->value();
        if (auto *post = qobject_cast<QDoubleSpinBox *>(m_table->cellWidget(row, PostWaitColumn))) st.postWait = post->value();
        if (auto *next = qobject_cast<QComboBox *>(m_table->cellWidget(row, NextColumn)))
            st.next = next->currentIndex() == 1 ? Sequencer::StepContinue::AutoFollow : Sequencer::StepContinue::Wait;
        if (!st.actions.empty()) continue; // a group: its actions as they are
        if (auto *action = qobject_cast<QComboBox *>(m_table->cellWidget(row, ActionColumn))) st.action = AnimAction(action->currentIndex());
        if (auto *timeline = qobject_cast<QComboBox *>(m_table->cellWidget(row, TimelineColumn))) st.timeline = timeline->currentData().toULongLong();
        if (auto *value = qobject_cast<QDoubleSpinBox *>(m_table->cellWidget(row, ValueColumn))) {
            if (st.action == AnimAction::Speed) st.speed = value->value();
            if (st.action == AnimAction::Seek) st.seekTime = value->value();
        }
    }
    apply(s, QStringLiteral("Edit Step"));
    QTimer::singleShot(0, this, [this] { rebuild(); }); // the value's unit and range follow the action (not from inside its widget)
}

void SequencePage::apply(const Sequencer::Sequence &after, const QString &text)
{
    const Sequencer::Sequence before = sequence();
    QJsonArray a, b;
    for (const auto &st : before.steps) a.append(Sequencer::stepToJson(st));
    for (const auto &st : after.steps) b.append(Sequencer::stepToJson(st));
    if (a == b) return; // nothing changed: no step
    const int index = std::max(0, m_device->sequencer().current());
    if (m_undo) m_undo->push(new cmd::SetSequence(m_device, index, before, after, text));
    else m_device->sequencer().setSequence(index, after);
}

void SequencePage::move(int by)
{
    const int row = m_table->currentRow();
    Sequencer::Sequence s = sequence();
    const int to = row + by;
    if (row < 0 || to < 0 || to >= int(s.steps.size())) return;
    std::swap(s.steps[size_t(row)], s.steps[size_t(to)]);
    apply(s, QStringLiteral("Move Step"));
    rebuild();
    m_table->selectRow(to);
}
