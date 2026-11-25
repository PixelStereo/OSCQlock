// OSCQlock: a device that exists to be tested against. It generates a signal (a sine, a saw, a triangle, a square or noise), and says
// everything about it in every kind of node the pxs-core tree has (see Device.h for the namespace), so that a controller, an explorer or a
// mapper can be tried on all of them. The window shows the same tree, a tab for each top-level node (Panel.h).
//
// Run it, then explore it:  curl http://127.0.0.1:5678/   (or find "OSCQlock" on the network from any OSCQuery client).
#include "Destinations.h"
#include "Device.h"
#include "DevicesWindow.h"
#include "Commands.h"
#include "GeneratorEditor.h"
#include "Panel.h"
#include "Show.h"

#include <pxs/OscService.h>
#include <pxs/SequenceView.h>
#include <pxs/Theme.h>
#include <Fields.h>

#include <QApplication>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QEventLoop>
#include <QHBoxLayout>
#include <QLabel>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QFileDialog>
#include <QFileInfo>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QSettings>
#include <QTemporaryDir>
#include <QUndoStack>
#include <QPushButton>
#include <QUdpSocket>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <cstdio>
#include <algorithm>
#include <cmath>
#include <cstring>

using namespace pxs::core;
using namespace pxs::core::tree;

namespace {

// --check: the device starts, its tree is as every pxs tree must be, and its OSCQuery server answers with every top-level node. For the package
// to be tried before it is published
int selfTest()
{
    Device device;
    const std::vector<std::string> problems = device.tree().check();
    for (const std::string &problem : problems) std::fprintf(stderr, "OSCQlock: the tree lacks something: %s\n", problem.c_str());
    pxs::OscService service(&device.tree(), QStringLiteral("OSCQlock"));
    service.setLocalOnly(true);
    if (!service.start(0, 0, false)) return 2;
    QNetworkAccessManager net;
    QEventLoop loop;
    QByteArray body;
    QNetworkReply *reply = net.get(QNetworkRequest(QUrl(QStringLiteral("http://127.0.0.1:%1/").arg(service.httpPort()))));
    QObject::connect(reply, &QNetworkReply::finished, [&] {
        if (reply->error() == QNetworkReply::NoError) body = reply->readAll();
        loop.quit();
    });
    QTimer::singleShot(10000, &loop, &QEventLoop::quit);
    loop.exec();
    // What the editor writes (a timeline, its transport) is told to the tree: the OSC feedback
    std::vector<std::string> told;
    const int watching = device.tree().addObserver([&told](const pxs::core::tree::Node &node, const ValueList &) { told.push_back(node.path()); });
    const quint64 first = device.timelines().front().anim.id;
    Animation edited = device.timelines().front().anim;
    edited.tracks.front().wave = AnimWave::Saw;
    edited.duration = 2;
    device.setTimeline(first, edited);
    device.control(first, AnimAction::Play);
    const auto saw = [&told](const char *path) { return std::find(told.begin(), told.end(), path) != told.end(); };
    bool feedback = saw("/generator/timeline1/timeline/duration") && saw("/generator/timeline1/transport/play");
    if (!feedback) std::fprintf(stderr, "OSCQlock: an edit of the timeline was not told to the tree\n");

    // A timeline more, by OSC: its nodes are made, and it is the one shown
    const bool made = device.tree().apply("/generator/new", {}) == 1 && device.timelines().size() == 2 && device.tree().find("/generator/timeline2/transport/play") &&
                      device.tree().find("/generator/shown")->get().front().text() == "timeline2" && device.tree().apply("/generator/shown", {Value("timeline1")}) == 1 &&
                      device.shown() == first;
    if (!made) std::fprintf(stderr, "OSCQlock: a timeline made by OSC is not there, or not shown\n");

    // A track that drives a plain OSC device through a filter: a value of 0.8 (keys), multiplied by 0.5, arrives as 0.4
    QUdpSocket receiver;
    receiver.bind(QHostAddress::LocalHost, 0);
    Destinations destinations(QStringLiteral("OSCQlock"), service.httpPort(), false);
    device.setDestinations(&destinations);
    const quint64 osc = destinations.addOsc(QStringLiteral("Receiver"), QStringLiteral("127.0.0.1"), receiver.localPort());
    destinations.setAddresses(osc, {OscAddress{QStringLiteral("/level"), QStringLiteral("f"), 0, 1}});
    Animation sending = device.timelines().front().anim;
    AnimTrack level;
    level.layer = osc;
    level.param = QStringLiteral("/level");
    level.keys = {AnimKey{0, 0.8, 0}};
    InteractionStage half = InteractionStage::made(InteractionFilter::Math);
    half.op = MathOp::Multiply;
    half.a = 0.5;
    level.dataFilters = QJsonArray{half.toJson()};
    sending.tracks.push_back(level);
    device.setTimeline(first, sending);
    double received = -1;
    {
        QEventLoop wait; // the device steps (50 times a second) and sends
        QObject::connect(&receiver, &QUdpSocket::readyRead, &wait, [&] {
            while (receiver.hasPendingDatagrams()) {
                QByteArray packet(int(receiver.pendingDatagramSize()), 0);
                receiver.readDatagram(packet.data(), packet.size());
                std::vector<::osc::Message> messages;
                if (::osc::decode(packet, messages))
                    for (const auto &m : messages)
                        if (m.address == QLatin1String("/level") && !m.args.isEmpty()) received = m.args.front().toDouble();
            }
            if (received >= 0) wait.quit();
        });
        QTimer::singleShot(3000, &wait, &QEventLoop::quit);
        wait.exec();
    }
    const bool sent = std::abs(received - 0.4) < 1e-4 && device.tree().find("/generator/timeline1/tracks/2");
    if (!sent) std::fprintf(stderr, "OSCQlock: a track did not reach its OSC device through its filter (received %g)\n", received);

    // A track that drives an OSCQuery device: OSCQlock itself, added by its address (its namespace read, then written over OSC). A number
    // and a switch of its own tree take what the tracks say: the speed of the second timeline, and its play
    const quint64 query = destinations.addOscQuery(QStringLiteral("127.0.0.1"), service.httpPort());
    {
        QEventLoop wait;
        QObject::connect(&destinations, &Destinations::changed, &wait, [&] {
            if (const Destination *d = destinations.find(query); d && d->online) wait.quit();
        });
        QTimer::singleShot(5000, &wait, &QEventLoop::quit);
        if (!destinations.find(query)->online) wait.exec();
    }
    Animation driving = device.timelines().front().anim;
    AnimTrack number, onOff;
    number.layer = onOff.layer = query;
    number.param = QStringLiteral("/generator/timeline2/timeline/speed");
    number.keys = {AnimKey{0, 0.25, 0}};
    onOff.param = QStringLiteral("/generator/timeline2/transport/play");
    onOff.keys = {AnimKey{0, 1, 0}};
    driving.tracks.push_back(number);
    driving.tracks.push_back(onOff);
    device.setTimeline(first, driving);
    const auto value = [&device](const char *path) {
        const pxs::core::tree::Node *n = device.tree().find(path);
        return n && n->get ? n->get().front() : Value();
    };
    {
        QEventLoop wait;
        QTimer poll;
        QObject::connect(&poll, &QTimer::timeout, &wait, [&] {
            if (std::abs(value("/generator/timeline2/timeline/speed").number().value_or(-1) - 0.25) < 1e-4 && value("/generator/timeline2/transport/play").truthy()) wait.quit();
        });
        poll.start(20);
        QTimer::singleShot(5000, &wait, &QEventLoop::quit);
        wait.exec();
    }
    const bool queried = destinations.find(query)->online && !destinations.params(query).empty() &&
                         std::abs(value("/generator/timeline2/timeline/speed").number().value_or(-1) - 0.25) < 1e-4 && value("/generator/timeline2/transport/play").truthy();
    if (!queried) std::fprintf(stderr, "OSCQlock: a track did not reach its OSCQuery device (float %g, switch %d)\n",
                               value("/generator/timeline2/timeline/speed").number().value_or(-1), int(value("/generator/timeline2/transport/play").truthy()));

    // The sequence (NyxMap's), by OSC: GO plays the first step (a timeline plays), which follows: the second stops it
    Sequencer::Sequence cueList = device.sequencer().sequence(0);
    Sequencer::Step intro, out;
    intro.description = QStringLiteral("Intro");
    intro.timeline = first;
    intro.action = AnimAction::Play;
    intro.postWait = 0.1;
    intro.next = Sequencer::StepContinue::AutoFollow;
    out.description = QStringLiteral("Blackout");
    out.timeline = first;
    out.action = AnimAction::Stop;
    cueList.steps = {intro, out};
    device.control(first, AnimAction::Stop);
    device.sequencer().setSequence(0, cueList);
    bool cued = device.tree().apply("/sequence/go", {}) == 1 && device.timelines().front().anim.state == AnimState::Playing &&
                device.tree().find("/sequence/steps")->get().front().number() == 2.0;
    {
        QEventLoop wait; // the device's clock moves the follow on
        QTimer::singleShot(500, &wait, &QEventLoop::quit);
        wait.exec();
    }
    cued = cued && device.timelines().front().anim.state == AnimState::Stopped && device.sequencer().position() == 1 &&
           device.tree().find("/sequence/position")->get().front().number() == 2.0;
    if (!cued) std::fprintf(stderr, "OSCQlock: GO by OSC did not play the sequence\n");

    // A show saved and opened again: the same timelines, the same cues, the tracks driving the same devices
    QTemporaryDir dir;
    const QString file = dir.filePath(QStringLiteral("check.oscqlock"));
    QString why;
    const size_t timelines = device.timelines().size(), tracks = device.timelines().front().anim.tracks.size();
    bool shown = show::save(file, device, destinations, &why);
    device.addTimeline(QStringLiteral("not in the show"));
    shown = shown && show::open(file, device, destinations, &why) && device.timelines().size() == timelines &&
            device.timelines().front().anim.tracks.size() == tracks && device.sequencer().sequence(0).steps.size() == 2 &&
            device.sequencer().sequence(0).steps[1].description == QLatin1String("Blackout") &&
            device.timelines().front().anim.tracks[1].layer == destinations.all().back()->id;
    // Opened, it is not modified; playing does not modify it; a step edited does
    shown = shown && !device.modified();
    device.control(first, AnimAction::Play);
    shown = shown && !device.modified();
    Sequencer::Sequence shorter = device.sequencer().sequence(0);
    shorter.steps.pop_back();
    device.sequencer().setSequence(0, shorter);
    shown = shown && device.modified();
    if (!shown) std::fprintf(stderr, "OSCQlock: a show saved and opened again is not the same, or not told modified as it should (%s)\n", qPrintable(why));


    // Undo: a timeline removed comes back at its place with its tracks and its nodes; a sequence edited goes back and forth
    QUndoStack history;
    const Animation second = device.timelines()[1].anim;
    history.push(new cmd::TimelineThere(&device, second, 1, false));
    bool undone = !device.timeline(second.id) && !device.tree().find("/generator/timeline2");
    history.undo();
    undone = undone && device.timeline(second.id) && device.timelines()[1].anim.id == second.id &&
             device.timelines()[1].anim.tracks.size() == second.tracks.size() && device.tree().find("/generator/timeline2/transport/play");
    const Sequencer::Sequence was = device.sequencer().sequence(0);
    Sequencer::Sequence more = was;
    Sequencer::Step one;
    one.description = QStringLiteral("One");
    more.steps.push_back(one);
    history.push(new pxs::sequenceview::SetSequence(device.sequencer(), 0, was, more, QStringLiteral("New Step")));
    undone = undone && device.sequencer().sequence(0).steps.size() == was.steps.size() + 1;
    history.undo();
    undone = undone && device.sequencer().sequence(0).steps.size() == was.steps.size();
    history.redo();
    undone = undone && device.sequencer().sequence(0).steps.back().description == QLatin1String("One");
    if (!undone) std::fprintf(stderr, "OSCQlock: an undo did not put things back\n");

    device.tree().removeObserver(watching);
    feedback = feedback && made && sent && queried && cued && shown && undone;

    bool served = !body.isEmpty() && feedback;
    for (const std::string &tab : device.tabs()) served = served && body.contains(QByteArray("\"") + tab.c_str() + "\"");
    std::puts(served && problems.empty() ? "OSCQlock: ok" : "OSCQlock: the tree or the server is not what it must be");
    return served && problems.empty() ? 0 : 1;
}

} // namespace

