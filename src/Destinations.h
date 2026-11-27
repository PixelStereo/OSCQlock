#pragma once

#include <pxs/Devices.h>
#include <pxs/DmxOut.h>

#include <QJsonArray>
#include <map>

struct OscAddress {
    QString path;
    QString type = "f";
    double min = 0, max = 1;
};

// OSCQlock's show adapter for the shared PXS device registry and DMX outputs.
class Destinations : public QObject
{
    Q_OBJECT
public:
    Destinations(const QString &selfName, quint16 selfPort, bool persistent = true, QObject *parent = nullptr);
    pxs::devices::Devices &manager() { return m_devices; }
    const pxs::devices::Devices &manager() const { return m_devices; }
    void startDiscovery();
    auto all() const { return m_devices.all(); }
    auto find(quint64 id) const { return m_devices.find(id); }
    QString nameOf(quint64 id) const { return m_devices.nameOf(id); }
    bool online(quint64 id) const { return m_devices.online(id); }
    quint64 addOscQuery(const QString &host, quint16 port) { return m_devices.addOscQuery(host, port); }
    quint64 addOsc(const QString &name, const QString &host, quint16 port);
    void setAddresses(quint64 id, const std::vector<OscAddress> &addresses);
    auto params(quint64 id) const { return m_devices.params(id); }
    bool value(quint64 id, const QString &path, double *v) const { return m_devices.value(id, path, v); }
    const pxs::devices::Address *scaledAddress(quint64 id, const QString &path) const;
    bool send(quint64 id, const QString &path, double v) { return m_devices.send(id, path, v); }
    int sent() const { return m_devices.sent(); }

    QJsonArray showJson() const;
    std::map<quint64, quint64> readShow(const QJsonArray &devices);
    QList<pxs::dmx::Output> outputs() const;
    void setOutputs(const QList<pxs::dmx::Output> &outputs);
    pxs::dmx::OutputState outputState(quint64 id) const { return m_sender.state(id); }

signals:
    void changed();

private:
    void submit();
    pxs::devices::Devices m_devices;
    pxs::dmx::Sender m_sender;
    std::vector<pxs::dmx::Output> m_outputs;
    bool m_loading = false;
};
