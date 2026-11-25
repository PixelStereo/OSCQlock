#include "DevicesWindow.h"

#include "Destinations.h"

#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStackedWidget>
#include <QTableWidget>
#include <QVBoxLayout>

namespace {

enum Page { Empty = 0, OscQueryPage, OscPage };
enum Column { PathColumn = 0, TypeColumn, MinColumn, MaxColumn };

QComboBox *typeBox(const QString &type)
{
    auto *box = new QComboBox;
    box->addItem(QStringLiteral("number (f)"), QStringLiteral("f"));
    box->addItem(QStringLiteral("whole number (i)"), QStringLiteral("i"));
    box->addItem(QStringLiteral("on / off (T, F)"), QStringLiteral("T"));
    box->setCurrentIndex(std::max(0, box->findData(type)));
    return box;
}

QDoubleSpinBox *numberBox(double v)
{
    auto *box = new QDoubleSpinBox;
    box->setRange(-1e6, 1e6);
    box->setDecimals(3);
    box->setValue(v);
    box->setButtonSymbols(QAbstractSpinBox::NoButtons);
    return box;
}

} // namespace

DevicesWindow::DevicesWindow(Destinations *destinations, QWidget *parent) : QWidget(parent, Qt::Window), m_d(destinations)
{
    setWindowTitle(QStringLiteral("OSCQlock — Devices"));
    resize(820, 480);
    auto *root = new QHBoxLayout(this);
    root->setSpacing(12);

    // Left: the devices, and how to make one
    auto *left = new QVBoxLayout;
    auto *title = new QLabel(QStringLiteral("<b>Devices</b>"));
    left->addWidget(title);
    auto *hint = new QLabel(QStringLiteral("What the tracks of the timelines can drive. The OSCQuery devices of the network appear by themselves."));
    hint->setWordWrap(true);
    hint->setObjectName(QStringLiteral("caption"));
    left->addWidget(hint);
    m_list = new QListWidget;
    m_list->setMinimumWidth(260);
    left->addWidget(m_list, 1);
    auto *buttons = new QHBoxLayout;
    auto *addOsc = new QPushButton(QStringLiteral("+ OSC"));
    addOsc->setObjectName(QStringLiteral("accent"));
    addOsc->setToolTip(QStringLiteral("A plain OSC receiver: a host, a UDP port, and the addresses it takes"));
    auto *addQuery = new QPushButton(QStringLiteral("+ OSCQuery…"));
    addQuery->setToolTip(QStringLiteral("An OSCQuery device that is not announced: its address and its HTTP port"));
    auto *remove = new QPushButton(QStringLiteral("Remove"));
    buttons->addWidget(addOsc);
    buttons->addWidget(addQuery);
    buttons->addStretch(1);
    buttons->addWidget(remove);
    left->addLayout(buttons);
    root->addLayout(left);

    // Right: the selected one
    m_pages = new QStackedWidget;
    auto *empty = new QLabel(QStringLiteral("Select a device, or make one."));
    empty->setAlignment(Qt::AlignCenter);
    empty->setObjectName(QStringLiteral("caption"));
    m_pages->addWidget(empty);

    auto *query = new QWidget;
    auto *qv = new QVBoxLayout(query);
    m_queryTitle = new QLabel;
    m_queryInfo = new QLabel;
    m_queryInfo->setWordWrap(true);
    m_queryInfo->setTextInteractionFlags(Qt::TextSelectableByMouse);
    auto *reread = new QPushButton(QStringLiteral("Read its namespace again"));
    qv->addWidget(m_queryTitle);
    qv->addWidget(m_queryInfo);
    qv->addWidget(reread, 0, Qt::AlignLeft);
    qv->addStretch(1);
    m_pages->addWidget(query);

    auto *osc = new QWidget;
    auto *ov = new QVBoxLayout(osc);
    auto *form = new QFormLayout;
    m_oscName = new QLineEdit;
    m_oscHost = new QLineEdit;
    m_oscHost->setPlaceholderText(QStringLiteral("127.0.0.1, or a name"));
    m_oscPort = new QSpinBox;
    m_oscPort->setRange(1, 65535);
    form->addRow(QStringLiteral("Name"), m_oscName);
    form->addRow(QStringLiteral("Host"), m_oscHost);
    form->addRow(QStringLiteral("UDP port"), m_oscPort);
    ov->addLayout(form);
    auto *addrTitle = new QLabel(QStringLiteral("<b>Addresses</b> — what it takes (a track drives one of them)"));
    ov->addWidget(addrTitle);
    m_addresses = new QTableWidget(0, 4);
    m_addresses->setHorizontalHeaderLabels({QStringLiteral("Address"), QStringLiteral("Type"), QStringLiteral("Min"), QStringLiteral("Max")});
    m_addresses->horizontalHeader()->setSectionResizeMode(PathColumn, QHeaderView::Stretch);
    m_addresses->verticalHeader()->hide();
    m_addresses->setSelectionBehavior(QAbstractItemView::SelectRows);
    ov->addWidget(m_addresses, 1);
    auto *rows = new QHBoxLayout;
    auto *addAddress = new QPushButton(QStringLiteral("+ Address"));
    auto *removeAddress = new QPushButton(QStringLiteral("− Address"));
    rows->addWidget(addAddress);
    rows->addWidget(removeAddress);
    rows->addStretch(1);
    ov->addLayout(rows);
    m_pages->addWidget(osc);
    root->addWidget(m_pages, 1);

    connect(m_list, &QListWidget::currentRowChanged, this, [this] { showSelected(); });
    connect(addOsc, &QPushButton::clicked, this, [this] {
        const quint64 id = m_d->addOsc(QStringLiteral("OSC device"), QStringLiteral("127.0.0.1"), 9001);
        m_d->setAddresses(id, {OscAddress{QStringLiteral("/value"), QStringLiteral("f"), 0, 1}});
        refresh();
        for (int i = 0; i < m_list->count(); ++i)
            if (m_list->item(i)->data(Qt::UserRole).toULongLong() == id) m_list->setCurrentRow(i);
        m_oscName->setFocus();
        m_oscName->selectAll();
    });
    connect(addQuery, &QPushButton::clicked, this, [this] {
        bool ok = false;
        const QString text = QInputDialog::getText(this, QStringLiteral("An OSCQuery device"), QStringLiteral("Its address and HTTP port (host:port)"),
                                                   QLineEdit::Normal, QStringLiteral("127.0.0.1:5678"), &ok);
        if (!ok) return;
        const QString host = text.section(':', 0, -2).trimmed();
        const int port = text.section(':', -1).toInt();
        if (host.isEmpty() || port <= 0 || port > 65535) return;
        m_d->addOscQuery(host, quint16(port));
    });
    connect(remove, &QPushButton::clicked, this, [this] {
        if (const quint64 id = selected()) m_d->remove(id);
    });
    connect(reread, &QPushButton::clicked, this, [this] {
        if (const quint64 id = selected()) m_d->refresh(id);
    });
    for (QLineEdit *e : {m_oscName, m_oscHost}) connect(e, &QLineEdit::editingFinished, this, [this] { storeOsc(); });
    connect(m_oscPort, &QSpinBox::editingFinished, this, [this] { storeOsc(); });
    connect(addAddress, &QPushButton::clicked, this, [this] {
        const int row = m_addresses->rowCount();
        m_filling = true;
        m_addresses->insertRow(row);
        m_addresses->setItem(row, PathColumn, new QTableWidgetItem(QStringLiteral("/value%1").arg(row + 1)));
        auto *type = typeBox(QStringLiteral("f"));
        connect(type, &QComboBox::currentIndexChanged, this, [this] { storeAddresses(); });
        m_addresses->setCellWidget(row, TypeColumn, type);
        for (int c : {MinColumn, MaxColumn}) {
            auto *box = numberBox(c == MinColumn ? 0 : 1);
            connect(box, &QDoubleSpinBox::editingFinished, this, [this] { storeAddresses(); });
            m_addresses->setCellWidget(row, c, box);
        }
        m_filling = false;
        storeAddresses();
        m_addresses->editItem(m_addresses->item(row, PathColumn));
    });
    connect(removeAddress, &QPushButton::clicked, this, [this] {
        const int row = m_addresses->currentRow();
        if (row < 0) return;
        m_addresses->removeRow(row);
        storeAddresses();
    });
    connect(m_addresses, &QTableWidget::itemChanged, this, [this] {
        if (!m_filling) storeAddresses();
    });
    connect(m_d, &Destinations::changed, this, [this] { refresh(); });
    refresh();
}

