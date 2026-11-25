#pragma once
// The devices the timelines can drive (Destinations.h), in a window of their own: the OSCQuery devices found on the network or added by
// their address (their namespace, read), and the plain OSC devices made here, with the addresses they take (a path, a type, a range).
#include <QWidget>

class Destinations;
class QLabel;
class QLineEdit;
class QListWidget;
class QSpinBox;
class QStackedWidget;
class QTableWidget;

class DevicesWindow : public QWidget
{
public:
    explicit DevicesWindow(Destinations *destinations, QWidget *parent = nullptr);

private:
    void refresh();          // the list, from the destinations
    void showSelected();     // the selected one on the right
    void storeOsc();         // the OSC device's settings, as edited
    void storeAddresses();   // its addresses, as edited
    quint64 selected() const;

    Destinations *m_d;
    QListWidget *m_list;
    QStackedWidget *m_pages;
    // An OSCQuery device
    QLabel *m_queryTitle, *m_queryInfo;
    // An OSC device
    QLineEdit *m_oscName, *m_oscHost;
    QSpinBox *m_oscPort;
    QTableWidget *m_addresses;
    bool m_filling = false;
    quint64 m_tableFor = 0; // the OSC device the table of addresses shows
};
