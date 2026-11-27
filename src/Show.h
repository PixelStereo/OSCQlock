#pragma once
// A show in a file (.oscqlock, JSON): the timelines, the cues, and the devices they drive (those made by hand in full, those found on
// the network by what they are). Opened on another computer, a track still drives the device it drove: the ids of the file are mapped
// to the ones here.
#include <QString>

class Destinations;
class Device;

namespace show {

inline constexpr int kVersion = 6;
inline QString settingsGroup() { return QStringLiteral("show-v%1").arg(kVersion); }
inline constexpr const char *kSuffix = "oscqlock";
bool save(const QString &path, const Device &device, const Destinations &destinations, QString *error);
bool open(const QString &path, Device &device, Destinations &destinations, QString *error);

} // namespace show