quint64 DevicesWindow::selected() const
{
    const QListWidgetItem *item = m_list->currentItem();
    return item ? item->data(Qt::UserRole).toULongLong() : 0;
}

void DevicesWindow::refresh()
{
    const quint64 keep = selected();
    const QSignalBlocker block(m_list);
    m_list->clear();
    for (const Destination *d : m_d->all()) {
        const bool osc = d->kind == Destination::Kind::Osc;
        const QString state = osc ? QStringLiteral("● OSC") : d->online ? QStringLiteral("● OSCQuery") : QStringLiteral("○ OSCQuery");
        auto *item = new QListWidgetItem(QStringLiteral("%1\n%2   %3:%4").arg(d->name, state, d->host).arg(d->port));
        item->setData(Qt::UserRole, QVariant::fromValue<qulonglong>(d->id));
        if (!osc && !d->online) item->setForeground(QColor(0x8a, 0x8a, 0x90));
        m_list->addItem(item);
        if (d->id == keep) m_list->setCurrentItem(item);
    }
    showSelected();
}

void DevicesWindow::showSelected()
{
    const Destination *d = m_d->find(selected());
    if (!d) {
        m_pages->setCurrentIndex(Empty);
        return;
    }
    m_filling = true;
    if (d->kind == Destination::Kind::OscQuery) {
        m_pages->setCurrentIndex(OscQueryPage);
        m_queryTitle->setText(QStringLiteral("<b>%1</b>").arg(d->name.toHtmlEscaped()));
        const size_t numbers = m_d->params(d->id).size();
        m_queryInfo->setText(QStringLiteral("OSCQuery at http://%1:%2/ — %3\n%4 number(s) a track can drive.%5")
                                 .arg(d->host)
                                 .arg(d->port)
                                 .arg(d->online ? QStringLiteral("its namespace is read") : QStringLiteral("not reached"))
                                 .arg(numbers)
                                 .arg(d->manual ? QStringLiteral("\nAdded by hand: kept between launches.")
                                                : QStringLiteral("\nFound on the network: found again at each launch.")));
    } else {
        m_pages->setCurrentIndex(OscPage);
        if (!m_oscName->hasFocus()) m_oscName->setText(d->name);
        if (!m_oscHost->hasFocus()) m_oscHost->setText(d->host);
        if (!m_oscPort->hasFocus()) m_oscPort->setValue(d->port);
        if (m_tableFor != d->id || m_addresses->rowCount() != int(d->addresses.size())) {
            m_tableFor = d->id;
            m_addresses->setRowCount(0);
            for (const OscAddress &a : d->addresses) {
                const int row = m_addresses->rowCount();
                m_addresses->insertRow(row);
                m_addresses->setItem(row, PathColumn, new QTableWidgetItem(a.path));
                auto *type = typeBox(a.type);
                connect(type, &QComboBox::currentIndexChanged, this, [this] { storeAddresses(); });
                m_addresses->setCellWidget(row, TypeColumn, type);
                auto *lo = numberBox(a.min), *hi = numberBox(a.max);
                connect(lo, &QDoubleSpinBox::editingFinished, this, [this] { storeAddresses(); });
                connect(hi, &QDoubleSpinBox::editingFinished, this, [this] { storeAddresses(); });
                m_addresses->setCellWidget(row, MinColumn, lo);
                m_addresses->setCellWidget(row, MaxColumn, hi);
            }
        }
    }
    m_filling = false;
}

void DevicesWindow::storeOsc()
{
    const quint64 id = selected();
    if (!id || m_filling) return;
    m_d->setOsc(id, m_oscName->text(), m_oscHost->text(), quint16(m_oscPort->value()));
}

void DevicesWindow::storeAddresses()
{
    const quint64 id = selected();
    if (!id || m_filling) return;
    std::vector<OscAddress> list;
    for (int row = 0; row < m_addresses->rowCount(); ++row) {
        const QTableWidgetItem *path = m_addresses->item(row, PathColumn);
        QString p = path ? path->text().trimmed() : QString();
        if (p.isEmpty()) continue;
        if (!p.startsWith('/')) p.prepend('/');
        OscAddress a;
        a.path = p;
        if (auto *type = qobject_cast<QComboBox *>(m_addresses->cellWidget(row, TypeColumn))) a.type = type->currentData().toString();
        if (auto *lo = qobject_cast<QDoubleSpinBox *>(m_addresses->cellWidget(row, MinColumn))) a.min = lo->value();
        if (auto *hi = qobject_cast<QDoubleSpinBox *>(m_addresses->cellWidget(row, MaxColumn))) a.max = hi->value();
        list.push_back(a);
    }
    m_d->setAddresses(id, list);
}
