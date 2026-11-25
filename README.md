# OSCQlock

**A Pixel Stereo application**, built on [pxs-qt](https://github.com/PixelStereo/pxs-qt).

<!-- release-downloads:start -->
No release yet.
<!-- release-downloads:end -->

On a **Mac**, unzip, move OSCQlock to Applications and Control-click › Open the first time (it is signed ad hoc, not notarized). On **Windows**, run the installer. On **Linux** and the **Raspberry Pi** (4 and 5, 64-bit), make the AppImage executable and run it (or unpack the tar.gz).

A device made to be tested against: a generator that serves an OSCQuery tree with **every kind of parameter, action and signal**, so that a controller, an
explorer or a mapper (NyxMap, OSCQontrol, Kadran, MpOsc…) can be tried on all of them.

```sh
git clone --recurse-submodules https://github.com/PixelStereo/OSCQlock.git
cd OSCQlock
./build.sh                                  # build into build/ and open it (Qt 6.4 or later; --no-run, --debug, --clean)
# a window, OSCQuery on :5678, OSC on udp 9000, announced as "OSCQlock"
./build.sh -- --no-window --local            # only the device, reachable from this computer (what follows -- goes to OSCQlock)
curl http://127.0.0.1:5678/
```

The executable carries its version in its name (`build/OSCQlock 0.1.0`, an application bundle on macOS). `--check` starts the device,
asks its own OSCQuery server for the tree and exits: what CI runs on every package.

It is built on [pxs-qt](https://github.com/PixelStereo/pxs-qt) (a submodule, which carries pxs-core): the OSC and OSCQuery server, the
parameter tree and the dark look of the Pixel Stereo applications.

## The namespace, tab by tab

The window is made from the tree, and can be resized: **a tab for each top-level node**, a section for each node that holds others, and **one widget for each leaf**,
the one of its kind (the fields and the colour picker of pxs-qt). **The signals are not in the tabs: they are in a drawer** on the right of the window (the handle ◂ opens it),
with their meters, lamps and readouts, grouped by the node that holds them. A widget writes the way an OSC message does, and follows the tree
when anything else writes: from OSC, from another widget, from the device. Hover a row for its address, kind and tags.

The tab and the drawer are parameters like the others, so a controller can move the window: `/ui/tab` (one of `generator values types actions access structure ui`),
the actions `/ui/tab/next` and `/ui/tab/previous`, and `/ui/signals` (`T`: the drawer open or closed).

| Tab | Under | The case it is for |
|---|---|---|
| **Generator** | the timelines | **as many timelines as one wants**, a sub-tab each (**+** makes one, double-click renames, **×** removes; the last one stays), NyxMap's (pxs-core's logic, pxs-qt's editor). Each has **tracks**: keys or a wave, and **filters** (pxs-core's: smoothing, shape, timing, triggers), each track driving OSCQlock's own output or **a number of a device of the network** (the **Devices** window, below). The editor and the nodes below edit the same timeline: each change is told to the tree (OSC feedback) |
| | `/generator/shown` | the sub-tab shown (a choice of the slugs, `timeline1`, `timeline2`…), that holds the actions `next` and `previous` |
| | `/generator/new`, `new_named`, `remove` | actions: a timeline more (`N`, or named: `s`), shown; a timeline removed by its slug (`s`). `last_action` (`s`): a signal, the last action received |
| | `/generator/<slug>` | one timeline: `name` (text); `transport/play` (on: plays, off: pauses), `stop`, `rewind`, `seek` (`f`, in the pass); `timeline/duration` (s, of a pass), `loop` (`once loop pingpong`), `passes` (0: endless), `speed`, `passes_done` (only read); `remove` |
| | `/generator/<slug>/tracks`, `signals` | signals: `tracks/1`, `2`… what each track says now, after its filters (`f`); `signals/cycle` (a pass starts) and `done` (`N`), `progress` (`f`, in the pass), `seconds` (`d`, played) |
| **Values** | `/values/position` | a value (`ff`) that holds its components `x` and `y`: writing one says it to the other |
| | `/values/color` | a colour (`fff`) that holds `r` `g` `b`, a switch `enable` and an action `set`; its widget is the colour picker |
| | `/values/rgba`, `origin` | a colour with alpha (`ffff`) and three plain numbers (`fff`), neither with components |
| **Types** | `/types/…` | one parameter of each kind: `switch` (`T`), `int`, `int_among`, `choice`, `float`, `wide` (a wide range), `text`, `double` (`d`), `int64` (`h`), `blob` (`b`) |
| **Actions** | `/actions/…` | nodes that are only actions: with no value (`none`), with one of each type, with several (`point`, `color`, `several`, `mixed`); `counter` is a parameter that holds the actions `increment`, `decrement`, `reset` |
| *(drawer)* | `/signals/…` | the kinds the generator does not say: `xy` (`ff`), `rgb` (`fff`), `text` (`s`) that holds `length` (`i`), `stamp` (`h`), `digest` (`b`), following the first track of the first timeline; with `/structure/mixed/pulse` |
| **Access** | `/access/…` | `read_only` (a write is refused), `write_only` (no value to read), `locked` (the device refuses every write), `transient` (never saved), `critical`, `tagged` (tags and an attribute of its own), `trigger` (a parameter that is an action) |
| **Structure** | `/structure/…` | nodes that are only nodes: `empty`, four levels deep (`a/b/c/d`), side by side (`b/sibling`), and `mixed`: a parameter, an action and a signal in one node |
| **Interface** | `/ui/tab`, `/ui/signals` | the window's own tab (with the actions `next` and `previous` that it holds), and the drawer of the signals |

## Devices: where the tracks send

The **Devices** button (top right) opens the devices a track can drive. The **OSCQuery** devices of the network appear by themselves (mDNS):
their namespace is read, and a track can drive any number of them that can be written (sent over OSC to the port they say). One that is
not announced is added by its address (**+ OSCQuery…**, `host:port`). A **plain OSC** device is made here (**+ OSC**): a name, a host, a
UDP port, and the addresses it takes, each with its type (a number `f`, a whole number `i`, on / off `T`/`F`) and its range. In a track,
the button above its settings chooses what it drives: OSCQlock's own output, or a device and one of its numbers. A value is sent when it
changes, after the track's filters. The OSC devices, the OSCQuery ones added by hand and the timelines are kept between launches.

`--screenshot file.png` saves a picture of the window and exits (`--tab devices`: the Devices window).

Every numeric parameter says its range and default (a reset is writing `DEFAULT_VALUE`); the tree is checked at start (`Tree::check()`), and `--check` fails when it lacks something.
