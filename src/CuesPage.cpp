#include "CuesPage.h"

#include "Device.h"

#include <QApplication>
#include <QCheckBox>
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
#include <QVBoxLayout>

namespace {

enum Column { NameColumn = 0, WaitColumn, ActionColumn, TimelineColumn, ContinueColumn, ColumnCount };

const char *kAccent = "#ff8a3d";

} // namespace

CuesPage::CuesPage(Device *device, QWidget *parent) : QWidget(parent), m_device(device)
{
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 8, 0, 0);
    outer->setSpacing(10);

    // The transport of the list: GO, what is on standby, Back, Stop all
    auto *top = new QHBoxLayout;
    top->setSpacing(10);
    m_go = new QPushButton(QStringLiteral("GO"));
    m_go->setObjectName(QStringLiteral("go"));
    m_go->setToolTip(QStringLiteral("Fires the cue on standby (space bar; /cues/go)"));
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
    auto *back = new QPushButton(QStringLiteral("◀  Back"));
    back->setToolTip(QStringLiteral("The cue before is on standby (/cues/back)"));
    auto *stop = new QPushButton(QStringLiteral("■  Stop all"));
    stop->setToolTip(QStringLiteral("Every timeline stopped, and what waits forgotten (Escape; /cues/stop_all)"));
    stop->setStyleSheet(QStringLiteral("QPushButton { color: #ff6b6b; }"));
    top->addWidget(back);
    top->addWidget(stop);
    outer->addLayout(top);

    m_table = new QTableWidget(0, ColumnCount);
    m_table->setHorizontalHeaderLabels({QStringLiteral("Cue"), QStringLiteral("Wait"), QStringLiteral("Does"), QStringLiteral("Timeline"),
                                        QStringLiteral("Then")});
    m_table->horizontalHeader()->setSectionResizeMode(NameColumn, QHeaderView::Stretch);
    m_table->horizontalHeader()->setSectionResizeMode(TimelineColumn, QHeaderView::Stretch);
    m_table->setColumnWidth(WaitColumn, 90);
    m_table->setColumnWidth(ActionColumn, 120);
    m_table->setColumnWidth(ContinueColumn, 150);
    m_table->verticalHeader()->setDefaultSectionSize(34);
    m_table->verticalHeader()->setToolTip(QStringLiteral("Double-click: on standby"));
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    outer->addWidget(m_table, 1);

    auto *bottom = new QHBoxLayout;
    auto *add = new QPushButton(QStringLiteral("+ Cue"));
    add->setObjectName(QStringLiteral("accent"));
    auto *remove = new QPushButton(QStringLiteral("− Cue"));
    auto *up = new QPushButton(QStringLiteral("↑"));
    auto *down = new QPushButton(QStringLiteral("↓"));
    up->setFixedWidth(34);
    down->setFixedWidth(34);
    auto *hint = new QLabel(QStringLiteral("A cue: its wait after GO, what it does to a timeline, and whether GO goes on to the next one by itself."));
    hint->setObjectName(QStringLiteral("caption"));
    bottom->addWidget(add);
    bottom->addWidget(remove);
    bottom->addWidget(up);
    bottom->addWidget(down);
    bottom->addSpacing(10);
    bottom->addWidget(hint, 1);
    outer->addLayout(bottom);

    connect(m_go, &QPushButton::clicked, this, [this] { m_device->go(); });
    connect(back, &QPushButton::clicked, this, [this] { m_device->back(); });
    connect(stop, &QPushButton::clicked, this, [this] { m_device->stopAll(); });
    auto *space = new QShortcut(QKeySequence(Qt::Key_Space), this);
    space->setContext(Qt::WidgetWithChildrenShortcut);
    connect(space, &QShortcut::activated, this, [this] {
        if (!typing()) m_device->go();
    });
    auto *escape = new QShortcut(QKeySequence(Qt::Key_Escape), this);
    escape->setContext(Qt::WidgetWithChildrenShortcut);
    connect(escape, &QShortcut::activated, this, [this] {
        if (!typing()) m_device->stopAll();
    });
    connect(add, &QPushButton::clicked, this, [this] {
        std::vector<Device::Cue> cues = m_device->cues();
        Device::Cue c;
        c.name = QStringLiteral("Cue %1").arg(cues.size() + 1);
        c.timeline = m_device->shown();
        const int at = m_table->currentRow() >= 0 ? m_table->currentRow() + 1 : int(cues.size());
        cues.insert(cues.begin() + at, c);
        m_device->setCues(cues);
        m_table->selectRow(at);
    });
    connect(remove, &QPushButton::clicked, this, [this] {
        const int row = m_table->currentRow();
        std::vector<Device::Cue> cues = m_device->cues();
        if (row < 0 || row >= int(cues.size())) return;
        cues.erase(cues.begin() + row);
        m_device->setCues(cues);
    });
    connect(up, &QPushButton::clicked, this, [this] { move(-1); });
    connect(down, &QPushButton::clicked, this, [this] { move(1); });
    connect(m_table->verticalHeader(), &QHeaderView::sectionDoubleClicked, this, [this](int row) { m_device->setStandby(row); });
    connect(m_table, &QTableWidget::itemChanged, this, [this] {
        if (!m_filling) store();
    });

    m_device->onCuesChanged([this] {
        if (int(m_device->cues().size()) != m_table->rowCount()) rebuild();
        else refresh();
    });
    m_listener = m_device->addTimelinesListener([this] { rebuild(); }); // the timelines a cue can act on
    rebuild();
}

CuesPage::~CuesPage()
{
    m_device->onCuesChanged(nullptr);
    m_device->forgetTimelinesListener(m_listener);
}

