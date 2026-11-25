#pragma once
// Where the timelines send: the devices of the network a track can drive. Two kinds:
//   OSCQuery  found on the network (mDNS) or added by its address: its namespace is read, and its numbers can be driven (written over OSC
//             to the port its HOST_INFO says)
//   OSC       a plain OSC receiver, made by hand: a host, a UDP port, and the addresses it takes, declared with their type and range (it
//             has no namespace to read)
// Each device has an id that stays across launches (by what it is: an OSCQuery device by its name or its address, an OSC device by its
// name), so that a track still drives it the next time. Id 0 is OSCQlock itself. The OSC devices and the OSCQuery ones added by hand are
// kept in the settings.
#include <pxs/Parameter.h>

#include <QHash>
#include <QJsonArray>
#include <QHostAddress>
#include <QObject>
#include <QString>
#include <QUdpSocket>

#include <map>
#include <memory>
#include <vector>

namespace pxs::oscquery {
class Client;
class Discovery;
struct Device;
} // namespace pxs::oscquery

// An address an OSC device takes
struct OscAddress {
    QString path;          // "/mixer/fader/1"
    QString type = "f";    // "f" a number, "i" a whole number, "T" on / off
    double min = 0, max = 1;
};

struct Destination {
    enum class Kind { OscQuery, Osc };
    quint64 id = 0;
    Kind kind = Kind::OscQuery;
    QString name;
    QString host;       // a name or an address
    quint16 port = 0;   // OSCQuery: its HTTP port; OSC: its UDP port
    bool manual = false; // OSCQuery: added by hand (else found on the network)
    bool online = false; // OSCQuery: its namespace is read; OSC: always
    std::vector<OscAddress> addresses; // OSC
    QString key() const; // what it is, for its id across launches
};

class Destinations : public QObject
{
    Q_OBJECT
public:
    // `selfName`, `selfPort`: OSCQlock's own announcement, not offered as a destination
    // `persistent`: the devices made by hand are read from the settings and kept there (the window's; --check runs without)
    Destinations(const QString &selfName, quint16 selfPort, bool persistent = true, QObject *parent = nullptr);
    ~Destinations() override;

    void startDiscovery();
    std::vector<const Destination *> all() const; // OSCQuery first, then OSC, each by name
    const Destination *find(quint64 id) const;
    QString nameOf(quint64 id) const; // "OSCQlock" for 0; "" for none

    quint64 addOscQuery(const QString &host, quint16 httpPort); // by hand
    quint64 addOsc(const QString &name, const QString &host, quint16 port);
    void setOsc(quint64 id, const QString &name, const QString &host, quint16 port);
    void setAddresses(quint64 id, const std::vector<OscAddress> &addresses);
    void remove(quint64 id); // an OSCQuery device found on the network comes back when it is found again
    void refresh(quint64 id); // an OSCQuery device: its namespace read again

    // The numbers of a device a track can drive (its OSCQuery leaves of one number that can be written; an OSC device's addresses)
    std::vector<ParamInfo> params(quint64 id) const;
    bool value(quint64 id, const QString &path, double *v) const; // the last one known (read from the device, or sent)
    // Sends a value: to an OSCQuery device as its node's type says, to an OSC device as its address is declared. Sent again only when it
    // changed (a value the same as the last one is not sent). False: no such device or address, or it cannot be reached now
    bool send(quint64 id, const QString &path, double v);
    int sent() const { return m_sent; } // messages sent since the start (shown)

    // In a show file: the devices made by hand (OSC, OSCQuery by address), and those of `used` found on the network (by what they are,
    // to be found again), each with its id in this show
    QJsonArray showJson(const std::vector<quint64> &used) const;
    // Back from a show file: the devices made by hand replaced by its own, the others known by what they are. What each id of the file
    // is here (a track drives the device of the id it maps to)
    std::map<quint64, quint64> readShow(const QJsonArray &devices);

signals:
    void changed(); // a device came, went, was read or edited

private:
    struct Entry {
        Destination d;
        std::unique_ptr<pxs::oscquery::Client> client; // OSCQuery
        QHostAddress address;                          // OSC: resolved
        QHash<QString, double> last;                   // what was sent last, by path
    };
    Entry *entry(quint64 id);
    const Entry *entry(quint64 id) const;
    quint64 idFor(const QString &key);
    Entry &make(Destination d);
    void open(Entry &e);
    void resolve(Entry &e);
    void found(const pxs::oscquery::Device &device);
    void load();
    void save() const;

    QString m_selfName;
    quint16 m_selfPort;
    std::map<quint64, std::unique_ptr<Entry>> m_entries;
    QHash<QString, quint64> m_ids; // key -> id, kept
    quint64 m_nextId = 1;
    pxs::oscquery::Discovery *m_discovery = nullptr;
    QUdpSocket m_udp;
    int m_sent = 0;
    bool m_loading = false;
    bool m_persistent = true;
};
