#include "Destinations.h"
#include "Show.h"

#include <QJsonDocument>
#include <QScopedValueRollback>
#include <QSettings>

using pxs::devices::Kind;

Destinations::Destinations(const QString &name, quint16 port, bool persistent, QObject *parent)
    : QObject(parent), m_devices(name, port)
{
    if (persistent) {
        QSettings settings;
        settings.beginGroup(show::settingsGroup());
        readShow(QJsonDocument::fromJson(settings.value(QStringLiteral("devicePatch")).toByteArray()).array());
        connect(this, &Destinations::changed, this, [this] {
            QSettings settings;
            settings.beginGroup(show::settingsGroup());
            settings.setValue(QStringLiteral("devicePatch"), QJsonDocument(showJson()).toJson(QJsonDocument::Compact));
        });
    }
    connect(&m_devices, &pxs::devices::Devices::changed, this, [this] {
        if (m_loading) return;
        emit changed();
    });
    connect(&m_devices, &pxs::devices::Devices::dmxChanged, this, &Destinations::submit);
}

void Destinations::startDiscovery()
{
    m_devices.startDiscovery();
}

const pxs::devices::Address *Destinations::scaledAddress(quint64 id, const QString &path) const
{
    const auto *device = find(id);
    if (!device || (device->kind != Kind::Dmx && device->kind != Kind::Midi)) return nullptr;
    return device->address(path.toStdString());
}

quint64 Destinations::addOsc(const QString &name, const QString &host, quint16 port)
{
    pxs::devices::Device device;
    device.kind = Kind::Osc;
    device.name = name.toStdString();
    device.host = host.toStdString();
    device.port = port;
    return m_devices.add(device);
}

void Destinations::setAddresses(quint64 id, const std::vector<OscAddress> &addresses)
{
    const auto *stored = find(id);
    if (!stored) return;
    auto device = *stored;
    device.addresses.clear();
    for (const auto &address : addresses) {
        pxs::devices::Address a;
        a.path = address.path.toStdString();
        a.min = address.min;
        a.max = address.max;
        a.type = address.type.isEmpty() ? 'f' : address.type.front().toLatin1();
        device.addresses.push_back(a);
    }
    m_devices.set(device);
}

QJsonArray Destinations::showJson() const
{
    QJsonArray outputs;
    for (const auto &output : m_outputs) outputs.append(output.toJson());
    return {QJsonObject{{"registry", m_devices.toJson()}, {"dmx_outputs", outputs}}};
}

std::map<quint64, quint64> Destinations::readShow(const QJsonArray &devices)
{
    QScopedValueRollback loading(m_loading, true);
    std::map<quint64, quint64> ids;
    const auto first = devices.isEmpty() ? QJsonObject{} : devices.first().toObject();
    const auto registry = first.value("registry").toObject();
    const auto keys = registry.value("ids").toObject();
    const auto manual = registry.value("devices").toArray();
    const auto outputs = first.value("dmx_outputs").toArray();
    for (const auto &id : keys) if (id.toDouble() > 0) ids.emplace(quint64(id.toDouble()), quint64(id.toDouble()));
    for (const auto &value : manual) {
        const auto id = quint64(value.toObject().value("id").toDouble());
        if (id) ids.emplace(id, id);
    }
    m_outputs.clear();
    for (const auto &value : outputs) m_outputs.push_back(pxs::dmx::Output::fromJson(value.toObject()));
    m_devices.fromJson(registry);
    submit();
    emit changed();
    return ids;
}

QList<pxs::dmx::Output> Destinations::outputs() const
{
    return {m_outputs.begin(), m_outputs.end()};
}

void Destinations::setOutputs(const QList<pxs::dmx::Output> &outputs)
{
    m_outputs.assign(outputs.begin(), outputs.end());
    submit();
    emit changed();
}

void Destinations::submit()
{
    m_sender.follow(m_outputs);
    m_sender.submit(pxs::dmx::route(m_devices.dmxUniverses(), m_outputs));
}
