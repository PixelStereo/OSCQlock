#include "Destinations.h"

#include <pxs/Client.h>
#include <pxs/Discovery.h>

#include <OscCodec.h>

#include <QHostInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>

#include <algorithm>
#include <cmath>

using pxs::oscquery::Client;
using pxs::oscquery::RemoteNode;

QString Destination::key() const
{
    if (kind == Kind::Osc) return QStringLiteral("osc:") + name;
    return manual ? QStringLiteral("oscquery:%1:%2").arg(host).arg(port) : QStringLiteral("oscquery:") + name;
}

Destinations::Destinations(const QString &selfName, quint16 selfPort, bool persistent, QObject *parent)
    : QObject(parent), m_selfName(selfName), m_selfPort(selfPort), m_persistent(persistent)
{
    m_udp.bind(QHostAddress::AnyIPv4, 0);
    if (m_persistent) load();
}

Destinations::~Destinations() = default;

void Destinations::startDiscovery()
{
    if (m_discovery) return;
    m_discovery = new pxs::oscquery::Discovery(this);
    connect(m_discovery, &pxs::oscquery::Discovery::deviceFound, this, [this](const pxs::oscquery::Device &d) { found(d); });
    connect(m_discovery, &pxs::oscquery::Discovery::deviceLost, this, [this](const pxs::oscquery::Device &d) {
        for (auto &[id, e] : m_entries)
            if (e->d.kind == Destination::Kind::OscQuery && !e->d.manual && e->d.name == d.name) {
                e->d.online = false;
                emit changed();
            }
    });
    m_discovery->start();
}

void Destinations::found(const pxs::oscquery::Device &device)
{
    // Not itself: its own name, or its own port on this computer
    if (device.name == m_selfName || (device.port == m_selfPort && (device.address.isLoopback() || QHostInfo::localHostName().startsWith(device.host.section('.', 0, 0)))))
        return;
    for (auto &[id, e] : m_entries)
        if (e->d.kind == Destination::Kind::OscQuery && !e->d.manual && e->d.name == device.name) {
            const QString host = device.address.isNull() ? device.host : device.address.toString();
            if (e->d.host != host || e->d.port != device.port || !e->client) {
                e->d.host = host;
                e->d.port = device.port;
                open(*e);
            }
            return;
        }
    Destination d;
    d.kind = Destination::Kind::OscQuery;
    d.name = device.name;
    d.host = device.address.isNull() ? device.host : device.address.toString();
    d.port = device.port;
    open(make(d));
    emit changed();
}

quint64 Destinations::idFor(const QString &key)
{
    const auto it = m_ids.constFind(key);
    if (it != m_ids.constEnd()) return *it;
    const quint64 id = m_nextId++;
    m_ids.insert(key, id);
    return id;
}

Destinations::Entry &Destinations::make(Destination d)
{
    d.id = idFor(d.key());
    auto e = std::make_unique<Entry>();
    e->d = std::move(d);
    Entry &ref = *e;
    m_entries[ref.d.id] = std::move(e);
    save();
    return ref;
}

void Destinations::open(Entry &e)
{
    if (e.d.kind != Destination::Kind::OscQuery) return;
    if (!e.client) {
        e.client = std::make_unique<Client>();
        Client *c = e.client.get();
        const quint64 id = e.d.id;
        connect(c, &Client::ready, this, [this, id, c] {
            if (Entry *x = entry(id)) {
                x->d.online = true;
                if (x->d.manual && !c->name().isEmpty()) x->d.name = c->name();
            }
            emit changed();
        });
        connect(c, &Client::treeChanged, this, &Destinations::changed);
        connect(c, &Client::failed, this, [this, id](const QString &) {
            if (Entry *x = entry(id)) x->d.online = false;
            emit changed();
        });
    }
    e.d.online = false;
    e.client->open(e.d.host, e.d.port);
}

void Destinations::resolve(Entry &e)
{
    e.address = QHostAddress();
    if (e.d.host.isEmpty()) return;
    QHostAddress direct;
    if (direct.setAddress(e.d.host)) {
        e.address = direct;
        return;
    }
    const quint64 id = e.d.id;
    const QString host = e.d.host;
    QHostInfo::lookupHost(host, this, [this, id, host](const QHostInfo &info) {
        Entry *x = entry(id);
        if (!x || x->d.host != host) return;
        for (const QHostAddress &a : info.addresses())
            if (a.protocol() == QAbstractSocket::IPv4Protocol) {
                x->address = a;
                break;
            }
    });
}

std::vector<const Destination *> Destinations::all() const
{
    std::vector<const Destination *> out;
    for (const auto &[id, e] : m_entries) out.push_back(&e->d);
    std::sort(out.begin(), out.end(), [](const Destination *a, const Destination *b) {
        if (a->kind != b->kind) return a->kind == Destination::Kind::OscQuery;
        return a->name.compare(b->name, Qt::CaseInsensitive) < 0;
    });
    return out;
}

Destinations::Entry *Destinations::entry(quint64 id)
{
    const auto it = m_entries.find(id);
    return it == m_entries.end() ? nullptr : it->second.get();
}