int main(int argc, char **argv)
{
    for (int i = 1; i < argc; ++i)
        if (std::strcmp(argv[i], "--check") == 0) { // what CI runs on the package: no window, no display needed
            QCoreApplication core(argc, argv);
            return selfTest();
        }
    QApplication app(argc, argv);
    QApplication::setApplicationName("OSCQlock");
    QApplication::setApplicationVersion(QStringLiteral(OSCQLOCK_VERSION));
    QApplication::setOrganizationName("Pixel Stereo");
    QApplication::setOrganizationDomain("pixelstereo.fr");
    pxs::apply(app, pxs::graphite());
    ignoreWheelOnFields(&app);

    QCommandLineParser cli;
    cli.setApplicationDescription("A device to test OSC and OSCQuery controllers on: a generator with every kind of node, parameter, action and signal.");
    cli.addHelpOption();
    cli.addVersionOption();
    QCommandLineOption name({"n", "name"}, "The name it announces on the network.", "name", "OSCQlock");
    QCommandLineOption oscPort({"o", "osc-port"}, "UDP port for OSC (0: any free one).", "port", "9000");
    QCommandLineOption httpPort({"p", "http-port"}, "TCP port for OSCQuery HTTP and WebSocket (0: any free one).", "port", "5678");
    QCommandLineOption local("local", "Reachable from this computer only, and not announced.");
    QCommandLineOption headless("no-window", "No window: only the device.");
    QCommandLineOption check("check", "Start, check the tree and the OSCQuery server, and exit (no window).");
    QCommandLineOption screenshot("screenshot", "Save a picture of the window (PNG) once it is shown, and exit. With --tab, the tab it shows.", "file");
    QCommandLineOption showOption("show", "The show (.oscqlock) to open at start (also headless: a show box).", "file");
    QCommandLineOption tabOption("tab", "The tab the window opens on (generator, values, types…; devices: the Devices window too).", "name");
    cli.addOptions({name, oscPort, httpPort, local, headless, check, screenshot, showOption, tabOption});
    cli.process(app);

    const bool keep = !cli.isSet(screenshot); // a picture taken changes nothing that is kept
    Device device(nullptr, keep);
    for (const std::string &problem : device.tree().check()) qWarning("OSCQlock: the tree lacks something: %s", problem.c_str()); // what every pxs tree must have
    pxs::OscService service(&device.tree(), cli.value(name));
    service.setLocalOnly(cli.isSet(local));
    if (!service.start(cli.value(oscPort).toUShort(), cli.value(httpPort).toUShort(), !cli.isSet(local))) {
        qCritical("OSCQlock: could not open its ports");
        return 1;
    }
    qInfo("OSCQlock: OSCQuery on http://127.0.0.1:%d/, OSC on udp %d", service.httpPort(), service.oscPort());
    Destinations destinations(cli.value(name), service.httpPort(), keep);
    device.setDestinations(&destinations);
    if (!cli.isSet(local)) destinations.startDiscovery();
    if (cli.isSet(showOption)) {
        QString why;
        if (show::open(cli.value(showOption), device, destinations, &why)) {
            if (keep) QSettings().setValue(QStringLiteral("showPath"), QFileInfo(cli.value(showOption)).absoluteFilePath());
        } else {
            qWarning("OSCQlock: the show could not be opened: %s", qPrintable(why));
        }
    }
    device.setSaved(); // what it starts with is what was kept: not a change

    QWidget window;
    QPlainTextEdit *log = new QPlainTextEdit;
    DevicesWindow *devicesWindow = nullptr;
    if (!cli.isSet(headless)) {
        window.setWindowTitle("OSCQlock");
        window.setMinimumSize(760, 520);
        auto *root = new QHBoxLayout(&window);
        root->setContentsMargins(14, 10, 14, 10);
        root->setSpacing(0);

        // The controls: a header (the name, where it is reached, the devices), the tabs, what OSC brought
        auto *left = new QVBoxLayout;
        left->setSpacing(8);
        auto *header = new QHBoxLayout;
        auto *brand = new QLabel(QStringLiteral("OSC<span style='color:#ff8a3d'>Q</span>lock"));
        brand->setObjectName(QStringLiteral("brand"));
        brand->setStyleSheet(QStringLiteral("#brand { font-size: 22px; font-weight: 700; letter-spacing: 0.5px; }"));
        header->addWidget(brand);
        const auto pill = [](const QString &text, const QString &tip) {
            auto *l = new QLabel(text);
            l->setToolTip(tip);
            l->setTextInteractionFlags(Qt::TextSelectableByMouse);
            l->setStyleSheet(QStringLiteral("QLabel { border: 1px solid #3a3d44; border-radius: 10px; padding: 2px 10px; color: #c8ccd4; font-size: 11px; }"));
            return l;
        };
        header->addSpacing(12);
        header->addWidget(pill(cli.value(name), QStringLiteral("The name it announces")));
        header->addWidget(pill(QStringLiteral("OSCQuery  http://127.0.0.1:%1/").arg(service.httpPort()), QStringLiteral("Its namespace, over HTTP and WebSocket")));
        header->addWidget(pill(QStringLiteral("OSC  udp %1").arg(service.oscPort()), QStringLiteral("What it takes over OSC")));
        header->addStretch(1);
        // The show: its file, opened and saved (what is done is kept between launches anyway)
        auto *showName = new QLabel;
        showName->setStyleSheet(QStringLiteral("color: #c8ccd4; font-size: 12px;"));
        auto *openButton = new QPushButton(QStringLiteral("Open…"));
        auto *saveButton = new QPushButton(QStringLiteral("Save"));
        auto *saveAsButton = new QPushButton(QStringLiteral("Save as…"));
        openButton->setToolTip(QStringLiteral("Open a show (.oscqlock): its timelines, its sequence, its devices"));
        saveButton->setToolTip(QStringLiteral("Save the show in its file (Cmd/Ctrl+S)"));
        saveButton->setShortcut(QKeySequence::Save);
        openButton->setShortcut(QKeySequence::Open);
        auto *undoButton = new QPushButton(QStringLiteral("Undo"));
        auto *redoButton = new QPushButton(QStringLiteral("Redo"));
        for (QPushButton *b : {undoButton, redoButton}) b->setEnabled(false);
        header->addWidget(undoButton);
        header->addWidget(redoButton);
        header->addSpacing(8);
        header->addWidget(showName);
        header->addWidget(openButton);
        header->addWidget(saveButton);
        header->addWidget(saveAsButton);
        header->addSpacing(10);
        auto *devicesButton = new QPushButton;
        devicesButton->setObjectName(QStringLiteral("accent"));
        devicesButton->setToolTip(QStringLiteral("The devices the tracks can drive: OSCQuery found on the network, plain OSC made here"));
        const auto countDevices = [devicesButton, &destinations] {
            devicesButton->setText(QStringLiteral("Devices  ·  %1").arg(destinations.all().size()));
        };
        countDevices();
        QObject::connect(&destinations, &Destinations::changed, devicesButton, countDevices);
        header->addWidget(devicesButton);
        header->addSpacing(14);
        left->addLayout(header);

        // Undo / redo: what is edited in the window (timelines, the sequence), Cmd/Ctrl+Z and Shift+Cmd/Ctrl+Z
        auto *undo = new QUndoStack(&window);
        auto *generator = new GeneratorPage(&device, undo);
        // The cue list: pxs's view of NyxMap's sequencer, its steps driving these timelines
        struct SequenceTargets : pxs::sequenceview::SequenceViewHost {
            Device *device;
            std::vector<std::pair<quint64, QString>> timelines() const override
            {
                std::vector<std::pair<quint64, QString>> out;
                for (const Device::Timeline &t : device->timelines()) out.emplace_back(t.anim.id, t.anim.name);
                return out;
            }
            quint64 currentTimeline() const override { return device->shown(); }
            void stopAll(Sequencer &) override { device->stopAll(); }
        };
        auto *targets = new SequenceTargets;
        targets->device = &device;
        auto *sequence = new pxs::sequenceview::SequenceView(device.sequencer(), targets, undo);
        QObject::connect(sequence, &QObject::destroyed, [targets] { delete targets; });
        const int sequenceListener = device.addTimelinesListener([sequence] { sequence->rebuild(); }); // the timelines a step can drive
        QObject::connect(sequence, &QObject::destroyed, [&device, sequenceListener] { device.forgetTimelinesListener(sequenceListener); });
        auto *controls = new Panel(device.tree(), device.tabs(), device.declared(), Panel::Mode::Controls, {{"generator", generator}, {"sequence", sequence}});
        controls->setMinimumSize(640, 380);
        controls->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        left->addWidget(controls, 1);
        auto *received = new QLabel("OSC received");
        received->setObjectName("caption");
        left->addWidget(received);
        log->setReadOnly(true);
        log->setMaximumBlockCount(400);
        log->setFixedHeight(84);
        left->addWidget(log);
        root->addLayout(left, 1);

        // The file of the show: its name in the header and in the title, • while it is not saved
        auto showPath = std::make_shared<QString>(cli.isSet(showOption) ? QFileInfo(cli.value(showOption)).absoluteFilePath()
                                                                        : QSettings().value(QStringLiteral("showPath")).toString());
        const auto title = [&window, showName, showPath, &device] {
            const QString n = showPath->isEmpty() ? QStringLiteral("Untitled show") : QFileInfo(*showPath).completeBaseName();
            const QString mark = device.modified() ? QStringLiteral("  •") : QString();
            showName->setText(n + mark);
            window.setWindowTitle(QStringLiteral("OSCQlock — %1%2").arg(n, mark));
        };
        const auto saveTo = [&window, &device, &destinations, showPath, title](QString path) {
            if (path.isEmpty())
                path = QFileDialog::getSaveFileName(&window, QStringLiteral("Save the show"), *showPath,
                                                    QStringLiteral("OSCQlock show (*.%1)").arg(show::kSuffix));
            if (path.isEmpty()) return;
            if (QFileInfo(path).suffix().isEmpty()) path += QStringLiteral(".") + show::kSuffix;
            QString why;
            if (!show::save(path, device, destinations, &why)) {
                QMessageBox::warning(&window, QStringLiteral("Save"), QStringLiteral("The show could not be saved: %1").arg(why));
                return;
            }
            *showPath = path;
            QSettings().setValue(QStringLiteral("showPath"), path);
            device.setSaved();
            title();
        };
        QObject::connect(saveButton, &QPushButton::clicked, &window, [saveTo, showPath] { saveTo(*showPath); });
        QObject::connect(saveAsButton, &QPushButton::clicked, &window, [saveTo] { saveTo(QString()); });
        QObject::connect(openButton, &QPushButton::clicked, &window, [&window, &device, &destinations, showPath, title, saveTo, undo] {
            // What would be replaced and is not in a file: saved first, or let go, or nothing opened
            if (device.modified()) {
                const auto answer = QMessageBox::question(&window, QStringLiteral("Open a show"),
                                                          QStringLiteral("The show has changes that are not saved. Save them first?"),
                                                          QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Save);
                if (answer == QMessageBox::Cancel) return;
                if (answer == QMessageBox::Save) {
                    saveTo(*showPath);
                    if (device.modified()) return; // not saved (cancelled, or it failed)
                }
            }
            const QString path = QFileDialog::getOpenFileName(&window, QStringLiteral("Open a show"), *showPath,
                                                              QStringLiteral("OSCQlock show (*.%1)").arg(show::kSuffix));
            if (path.isEmpty()) return;
            QString why;
            if (!show::open(path, device, destinations, &why)) {
                QMessageBox::warning(&window, QStringLiteral("Open"), QStringLiteral("The show could not be opened: %1").arg(why));
                return;
            }
            *showPath = path;
            QSettings().setValue(QStringLiteral("showPath"), path);
            undo->clear(); // another show: what was done before is not undone in it
            title();
        });
        device.onTick([generator, title] {
            generator->tick();
            title();
        });
        title();
        // The undo buttons and their keys, with what they would undo or redo
        QAction *undoAction = undo->createUndoAction(&window, QStringLiteral("Undo"));
        QAction *redoAction = undo->createRedoAction(&window, QStringLiteral("Redo"));
        undoAction->setShortcut(QKeySequence::Undo);
        redoAction->setShortcut(QKeySequence::Redo);
        window.addAction(undoAction);
        window.addAction(redoAction);
        QObject::connect(undoButton, &QPushButton::clicked, undoAction, &QAction::trigger);
        QObject::connect(redoButton, &QPushButton::clicked, redoAction, &QAction::trigger);
        QObject::connect(undoAction, &QAction::changed, undoButton, [undoButton, undoAction] {
            undoButton->setEnabled(undoAction->isEnabled());
            undoButton->setToolTip(undoAction->text() + QStringLiteral("  (") + QKeySequence(QKeySequence::Undo).toString(QKeySequence::NativeText) + ')');
        });
        QObject::connect(redoAction, &QAction::changed, redoButton, [redoButton, redoAction] {
            redoButton->setEnabled(redoAction->isEnabled());
            redoButton->setToolTip(redoAction->text() + QStringLiteral("  (") + QKeySequence(QKeySequence::Redo).toString(QKeySequence::NativeText) + ')');
        });

        devicesWindow = new DevicesWindow(&destinations, &window);
        QObject::connect(devicesButton, &QPushButton::clicked, devicesWindow, [devicesWindow] {
            devicesWindow->show();
            devicesWindow->raise();
            devicesWindow->activateWindow();
        });
        window.resize(1180, 780);
        if (cli.isSet(tabOption)) device.tree().apply("/ui/tab", {Value(cli.value(tabOption).toStdString())});
        window.show();
        const bool devicesShot = cli.value(tabOption) == QLatin1String("devices"); // --tab devices: the Devices window
        if (devicesShot) devicesWindow->show();
        if (cli.isSet(screenshot))
            QTimer::singleShot(1500, &window, [&window, devicesWindow, devicesShot, file = cli.value(screenshot)] {
                (devicesShot ? static_cast<QWidget *>(devicesWindow) : &window)->grab().save(file);
                QApplication::quit();
            });
    }
    QObject::connect(&service, &pxs::OscService::messageReceived, log, [log](const ::osc::Message &m, int accepted) {
        QStringList args;
        for (const QVariant &v : m.args) args << v.toString();
        log->appendPlainText(QString("%1 %2 %3%4").arg(m.address, m.types, args.join(' '), accepted ? "" : "  (refused)"));
    });

    const int result = app.exec();
    return result;
}
