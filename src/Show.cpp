#include "Show.h"

#include "Destinations.h"
#include "Device.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

#include <algorithm>

namespace show {

bool save(const QString &path, const Device &device, const Destinations &destinations, QString *error)
{
    QJsonObject o = device.show();
    o["devices"] = destinations.showJson();
    o["format"] = QStringLiteral("oscqlock-show");
    o["version"] = kVersion;
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        if (error) *error = file.errorString();
        return false;
    }
    file.write(QJsonDocument(o).toJson(QJsonDocument::Indented));
    if (!file.commit()) {
        if (error) *error = file.errorString();
        return false;
    }
    return true;
}

bool open(const QString &path, Device &device, Destinations &destinations, QString *error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error) *error = file.errorString();
        return false;
    }
    QJsonParseError parse;
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parse);
    QJsonObject o = doc.object();
    if (parse.error != QJsonParseError::NoError || o.value("format").toString() != QLatin1String("oscqlock-show")) {
        if (error) *error = QStringLiteral("not an OSCQlock show");
        return false;
    }
    if (o.value("version").toInt() != kVersion) {
        if (error) *error = QStringLiteral("Unsupported show version. Expected version %1.").arg(kVersion);
        return false;
    }
    // The devices first: what each id of the file is here, and the tracks made to drive those
    const std::map<quint64, quint64> ids = destinations.readShow(o.value("devices").toArray());
    QJsonArray timelines = o.value("timelines").toArray();
    for (int i = 0; i < timelines.size(); ++i) {
        QJsonObject t = timelines[i].toObject();
        QJsonArray tracks = t.value("tracks").toArray();
        for (int k = 0; k < tracks.size(); ++k) {
            QJsonObject track = tracks[k].toObject();
            const quint64 holder = quint64(track.value("holder").toDouble());
            if (holder) {
                const auto found = ids.find(holder);
                track["holder"] = found == ids.end() ? 0.0 : double(found->second);
                if (found == ids.end()) track["enable"] = false; // a device the file does not say: nothing to drive
            }
            tracks[k] = track;
        }
        t["tracks"] = tracks;
        timelines[i] = t;
    }
    o["timelines"] = timelines;
    return device.loadShow(o, error);
}

} // namespace show