// A cue being edited: Space is a space and Escape cancels, not GO and Stop all
bool CuesPage::typing() const
{
    const QWidget *focus = QApplication::focusWidget();
    return qobject_cast<const QLineEdit *>(focus) || // a cell being edited is a line edit that has the focus
           qobject_cast<const QAbstractSpinBox *>(focus) || qobject_cast<const QComboBox *>(focus);
}

void CuesPage::rebuild()
{
    m_filling = true;
    const auto &cues = m_device->cues();
    const auto &timelines = m_device->timelines();
    m_table->setRowCount(int(cues.size()));
    for (int row = 0; row < int(cues.size()); ++row) {
        const Device::Cue &c = cues[size_t(row)];
        auto *name = new QTableWidgetItem(c.name);
        m_table->setItem(row, NameColumn, name);

        auto *wait = new QDoubleSpinBox;
        wait->setRange(0, 3600);
        wait->setDecimals(2);
        wait->setSingleStep(0.1);
        wait->setSuffix(QStringLiteral(" s"));
        wait->setValue(c.wait);
        wait->setButtonSymbols(QAbstractSpinBox::NoButtons);
        connect(wait, &QDoubleSpinBox::editingFinished, this, [this] { store(); });
        m_table->setCellWidget(row, WaitColumn, wait);

        auto *action = new QComboBox;
        action->addItems(Device::cueActionNames());
        action->setCurrentIndex(int(c.action));
        m_table->setCellWidget(row, ActionColumn, action);

        auto *timeline = new QComboBox;
        timeline->addItem(QStringLiteral("—"), QVariant::fromValue<qulonglong>(0));
        for (const Device::Timeline &t : timelines) timeline->addItem(t.anim.name, QVariant::fromValue<qulonglong>(t.anim.id));
        timeline->setCurrentIndex(std::max(0, timeline->findData(QVariant::fromValue<qulonglong>(c.timeline))));
        timeline->setEnabled(c.action != Device::CueAction::StopAll);
        m_table->setCellWidget(row, TimelineColumn, timeline);
        connect(action, &QComboBox::currentIndexChanged, this, [this, timeline](int index) {
            timeline->setEnabled(index != int(Device::CueAction::StopAll));
            store();
        });
        connect(timeline, &QComboBox::currentIndexChanged, this, [this] { store(); });

        auto *cont = new QCheckBox(QStringLiteral("go on to the next"));
        cont->setChecked(c.autoContinue);
        cont->setToolTip(QStringLiteral("GO fires this one and the next together (its own wait counts from GO)"));
        connect(cont, &QCheckBox::toggled, this, [this] { store(); });
        m_table->setCellWidget(row, ContinueColumn, cont);
    }
    m_filling = false;
    refresh();
}

void CuesPage::refresh()
{
    const auto &cues = m_device->cues();
    const int standby = m_device->standby();
    for (int row = 0; row < m_table->rowCount(); ++row) {
        auto *head = m_table->verticalHeaderItem(row);
        if (!head) {
            head = new QTableWidgetItem;
            m_table->setVerticalHeaderItem(row, head);
        }
        head->setText(row == standby ? QStringLiteral("▶ %1").arg(row + 1) : QString::number(row + 1));
        head->setForeground(row == standby ? QColor(kAccent) : QColor(0x8a, 0x8e, 0x97));
        if (QTableWidgetItem *name = m_table->item(row, NameColumn)) {
            QFont f = name->font();
            f.setBold(row == standby);
            const QSignalBlocker block(m_table);
            name->setFont(f);
            name->setForeground(row == standby ? QColor(kAccent) : QColor(0xe6, 0xe8, 0xec));
        }
    }
    const bool any = standby < int(cues.size());
    m_go->setEnabled(any);
    m_next->setText(any ? QStringLiteral("%1   %2").arg(standby + 1).arg(cues[size_t(standby)].name) : QStringLiteral("— end of the list —"));
    m_state->setText(m_device->pending() ? QStringLiteral("%1 cue(s) waiting to act").arg(m_device->pending())
                                         : QStringLiteral("%1 cue(s)").arg(cues.size()));
}

void CuesPage::store()
{
    if (m_filling) return;
    std::vector<Device::Cue> cues = m_device->cues();
    for (int row = 0; row < m_table->rowCount() && row < int(cues.size()); ++row) {
        Device::Cue &c = cues[size_t(row)];
        if (const QTableWidgetItem *name = m_table->item(row, NameColumn)) c.name = name->text();
        if (auto *wait = qobject_cast<QDoubleSpinBox *>(m_table->cellWidget(row, WaitColumn))) c.wait = wait->value();
        if (auto *action = qobject_cast<QComboBox *>(m_table->cellWidget(row, ActionColumn))) c.action = Device::CueAction(action->currentIndex());
        if (auto *timeline = qobject_cast<QComboBox *>(m_table->cellWidget(row, TimelineColumn))) c.timeline = timeline->currentData().toULongLong();
        if (auto *cont = qobject_cast<QCheckBox *>(m_table->cellWidget(row, ContinueColumn))) c.autoContinue = cont->isChecked();
    }
    m_device->setCues(cues);
}

void CuesPage::move(int by)
{
    const int row = m_table->currentRow();
    std::vector<Device::Cue> cues = m_device->cues();
    const int to = row + by;
    if (row < 0 || to < 0 || to >= int(cues.size())) return;
    std::swap(cues[size_t(row)], cues[size_t(to)]);
    m_device->setCues(cues);
    rebuild();
    m_table->selectRow(to);
}
