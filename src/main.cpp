// OSCQlock: a device that exists to be tested against. It generates a signal (a sine, a saw, a triangle, a square or noise), and says
// everything about it in every kind of node the pxs-core tree has (see Device.h for the namespace), so that a controller, an explorer or a
// mapper can be tried on all of them. The window brings together the device patch and show editors.
//
// Run it, then explore it:  curl http://127.0.0.1:5678/   (or find "OSCQlock" on the network from any OSCQuery client).
#include "Destinations.h"
#include "Device.h"
#include <pxs/DevicesPanel.h>
#include <pxs/Discovery.h>
#include <pxs/OutputsPanel.h>
#include <QJsonDocument>
#include <QTabWidget>
#include "Commands.h"
#include "TimelinesEditor.h"
#include "Show.h"

#include <pxs/OscService.h>
#include <pxs/PresetView.h>
#include <QSplitter>
#include <QTcpServer>
#include <QStackedWidget>
#include <QMenuBar>
#include <QMenu>
#include <QPointer>
#include <QFormLayout>
#include <QScrollArea>
#include <QScopedValueRollback>
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
    Device device(nullptr, false);
    const std::vector<std::string> problems = device.tree().check();
    for (const std::string &problem : problems) std::fprintf(stderr, "OSCQlock: the tree lacks something: %s\n", problem.c_str());
    pxs::OscService service(&device.tree(), QStringLiteral("OSCQlock"));
    service.setLocalOnly(true);
    QUdpSocket occupiedUdp;
    QTcpServer occupiedTcp;
    if (!occupiedUdp.bind(QHostAddress::LocalHost, 0, QUdpSocket::DontShareAddress) ||
        !occupiedTcp.listen(QHostAddress::LocalHost, 0) ||
        !service.start(occupiedUdp.localPort(), occupiedTcp.serverPort(), false) ||
        service.oscPort() == occupiedUdp.localPort() || service.httpPort() == occupiedTcp.serverPort()) return 2;
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
    bool feedback = saw("/timelines/timeline/settings/duration") && saw("/timelines/timeline/transport/playing");
    if (!feedback) std::fprintf(stderr, "OSCQlock: an edit of the timeline was not told to the tree\n");

    const char *transport = "/timelines/timeline/transport/";
    const auto applyTransport = [&device, transport](const char *action, const ValueList &args = {}) {
        return device.tree().apply(std::string(transport) + action, args) == 1;
    };
    const char *point = "/timelines/timeline/tracks/output/curve/points/point/value";
    const auto *pointNode = device.tree().find(point);
    const bool transportOk = pointNode && pointNode->parameter() && device.tree().apply(point, {Value(0.2)}) == 1
        && std::abs(device.timeline(first)->anim.tracks.front().keys.front().v - 0.2) < 1e-6
        && applyTransport("pause") && device.timeline(first)->anim.state == AnimState::Paused
        && applyTransport("seek", {Value(0.75)}) && device.timeline(first)->anim.position() == 0.75
        && applyTransport("rewind") && device.timeline(first)->anim.position() == 0
        && applyTransport("play") && device.timeline(first)->anim.state == AnimState::Playing;
    if (!transportOk) std::fprintf(stderr, "OSCQlock: a timeline point or transport action failed\n");

    // A timeline more, by OSC: its nodes are made, and it is the one shown
    const bool made = device.tree().apply("/timelines/new", {}) == 1 && device.timelines().size() == 2 && device.tree().find("/timelines/timeline-a/transport/playing") &&
                      device.tree().find("/timelines/shown")->get().front().text() == "timeline-a" && device.tree().apply("/timelines/shown", {Value("timeline")}) == 1 &&
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
    const bool sent = std::abs(received - 0.4) < 1e-4 && device.tree().find("/timelines/timeline/tracks/level/output");
    if (!sent) std::fprintf(stderr, "OSCQlock: a track did not reach its OSC device through its filter (received %g)\n", received);

    // A track that drives an OSCQuery device: OSCQlock itself, added by its address (its namespace read, then written over OSC). A number
    // and a switch of its own tree take what the tracks say: the speed of the second timeline, and its play
    const quint64 query = destinations.addOscQuery(QStringLiteral("127.0.0.1"), service.httpPort());
    {
        QEventLoop wait;
        QObject::connect(&destinations, &Destinations::changed, &wait, [&] {
            if (destinations.online(query)) wait.quit();
        });
        QTimer::singleShot(5000, &wait, &QEventLoop::quit);
        if (!destinations.online(query)) wait.exec();
    }
    Animation driving = device.timelines().front().anim;
    AnimTrack number, onOff;
    number.layer = onOff.layer = query;
    number.param = QStringLiteral("/timelines/timeline-a/settings/speed");
    number.keys = {AnimKey{0, 0.25, 0}};
    onOff.param = QStringLiteral("/timelines/timeline-a/transport/playing");
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
            if (std::abs(value("/timelines/timeline-a/settings/speed").number().value_or(-1) - 0.25) < 1e-4 && value("/timelines/timeline-a/transport/playing").truthy()) wait.quit();
        });
        poll.start(20);
        QTimer::singleShot(5000, &wait, &QEventLoop::quit);
        wait.exec();
    }
    const bool queried = destinations.online(query) && !destinations.params(query).empty() &&
                         std::abs(value("/timelines/timeline-a/settings/speed").number().value_or(-1) - 0.25) < 1e-4 && value("/timelines/timeline-a/transport/playing").truthy();
    if (!queried) std::fprintf(stderr, "OSCQlock: a track did not reach its OSCQuery device (float %g, switch %d)\n",
                               value("/timelines/timeline-a/settings/speed").number().value_or(-1), int(value("/timelines/timeline-a/transport/playing").truthy()));

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
    device.control(first, AnimAction::Play);
    device.control(first, AnimAction::Seek, 0.625);
    bool shown = show::save(file, device, destinations, &why);
    device.addTimeline(QStringLiteral("not in the show"));
    shown = shown && show::open(file, device, destinations, &why) && device.timelines().size() == timelines &&
            device.timelines().front().anim.tracks.size() == tracks && device.sequencer().sequence(0).steps.size() == 2 &&
            device.sequencer().sequence(0).steps[1].description == QLatin1String("Blackout") &&
            device.timelines().front().anim.tracks[1].layer == destinations.all().back()->id &&
            device.timeline(first)->anim.state == AnimState::Playing && device.timeline(first)->anim.clock == 0.625;
    // Opened, it is not modified; playing does not modify it; a step edited does
    shown = shown && !device.modified();
    device.control(first, AnimAction::Pause);
    shown = shown && !device.modified();
    Sequencer::Sequence shorter = device.sequencer().sequence(0);
    shorter.steps.pop_back();
    device.sequencer().setSequence(0, shorter);
    shown = shown && device.modified();
    if (!shown) std::fprintf(stderr, "OSCQlock: a show saved and opened again is not the same, or not told modified as it should (%s)\n", qPrintable(why));


    // Undo: a timeline removed comes back at its place with its tracks and its nodes; a sequence edited goes back and forth
    QUndoStack history;
    const Animation second = device.timelines()[1].anim;
    history.push(new cmd::TimelineThere(&device, device.timelines()[1], 1, false));
    bool undone = !device.timeline(second.id) && !device.tree().find("/timelines/timeline-a");
    history.undo();
    undone = undone && device.timeline(second.id) && device.timelines()[1].anim.id == second.id &&
             device.timelines()[1].anim.tracks.size() == second.tracks.size() && device.tree().find("/timelines/timeline-a/transport/playing");
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

    device.stopAll();
    const std::string address = "/timelines/timeline/settings/speed";
    const bool writable = device.tree().apply(address, {Value(0.2)}) == 1;
    auto preset = pxs::core::tree::capture(device.tree(), {address});
    preset.id = "12345";
    preset.scope = "/timelines/timeline";
    preset.fade = 1;
    device.setPresets({preset});
    Sequencer::Step recall;
    recall.preset = 12345;
    recall.presetOverrides.values[address] = "[0.75]";
    recall.presetOverrides.times[address] = 0;
    auto presetSequence = device.sequencer().sequence(0);
    presetSequence.steps = {recall};
    device.sequencer().setSequence(0, presetSequence);
    const auto encoded = Sequencer::stepToJson(recall);
    const bool timed = writable && preset.values.count(address) && encoded.contains("preset_overrides")
        && device.sequencer().goTo(0)
        && device.tree().find(address)->get().front().number() == 0.75;
    if (!timed) std::fprintf(stderr, "OSCQlock: preset capture or step override failed\n");

    pxs::devices::Device fixture;
    fixture.kind = pxs::devices::Kind::Dmx;
    fixture.name = "Output test";
    pxs::devices::Address dimmer;
    dimmer.path = "/dimmer";
    fixture.addresses.push_back(dimmer);
    const auto fixtureId = destinations.manager().add(fixture);
    const bool direct = fixtureId
        && destinations.send(fixtureId, "/dimmer", 1)
        && destinations.manager().dmxLevel(fixtureId, "/dimmer") == 1;
    Animation remapped = device.timelines().front().anim;
    AnimTrack dimmerTrack;
    dimmerTrack.layer = fixtureId;
    dimmerTrack.param = QStringLiteral("/dimmer");
    dimmerTrack.unitCurve = true;
    dimmerTrack.keys = {AnimKey{0, 1}};
    appendTrackRemap(dimmerTrack, 0, 1, 0, 255);
    remapped.tracks = {dimmerTrack};
    device.setTimeline(remapped.id, remapped);
    QEventLoop output;
    QTimer::singleShot(50, &output, &QEventLoop::quit);
    output.exec();
    const bool scaled = direct && destinations.manager().dmxLevel(fixtureId, "/dimmer") == 255;
    if (!scaled) std::fprintf(stderr, "OSCQlock: raw DMX output or explicit Remap failed\n");

    const std::string scope = "/timelines/timeline";
    device.control(remapped.id, AnimAction::Play);
    device.control(remapped.id, AnimAction::Seek, 0.375);
    auto timelinePreset = pxs::core::tree::capture(device.tree(), pxs::core::tree::presetParameters(device.tree(), scope));
    timelinePreset.id = "12346";
    timelinePreset.scope = scope;
    const std::string trackPath = scope + "/tracks/dimmer";
    timelinePreset.removeParameter(trackPath + "/curve/points/point/value");
    device.setPresets({pxs::core::tree::Preset::fromJson(timelinePreset.toJson())});
    auto editedTrack = device.timeline(remapped.id)->anim;
    editedTrack.tracks.front().name = QStringLiteral("House lights");
    editedTrack.tracks.front().keys.front().v = 0.25;
    QJsonObject filter = editedTrack.tracks.front().dataFilters.first().toObject();
    filter["d"] = 200.0;
    editedTrack.tracks.front().dataFilters[0] = filter;
    device.setTimeline(remapped.id, editedTrack);
    device.control(remapped.id, AnimAction::Stop);
    const bool restored = !device.tree().find("/devices")
        && !timelinePreset.values.count(trackPath + "/target/address")
        && device.applyPreset(0)
        && device.timeline(remapped.id)->anim.tracks.front().name == QStringLiteral("House lights")
        && device.timeline(remapped.id)->anim.tracks.front().keys.front().v == 0.25
        && device.timeline(remapped.id)->anim.tracks.front().dataFilters == dimmerTrack.dataFilters
        && device.timeline(remapped.id)->anim.state == AnimState::Playing
        && device.timeline(remapped.id)->anim.clock == 0.375;
    if (!restored) std::fprintf(stderr, "OSCQlock: scoped timeline preset recall failed\n");

    destinations.startDiscovery();
    auto *discovery = destinations.manager().findChild<pxs::oscquery::Discovery *>();
    const pxs::oscquery::Device remote{QStringLiteral("OSCQlock lifecycle check"), QStringLiteral("127.0.0.1"),
                                     QHostAddress::LocalHost, occupiedTcp.serverPort()};
    bool retained = false;
    if (discovery) {
        emit discovery->deviceFound(remote);
        quint64 id = 0;
        for (const auto *entry : destinations.all())
            if (entry->name == "OSCQlock lifecycle check") id = entry->id;
        const auto visible = [&] {
            const auto devices = destinations.all();
            return std::any_of(devices.begin(), devices.end(), [id](const auto *entry) { return entry->id == id; });
        };
        const auto previous = device.timeline(first)->anim;
        auto patched = previous;
        AnimTrack track;
        track.layer = id;
        track.param = QStringLiteral("/dimmer");
        patched.tracks.push_back(track);
        device.setTimeline(first, patched);
        emit discovery->deviceLost(remote);
        retained = id && visible() && !destinations.online(id);

        const auto savedShow = dir.filePath(QStringLiteral("offline-device.oscqlock"));
        Device reopened(nullptr, false);
        Destinations reopenedDestinations(QStringLiteral("OSCQlock"), 0, false);
        reopened.setDestinations(&reopenedDestinations);
        retained = retained && show::save(savedShow, device, destinations, &why)
            && show::open(savedShow, reopened, reopenedDestinations, &why);
        const auto reopenedDevices = reopenedDestinations.all();
        retained = retained && std::any_of(reopenedDevices.begin(), reopenedDevices.end(), [id](const auto *entry) { return entry->id == id; })
            && !reopenedDestinations.online(id);
        device.setTimeline(first, previous);
        retained = retained && !visible();
    }
    if (!retained) std::fprintf(stderr, "OSCQlock: OSCQuery device retention did not follow the timeline and preset references\n");

    device.tree().removeObserver(watching);
    feedback = feedback && transportOk && made && sent && queried && cued && shown && undone && timed && scaled && restored && retained;

    const auto *tab = device.tree().find("/ui/tab");
    const bool named = tab && device.tree().apply("/ui/tab", {Value("timelines")}) == 1
        && tab->get().front().text() == "timelines";
    const bool served = !body.isEmpty() && feedback && named;
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
    cli.setApplicationDescription("Timeline and cue playback with OSC, MIDI and DMX.");
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
    QCommandLineOption tabOption("tab", "The section to focus (timelines, sequence, presets; devices for a Device Manager screenshot).", "name");
    cli.addOptions({name, oscPort, httpPort, local, headless, check, screenshot, showOption, tabOption});
    cli.process(app);

    const bool keep = !cli.isSet(screenshot); // a picture taken changes nothing that is kept
    Device device(nullptr, keep);
    for (const std::string &problem : device.tree().check()) qWarning("OSCQlock: the tree lacks something: %s", problem.c_str()); // what every pxs tree must have
    pxs::OscService service(&device.tree(), cli.value(name));
    QObject::connect(&service, &pxs::OscService::logMessage, &service,
                     [](const QString &message) { qInfo().noquote() << message; });
    service.setLocalOnly(cli.isSet(local));
    if (!service.start(cli.value(oscPort).toUShort(), cli.value(httpPort).toUShort(), !cli.isSet(local))) {
        qCritical("OSCQlock: could not open its ports");
        return 1;
    }
    qInfo("OSCQlock: OSCQuery on http://127.0.0.1:%d/, OSC on udp %d", service.httpPort(), service.oscPort());
    Destinations destinations(cli.value(name), service.httpPort(), keep);
    device.setDestinations(&destinations);
    if (!cli.isSet(local)) destinations.startDiscovery();
    const QString showPathKey = show::settingsGroup() + QStringLiteral("/showPath");
    QString initialShowPath = keep ? QSettings().value(showPathKey).toString() : QString();
    if (cli.isSet(showOption)) {
        QString why;
        if (show::open(cli.value(showOption), device, destinations, &why)) {
            initialShowPath = QFileInfo(cli.value(showOption)).absoluteFilePath();
            if (keep) QSettings().setValue(showPathKey, initialShowPath);
        } else {
            qWarning("OSCQlock: the show could not be opened: %s", qPrintable(why));
        }
    }
    device.setSaved(); // what it starts with is what was kept: not a change

    QWidget window;
    QPlainTextEdit *log = new QPlainTextEdit;
    QWidget *devicesWindow = nullptr;
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
        brand->setStyleSheet(QStringLiteral("#brand { font-weight: 600; }"));
        header->addWidget(brand);
        const auto pill = [](const QString &text, const QString &tip) {
            auto *l = new QLabel(text);
            l->setToolTip(tip);
            l->setTextInteractionFlags(Qt::TextSelectableByMouse);
            l->setStyleSheet(QStringLiteral("QLabel { border: 1px solid #3a3d44; border-radius: 10px; padding: 2px 10px; color: #c8ccd4; }"));
            return l;
        };
        header->addSpacing(12);
        header->addWidget(pill(cli.value(name), QStringLiteral("The name it announces")));
        header->addWidget(pill(QStringLiteral("OSCQuery  http://127.0.0.1:%1/").arg(service.httpPort()), QStringLiteral("Its namespace, over HTTP and WebSocket")));
        header->addWidget(pill(QStringLiteral("OSC  udp %1").arg(service.oscPort()), QStringLiteral("What it takes over OSC")));
        header->addStretch(1);
        // The show: its file, opened and saved (what is done is kept between launches anyway)
        auto *menus = new QMenuBar(&window);
        root->setMenuBar(menus);
        auto *fileMenu = menus->addMenu(QStringLiteral("&File"));
        auto *openAction = fileMenu->addAction(QStringLiteral("&Open…"));
        auto *saveAction = fileMenu->addAction(QStringLiteral("&Save"));
        auto *saveAsAction = fileMenu->addAction(QStringLiteral("Save &As…"));
        openAction->setShortcut(QKeySequence::Open);
        saveAction->setShortcut(QKeySequence::Save);
        saveAsAction->setShortcut(QKeySequence::SaveAs);
        auto *editMenu = menus->addMenu(QStringLiteral("&Edit"));
        header->addSpacing(10);
        left->addLayout(header);

        // Undo / redo: what is edited in the window (timelines, the sequence), Cmd/Ctrl+Z and Shift+Cmd/Ctrl+Z
        auto *undo = new QUndoStack(&window);
        auto *timelinesPage = new TimelinesPage(&device, undo);
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
            const pxs::core::tree::Preset *preset(quint64 id) const override
            {
                const int index = device->indexOfPreset(id);
                return index < 0 ? nullptr : &device->presets()[index];
            }
            void stopAll(Sequencer &) override { device->stopAll(); }
        };
        auto *targets = new SequenceTargets;
        targets->device = &device;
        auto *sequence = new pxs::sequenceview::SequenceView(device.sequencer(), targets, undo);
        QObject::connect(sequence, &QObject::destroyed, [targets] { delete targets; });
        const int sequenceListener = device.addTimelinesListener([sequence] { sequence->rebuild(); }); // the timelines a step can drive
        QObject::connect(sequence, &QObject::destroyed, [&device, sequenceListener] { device.forgetTimelinesListener(sequenceListener); });

        struct PresetScopes : pxs::presetview::PresetHost {
            Device &device;
            explicit PresetScopes(Device &owner) : device(owner) {}
            std::vector<std::pair<QString, QString>> scopes() const override
            {
                std::vector<std::pair<QString, QString>> out{{QStringLiteral("/timelines"), QStringLiteral("All timelines")}};
                for (const auto &timeline : device.timelines())
                    out.emplace_back(QStringLiteral("/timelines/") + QString::fromStdString(timeline.slug), timeline.anim.name);
                return out;
            }
        };
        auto *scopes = new PresetScopes(device);
        auto *presetView = new pxs::presetview::PresetView(device.tree(), undo, scopes);
        QObject::connect(presetView, &QObject::destroyed, [scopes] { delete scopes; });
        presetView->setPresets(device.presets());
        presetView->setRecaller([&device](int index) { return device.applyPreset(index); });
        const int scopeListener = device.addTimelinesListener([presetView] { presetView->rebuildScopes(); });
        QObject::connect(presetView, &QObject::destroyed, [&device, scopeListener] {
            device.forgetTimelinesListener(scopeListener);
        });
        auto editingPreset = std::make_shared<bool>(false);
        QObject::connect(presetView, &pxs::presetview::PresetView::presetsChanged, [&device, presetView, editingPreset] {
            QScopedValueRollback guard(*editingPreset, true);
            device.setPresets(presetView->presets());
        });
        const int presetListener = device.addPresetsListener([presetView, sequence, &device, editingPreset] {
            if (!*editingPreset) presetView->setPresets(device.presets());
            sequence->rebuild();
        });
        QObject::connect(presetView, &QObject::destroyed, [&device, presetListener] {
            device.forgetPresetsListener(presetListener);
        });

        auto *patch = new QTabWidget;
        devicesWindow = patch;
        auto *patchPanel = new pxs::devices::DevicesPanel(&destinations.manager());
        patchPanel->setCompactList();
        const int devicesTab = patch->addTab(patchPanel, QString());
        const auto countDevices = [patch, devicesTab, &destinations] {
            patch->setTabText(devicesTab, QStringLiteral("Device Manager (%1)").arg(destinations.all().size()));
        };
        countDevices();
        QObject::connect(&destinations, &Destinations::changed, patch, countDevices);
        auto *outputs = new pxs::dmx::OutputsPanel;
        outputs->setDefaultSourceName(QStringLiteral("OSCQlock"));
        outputs->setMappingShown(true);
        outputs->setOutputs(destinations.outputs());
        outputs->setStateSource([&destinations](quint64 id) { return destinations.outputState(id); });
        QObject::connect(outputs, &pxs::dmx::OutputsPanel::outputsChanged, &destinations,
                         [&destinations](const auto &list, const auto &, const auto &) { destinations.setOutputs(list); });
        QObject::connect(&destinations, &Destinations::changed, outputs, [outputs, &destinations] {
            if (outputs->outputs() != destinations.outputs()) outputs->setOutputs(destinations.outputs());
        });
        auto *outputScroll = new QScrollArea;
        outputScroll->setWidgetResizable(true);
        outputScroll->setWidget(outputs);
        patch->addTab(outputScroll, QStringLiteral("DMX Outputs"));
        auto *upper = new QSplitter(Qt::Horizontal);
        upper->addWidget(devicesWindow);
        const auto titled = [](const QString &name, QWidget *content) {
            auto *pane = new QWidget;
            auto *layout = new QVBoxLayout(pane);
            layout->setContentsMargins(4, 0, 4, 0);
            layout->addWidget(new QLabel(name));
            layout->addWidget(content, 1);
            return pane;
        };
        upper->addWidget(titled(QStringLiteral("Sequence"), sequence));
        upper->addWidget(titled(QStringLiteral("Presets"), presetView));
        upper->setSizes({260, 550, 215});
        auto *workspace = new QSplitter(Qt::Vertical);
        workspace->addWidget(upper);
        workspace->addWidget(titled(QStringLiteral("Timelines"), timelinesPage));
        workspace->setSizes({440, 340});
        auto *inspector = new QStackedWidget;
        inspector->addWidget(presetView->inspector());
        inspector->addWidget(patchPanel->inspector());
        auto stepDetails = std::make_shared<QPointer<QWidget>>();
        const auto showDetails = [inspector, stepDetails](QWidget *details) {
            if (*stepDetails) {
                inspector->removeWidget(*stepDetails);
                (*stepDetails)->deleteLater();
            }
            *stepDetails = details;
            inspector->addWidget(details);
            inspector->setCurrentWidget(details);
        };
        sequence->setInspectorSink([&device, showDetails](QWidget *details) {
            device.tree().apply("/ui/tab", {Value("sequence")});
            showDetails(details);
        });
        QObject::connect(patchPanel, &pxs::devices::DevicesPanel::deviceSelected, inspector,
                         [inspector, patchPanel](quint64) { inspector->setCurrentWidget(patchPanel->inspector()); });
        timelinesPage->setInspectorSink([&device, showDetails](quint64 id) {
            device.tree().apply("/ui/tab", {Value("timelines")});
            const auto *timeline = device.timeline(id);
            if (!timeline) return;
            auto *details = new QWidget;
            auto *form = new QFormLayout(details);
            form->addRow(QStringLiteral("Timeline"), new QLabel(timeline->anim.name));
            form->addRow(QStringLiteral("Duration"), new QLabel(QStringLiteral("%1 s").arg(timeline->anim.duration)));
            form->addRow(QStringLiteral("Tracks"), new QLabel(QString::number(timeline->anim.tracks.size())));
            showDetails(details);
        });
        QObject::connect(presetView, &pxs::presetview::PresetView::presetSelected, inspector,
                         [inspector, presetView, &device] {
                             device.tree().apply("/ui/tab", {Value("presets")});
                             inspector->setCurrentWidget(presetView->inspector());
                         });
        auto *editor = new QSplitter(Qt::Horizontal);
        editor->addWidget(workspace);
        auto *inspection = new QWidget;
        auto *inspectionLayout = new QVBoxLayout(inspection);
        inspectionLayout->addWidget(new QLabel(QStringLiteral("Inspector")));
        inspectionLayout->addWidget(inspector, 1);
        editor->addWidget(inspection);
        inspection->setMinimumWidth(365);
        editor->setSizes({1050, 400});
        left->addWidget(editor, 1);
        auto *logToggle = new QPushButton(QStringLiteral("OSC Log"));
        logToggle->setCheckable(true);
        auto *footer = new QHBoxLayout;
        footer->addWidget(logToggle);
        auto *recallStatus = new QLabel;
        recallStatus->setWordWrap(true);
        footer->addWidget(recallStatus, 1);
        left->addLayout(footer);
        const QPointer<QLabel> status(recallStatus);
        device.onPresetRejected([status](const QString &message) {
            if (status) status->setText(message);
        });
        log->setReadOnly(true);
        log->setMaximumBlockCount(400);
        log->setFixedHeight(84);
        left->addWidget(log);
        log->hide();
        QObject::connect(logToggle, &QPushButton::toggled, log, &QWidget::setVisible);
        root->addLayout(left, 1);

        // The file of the show: its name in the header and in the title, • while it is not saved
        auto showPath = std::make_shared<QString>(initialShowPath);
        const auto title = [&window, showPath, &device] {
            const QString n = showPath->isEmpty() ? QStringLiteral("Untitled show") : QFileInfo(*showPath).completeBaseName();
            const QString mark = device.modified() ? QStringLiteral("  •") : QString();
            window.setWindowTitle(QStringLiteral("OSCQlock — %1%2").arg(n, mark));
        };
        const auto saveTo = [&window, &device, &destinations, showPath, showPathKey, title](QString path) {
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
            QSettings().setValue(showPathKey, path);
            device.setSaved();
            title();
        };
        QObject::connect(saveAction, &QAction::triggered, &window, [saveTo, showPath] { saveTo(*showPath); });
        QObject::connect(saveAsAction, &QAction::triggered, &window, [saveTo] { saveTo(QString()); });
        QObject::connect(openAction, &QAction::triggered, &window, [&window, &device, &destinations, showPath, showPathKey, title, saveTo, undo] {
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
            QSettings().setValue(showPathKey, path);
            undo->clear(); // another show: what was done before is not undone in it
            title();
        });
        device.onTick([timelinesPage, title] {
            timelinesPage->tick();
            title();
        });
        title();
        // The undo buttons and their keys, with what they would undo or redo
        QAction *undoAction = undo->createUndoAction(&window, QStringLiteral("Undo"));
        QAction *redoAction = undo->createRedoAction(&window, QStringLiteral("Redo"));
        undoAction->setShortcut(QKeySequence::Undo);
        redoAction->setShortcut(QKeySequence::Redo);
        editMenu->addAction(undoAction);
        editMenu->addAction(redoAction);

        window.resize(1480, 860);
        const auto focusSection = [timelinesPage, sequence, presetView](const pxs::core::tree::Node &node, const auto &) {
            if (node.path() != "/ui/tab") return;
            const auto tab = node.get().front().text();
            QWidget *section = tab == "sequence" ? static_cast<QWidget *>(sequence)
                : tab == "presets" ? static_cast<QWidget *>(presetView) : static_cast<QWidget *>(timelinesPage);
            if (auto *focused = QApplication::focusWidget(); focused && (focused == section || section->isAncestorOf(focused))) return;
            section->setFocus(Qt::OtherFocusReason);
        };
        const int tabObserver = device.tree().addObserver(focusSection);
        QObject::connect(&window, &QObject::destroyed, [&device, tabObserver] { device.tree().removeObserver(tabObserver); });
        if (cli.isSet(tabOption) && cli.value(tabOption) != QLatin1String("devices"))
            device.tree().apply("/ui/tab", {Value(cli.value(tabOption).toStdString())});
        window.show();
        const bool devicesShot = cli.value(tabOption) == QLatin1String("devices"); // --tab devices: the Devices window

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
