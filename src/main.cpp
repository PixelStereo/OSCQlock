// OSCQlock: a device that exists to be tested against. It generates a signal (a sine, a saw, a triangle, a square or noise), and says
// everything about it in every kind of node the pxs-core tree has (see Device.h for the namespace), so that a controller, an explorer or a
// mapper can be tried on all of them. The window shows the same tree, a tab for each top-level node (Panel.h).
//
// Run it, then explore it:  curl http://127.0.0.1:5678/   (or find "OSCQlock" on the network from any OSCQuery client).
#include "Device.h"
#include "Drawer.h"
#include "GeneratorEditor.h"
#include "Panel.h"

#include <pxs/OscService.h>
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
#include <QPlainTextEdit>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <cstdio>
#include <algorithm>
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
    // What the editor writes (its timeline, its transport) is told to the tree: the OSC feedback
    std::vector<std::string> told;
    const int watching = device.tree().addObserver([&told](const pxs::core::tree::Node &node, const ValueList &) { told.push_back(node.path()); });
    Animation edited = device.timeline();
    edited.tracks.front().wave = AnimWave::Saw;
    edited.duration = 2;
    device.setTimeline(edited);
    device.control(AnimAction::Play);
    device.tree().removeObserver(watching);
    const auto saw = [&told](const char *path) { return std::find(told.begin(), told.end(), path) != told.end(); };
    const pxs::core::tree::Node *type = device.tree().find("/generator/wave/type");
    const bool feedback = saw("/generator/wave/type") && saw("/generator/timeline/duration") && saw("/generator/transport/play") && type && type->get &&
                          type->get().front().text() == "saw";
    if (!feedback) std::fprintf(stderr, "OSCQlock: an edit of the timeline was not told to the tree\n");

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
    cli.addOptions({name, oscPort, httpPort, local, headless, check});
    cli.process(app);

    Device device;
    for (const std::string &problem : device.tree().check()) qWarning("OSCQlock: the tree lacks something: %s", problem.c_str()); // what every pxs tree must have
    pxs::OscService service(&device.tree(), cli.value(name));
    service.setLocalOnly(cli.isSet(local));
    if (!service.start(cli.value(oscPort).toUShort(), cli.value(httpPort).toUShort(), !cli.isSet(local))) {
        qCritical("OSCQlock: could not open its ports");
        return 1;
    }
    qInfo("OSCQlock: OSCQuery on http://127.0.0.1:%d/, OSC on udp %d", service.httpPort(), service.oscPort());

    QWidget window;
    QPlainTextEdit *log = new QPlainTextEdit;
    Drawer *drawer = nullptr;
    int observed = 0;
    if (!cli.isSet(headless)) {
        window.setWindowTitle("OSCQlock");
        auto *root = new QHBoxLayout(&window);
        root->setSizeConstraint(QLayout::SetFixedSize); // a fixed window, as wide as the drawer is open
        root->setSpacing(0);

        // The controls: the title, the tabs, what OSC brought
        auto *left = new QVBoxLayout;
        auto *title = new QLabel(QString("<b>%1</b> — OSCQuery http://127.0.0.1:%2/ — OSC udp %3")
                                     .arg(cli.value(name)).arg(service.httpPort()).arg(service.oscPort()));
        title->setTextInteractionFlags(Qt::TextSelectableByMouse);
        left->addWidget(title);
        auto *controls = new Panel(device.tree(), device.tabs(), device.declared());
        controls->addHead("generator", new GeneratorEditor(&device));
        controls->setFixedSize(820, 640);
        left->addWidget(controls);
        auto *received = new QLabel("OSC received");
        received->setObjectName("caption");
        left->addWidget(received);
        log->setReadOnly(true);
        log->setMaximumBlockCount(400);
        log->setFixedHeight(96);
        left->addWidget(log);
        root->addLayout(left);

        // The signals, in a drawer of their own
        auto *signalsPanel = new Panel(device.tree(), device.topLevel(), device.declared(), Panel::Mode::Signals);
        drawer = new Drawer(signalsPanel, 380, "The signals the device emits");
        root->addWidget(drawer);
        // The drawer is the node "/ui/signals": the handle writes it, and a write to it (OSC) moves the drawer
        const pxs::core::tree::Node *drawerNode = device.tree().find("/ui/signals");
        drawer->onToggled([&device](bool open) { device.tree().apply("/ui/signals", {Value(open)}); });
        if (drawerNode) {
            observed = device.tree().addObserver([drawer, drawerNode](const pxs::core::tree::Node &node, const ValueList &v) {
                if (&node == drawerNode && !v.empty() && v.front().truthy() != drawer->isOpen()) drawer->setOpen(v.front().truthy());
            });
            if (drawerNode->get) drawer->setOpen(drawerNode->get().front().truthy(), false);
        }
        window.show();
    }
    QObject::connect(&service, &pxs::OscService::messageReceived, log, [log](const ::osc::Message &m, int accepted) {
        QStringList args;
        for (const QVariant &v : m.args) args << v.toString();
        log->appendPlainText(QString("%1 %2 %3%4").arg(m.address, m.types, args.join(' '), accepted ? "" : "  (refused)"));
    });

    const int result = app.exec();
    if (observed) device.tree().removeObserver(observed);
    return result;
}