const Destinations::Entry *Destinations::entry(quint64 id) const
{
    const auto it = m_entries.find(id);
    return it == m_entries.end() ? nullptr : it->second.get();
}

const Destination *Destinations::find(quint64 id) const
{
    const Entry *e = entry(id);
    return e ? &e->d : nullptr;
}

QString Destinations::nameOf(quint64 id) const
{
    if (id == 0) return QStringLiteral("OSCQlock");
    const Entry *e = entry(id);
    return e ? e->d.name : QString();
}

quint64 Destinations::addOscQuery(const QString &host, quint16 httpPort)
{
    Destination d;
    d.kind = Destination::Kind::OscQuery;
    d.manual = true;
    d.host = host.trimmed();
    d.port = httpPort;
    d.name = QStringLiteral("%1:%2").arg(d.host).arg(httpPort);
    for (auto &[id, e] : m_entries)
        if (e->d.key() == d.key()) return id;
    Entry &e = make(d);
    open(e);
    emit changed();
    return e.d.id;
}

quint64 Destinations::addOsc(const QString &name, const QString &host, quint16 port)
{
    Destination d;
    d.kind = Destination::Kind::Osc;
    d.name = name.trimmed();
    for (int n = 2; std::any_of(m_entries.begin(), m_entries.end(), [&](const auto &x) { return x.second->d.key() == d.key(); }); ++n)
        d.name = QStringLiteral("%1 %2").arg(name.trimmed()).arg(n);
    d.host = host.trimmed();
    d.port = port;
    d.online = true;
    Entry &e = make(d);
    resolve(e);
    emit changed();
    return e.d.id;
}

void Destinations::setOsc(quint64 id, const QString &name, const QString &host, quint16 port)
{
    Entry *e = entry(id);
    if (!e || e->d.kind != Destination::Kind::Osc) return;
    const QString was = e->d.key();
    e->d.name = name.trimmed().isEmpty() ? e->d.name : name.trimmed();
    if (e->d.key() != was) { // its id goes with its new name
        m_ids.remove(was);
        m_ids.insert(e->d.key(), id);
    }
    const bool moved = e->d.host != host.trimmed();
    e->d.host = host.trimmed();
    e->d.port = port;
    if (moved) resolve(*e);
    e->last.clear();
    save();
    emit changed();
}

void Destinations::setAddresses(quint64 id, const std::vector<OscAddress> &addresses)
{
    Entry *e = entry(id);
    if (!e || e->d.kind != Destination::Kind::Osc) return;
    e->d.addresses = addresses;
    e->last.clear();
    save();
    emit changed();
}

void Destinations::remove(quint64 id)
{
    if (!m_entries.erase(id)) return;
    save();
    emit changed();
}

void Destinations::refresh(quint64 id)
{
    if (Entry *e = entry(id)) {
        if (e->d.kind == Destination::Kind::OscQuery) open(*e);
        else resolve(*e);
    }
    emit changed();
}

namespace {

// The leaves of one number that can be written, as the timeline editor names them ("Mixer › Fader 1"), by their path in the device
void collect(const RemoteNode &node, const QString &category, std::vector<ParamInfo> &out)
{
    for (const auto &[name, child] : node.children) {
        pxs::core::tree::ParamInfo core;
        if (child.isLeaf() && (child.access & 2) && child.kind != QLatin1String("signal") && child.toParamInfo(&core) &&
            (core.type == pxs::core::tree::ParamInfo::Type::Float || core.type == pxs::core::tree::ParamInfo::Type::Int ||
             core.type == pxs::core::tree::ParamInfo::Type::Bool)) {
            ParamInfo p = ParamInfo::fromCore(core);
            p.path = child.path;
            p.label = category.isEmpty() ? child.name : category + QStringLiteral(" › ") + child.name;
            p.animatable = true;
            if (p.type == ParamInfo::Type::Bool) {
                p.type = ParamInfo::Type::Float; // a timeline drives it as 0..1: on from the middle
                p.min = p.lo = 0;
                p.max = p.hi = 1;
            }
            out.push_back(p);
        }
        collect(child, category.isEmpty() ? child.name : category + QStringLiteral(" › ") + child.name, out);
    }
}

} // namespace

std::vector<ParamInfo> Destinations::params(quint64 id) const
{
    std::vector<ParamInfo> out;
    const Entry *e = entry(id);
    if (!e) return out;
    if (e->d.kind == Destination::Kind::OscQuery) {
        if (e->client && e->client->tree().loaded()) collect(e->client->tree().root(), QString(), out);
        return out;
    }
    for (const OscAddress &a : e->d.addresses) {
        ParamInfo p;
        p.path = a.path;
        p.label = a.path;
        p.type = a.type == QLatin1String("i") ? ParamInfo::Type::Int : ParamInfo::Type::Float;
        p.min = p.lo = a.type == QLatin1String("T") ? 0 : a.min;
        p.max = p.hi = a.type == QLatin1String("T") ? 1 : a.max;
        p.step = a.type == QLatin1String("f") ? 0.01 : 1;
        p.animatable = true;
        out.push_back(p);
    }
    return out;
}

