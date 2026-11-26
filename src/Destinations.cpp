#include "Destinations.h"

#include <QJsonDocument>
#include <QHostInfo>
#include <QNetworkInterface>
#include <set>
#include <QScopedValueRollback>
#include <QSettings>

using pxs::devices::Kind;

Destinations::Destinations(const QString &name, quint16 port, bool persistent, QObject *parent)
    : QObject(parent), m_servicePort(port), m_devices(name, port)
{
    if (persistent) {
        QSettings settings;
        auto stored = QJsonDocument::fromJson(settings.value(QStringLiteral("devicePatch")).toByteArray()).array();
        if (stored.isEmpty()) {
            const auto legacy = QJsonDocument::fromJson(settings.value(QStringLiteral("destinations")).toByteArray()).object();
            const auto keys = legacy.value("ids").toObject();
            QJsonArray patched;
            for (const auto &value : legacy.value("devices").toArray()) {
                auto item = value.toObject();
                const QString key = item.value("kind").toString() == QStringLiteral("osc")
                    ? QStringLiteral("osc:") + item.value("name").toString()
                    : QStringLiteral("oscquery:%1:%2").arg(item.value("host").toString()).arg(item.value("port").toInt());
                item["id"] = keys.value(key);
                patched.append(item);
            }
            stored = {QJsonObject{{"registry", QJsonObject{{"ids", keys}, {"devices", patched}}}}};
        }
        readShow(stored);
        connect(this, &Destinations::changed, this, [this] {
            QSettings().setValue(QStringLiteral("devicePatch"), QJsonDocument(showJson()).toJson(QJsonDocument::Compact));
        });
    }
    connect(&m_devices, &pxs::devices::Devices::changed, this, [this] {
        if (m_loading) return;
        namePresetTargets();
        emit changed();
    });
    connect(&m_devices, &pxs::devices::Devices::dmxChanged, this, &Destinations::submit);
}

void Destinations::namePresetTargets()
{
    std::set<std::string> reserved;
    for (const auto &[id, name] : m_presetNames) reserved.insert(name);
    for (const auto *device : all()) {
        if (m_presetNames.count(device->id) || isLocalService(*device)) continue;
        if (device->kind == Kind::OscQuery && device->manual && !online(device->id)) continue;
        auto base = pxs::core::device::namespaceSegment(device->name);
        auto name = base;
        if (reserved.count(name)) {
            base += "-";
            base += pxs::core::device::key(device->kind);
            name = base;
        }
        for (std::string suffix = "a"; !reserved.insert(name).second; suffix += 'a') name = base + "-" + suffix;
        m_presetNames.emplace(device->id, std::move(name));
    }
}

std::string Destinations::presetPath(quint64 id) const
{
    const auto it = m_presetNames.find(id);
    return it == m_presetNames.end() ? std::string{} : "/devices/" + it->second;
}

quint64 Destinations::presetDevice(const std::string &address) const
{
    if (address.rfind("/devices/", 0) != 0) return 0;
    const auto segment = address.substr(9, address.find('/', 9) - 9);
    for (const auto &[id, name] : m_presetNames)
        if (name == segment) return id;
    return 0;
}

bool Destinations::isLocalService(const pxs::devices::Device &device) const
{
    if (device.kind != Kind::OscQuery || !m_servicePort || device.port != m_servicePort) return false;
    const auto host = QString::fromStdString(device.host);
    const QHostAddress address(host);
    return address.isLoopback() || QNetworkInterface::allAddresses().contains(address)
        || host.compare(QStringLiteral("localhost"), Qt::CaseInsensitive) == 0
        || host.compare(QHostInfo::localHostName(), Qt::CaseInsensitive) == 0;
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
    QJsonObject names;
    for (const auto &[id, name] : m_presetNames) names[QString::number(id)] = QString::fromStdString(name);
    return {QJsonObject{{"registry", m_devices.toJson()}, {"dmx_outputs", outputs}, {"preset_names", names}}};
}

std::map<quint64, quint64> Destinations::readShow(const QJsonArray &devices)
{
    QScopedValueRollback loading(m_loading, true);
    std::map<quint64, quint64> ids;
    QJsonObject registry, keys;
    QJsonArray manual, outputs;
    const auto first = devices.isEmpty() ? QJsonObject{} : devices.first().toObject();
    if (first.contains("registry")) {
        registry = first.value("registry").toObject();
        keys = registry.value("ids").toObject();
        manual = registry.value("devices").toArray();
        outputs = first.value("dmx_outputs").toArray();
    } else {
        for (const auto &value : devices) {
            auto object = value.toObject();
            const bool discovered = object.value("kind").toString() == QStringLiteral("found");
            if (discovered) object["kind"] = QStringLiteral("oscquery");
            auto device = pxs::core::device::fromJson(QJsonDocument(object).toJson(QJsonDocument::Compact).toStdString());
            if (!device) continue;
            device->manual = !discovered;
            if (device->id) keys[QString::fromStdString(device->key())] = double(device->id);
            if (!discovered) manual.append(object);
        }
        registry = QJsonObject{{"ids", keys}, {"devices", manual}};
    }
    for (const auto &id : keys) if (id.toDouble() > 0) ids.emplace(quint64(id.toDouble()), quint64(id.toDouble()));
    for (const auto &value : manual) {
        const auto id = quint64(value.toObject().value("id").toDouble());
        if (id) ids.emplace(id, id);
    }
    m_presetNames.clear();
    std::set<std::string> usedNames;
    const auto names = first.value("preset_names").toObject();
    for (auto it = names.begin(); it != names.end(); ++it) {
        const auto id = it.key().toULongLong();
        const auto value = it.value().toString().toStdString();
        if (id && !value.empty() && pxs::core::device::namespaceSegment(value) == value && usedNames.insert(value).second)
            m_presetNames.emplace(id, value);
    }
    m_outputs.clear();
    for (const auto &value : outputs) m_outputs.push_back(pxs::dmx::Output::fromJson(value.toObject()));
    m_devices.fromJson(registry);
    namePresetTargets();
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
