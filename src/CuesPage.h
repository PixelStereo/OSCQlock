#pragma once
// The second tab: the cue list. Each cue (a row) has a name, a wait, what it does (start, play, pause or stop a timeline; stop all) and
// whether GO goes on to the next one by itself. GO fires the cue on standby (the highlighted row) and moves the standby on; Back moves it
// back; a double-click on a row's number puts that cue on standby. The same as /cues/go, /cues/back, /cues/stop_all, /cues/standby.
#include <QWidget>

class Device;
class QLabel;
class QPushButton;
class QTableWidget;

class CuesPage : public QWidget
{
public:
    explicit CuesPage(Device *device, QWidget *parent = nullptr);
    ~CuesPage() override;

private:
    void rebuild();  // the rows, from the device's cues
    void refresh();  // the standby, the GO button, what waits
    void store();    // the rows, as edited, to the device
    void move(int by); // the selected cue up or down

    Device *m_device;
    QTableWidget *m_table;
    QPushButton *m_go;
    QLabel *m_next, *m_state;
    bool m_filling = false;
    int m_listener = 0;
};