bool Destinations::value(quint64 id, const QString &path, double *v) const
{
    const Entry *e = entry(id);
    if (!e) return false;
    if (e->client) {
        if (const RemoteNode *n = e->client->tree().find(path); n && !n->value.isEmpty()) {
            const QJsonValue x = n->value.first();
            *v = x.isBool() ? (x.toBool() ? 1 : 0) : x.toDouble();
            return true;
        }
    }
    const auto last = e->last.constFind(path);
    if (last != e->last.constEnd()) {
        *v = *last;
        return true;
    }
    if (e->d.kind == Destination::Kind::Osc)
        for (const OscAddress &a : e->d.addresses)
            if (a.path == path) {
                *v = a.type == QLatin1String("T") ? 0 : a.min;
                return true;
            }
    return false;
}

bool Destinations::send(quint64 id, const QString &path, double v)
{
    Entry *e = entry(id);
    if (!e || !std::isfinite(v)) return false;
    const auto last = e->last.constFind(path);
    if (last != e->last.constEnd() && std::abs(*last - v) < 1e-7) return true; // the same: not again
    bool done = false;
    if (e->d.kind == Destination::Kind::OscQuery) {
        if (!e->client || e->client->state() != Client::State::Ready) return false;
        const RemoteNode *n = e->client->tree().find(path);
        if (!n) return false;
        const QJsonValue arg = n->type == QLatin1String("T") || n->type == QLatin1String("F") ? QJsonValue(v >= 0.5) : QJsonValue(v);
        done = e->client->write(path, QJsonArray{arg});
    } else {
        const auto a = std::find_if(e->d.addresses.begin(), e->d.addresses.end(), [&](const OscAddress &x) { return x.path == path; });
        if (a == e->d.addresses.end() || e->address.isNull()) return false;
        osc::Message m;
        m.address = path;
        if (a->type == QLatin1String("T")) {
            m.types = v >= 0.5 ? QStringLiteral("T") : QStringLiteral("F");
        } else if (a->type == QLatin1String("i")) {
            m.types = QStringLiteral("i");
            m.args = {int(std::lround(v))};
        } else {
            m.types = QStringLiteral("f");
            m.args = {v};
        }
        done = m_udp.writeDatagram(osc::encode(m), e->address, e->d.port) > 0;
    }
    if (done) {
        e->last[path] = v;
        ++m_sent;
    }
    return done;
}

// ---- kept between launches: the ids, the OSC devices, the OSCQuery devices added by hand

void Destinations::load()
{
    QSettings settings;
    const QJsonObject o = QJsonDocument::fromJson(settings.value(QStringLiteral("destinations")).toByteArray()).object();
    m_loading = true;
    const QJsonObject kept = o.value("ids").toObject();
    for (auto it = kept.begin(); it != kept.end(); ++it) {
        const quint64 id = quint64(it.value().toDouble());
        m_ids.insert(it.key(), id);
        m_nextId = std::max(m_nextId, id + 1);
    }
    for (const QJsonValue &v : o.value("devices").toArray()) {
        const QJsonObject x = v.toObject();
        Destination d;
        d.kind = x.value("kind").toString() == QLatin1String("osc") ? Destination::Kind::Osc : Destination::Kind::OscQuery;
        d.manual = d.kind == Destination::Kind::OscQuery;
        d.name = x.value("name").toString();
        d.host = x.value("host").toString();
        d.port = quint16(x.value("port").toInt());
        d.online = d.kind == Destination::Kind::Osc;
        for (const QJsonValue &a : x.value("addresses").toArray()) {
            const QJsonObject y = a.toObject();
            d.addresses.push_back({y.value("path").toString(), y.value("type").toString("f"), y.value("min").toDouble(0), y.value("max").toDouble(1)});
        }
        if (d.host.isEmpty() || !d.port) continue;
        Entry &e = make(d);
        if (d.kind == Destination::Kind::OscQuery) open(e);
        else resolve(e);
    }
    m_loading = false;
}

void Destinations::save() const
{
    if (m_loading || !m_persistent) return;
    QJsonObject ids;
    for (auto it = m_ids.constBegin(); it != m_ids.constEnd(); ++it) ids.insert(it.key(), double(it.value()));
    QJsonArray devices;
    for (const auto &[id, e] : m_entries) {
        if (e->d.kind == Destination::Kind::OscQuery && !e->d.manual) continue; // found again on the network
        QJsonArray addresses;
        for (const OscAddress &a : e->d.addresses)
            addresses.append(QJsonObject{{"path", a.path}, {"type", a.type}, {"min", a.min}, {"max", a.max}});
        devices.append(QJsonObject{{"kind", e->d.kind == Destination::Kind::Osc ? "osc" : "oscquery"},
                                   {"name", e->d.name},
                                   {"host", e->d.host},
                                   {"port", int(e->d.port)},
                                   {"addresses", addresses}});
    }
    QSettings settings;
    settings.setValue(QStringLiteral("destinations"), QJsonDocument(QJsonObject{{"ids", ids}, {"devices", devices}}).toJson(QJsonDocument::Compact));
}
