# OSCQlock

**A Pixel Stereo application**, built on [pxs-qt](https://github.com/PixelStereo/pxs-qt).

<!-- release-downloads:start -->
No release yet.
<!-- release-downloads:end -->

On a **Mac**, unzip, move OSCQlock to Applications and Control-click › Open the first time (it is signed ad hoc, not notarized). On **Windows**, run the installer. On **Linux** and the **Raspberry Pi** (4 and 5, 64-bit), make the AppImage executable and run it (or unpack the tar.gz).

A device made to be tested against: a generator that serves an OSCQuery tree with **every kind of parameter, action and signal**, so that a controller, an
explorer or a mapper (NyxMap, OSCKadran, Kadran, MpOsc…) can be tried on all of them.

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

The window is made from the tree: **a tab for each top-level node**, a section for each node that holds others, and **one widget for each leaf**,
the one of its kind (the fields, the colour picker and the meters of pxs-qt). A widget writes the way an OSC message does, and follows the tree
when anything else writes: from OSC, from another widget, from the device. Hover a row for its address, kind and tags.

The tab is a parameter like the others, `/ui/tab` (one of `generator values types actions signals access structure ui`), so a controller
can move the window: `/ui/tab values`, or the actions `/ui/tab/next` and `/ui/tab/previous`.

| Tab | Under | The case it is for |
|---|---|---|
| **Generator** | `/generator/transport` | a parameter (`play`) and actions side by side: `start`, `stop`, `rewind`, `randomize`, `seek` (`f`), `jump` (`i`), `go_to` (`if`), `flash` (`T`) |
| | `/generator/wave` | parameters only: `type` (a choice), `frequency` (Hz, a step), `amplitude`, `offset`, `shift`, `hue` (degrees, a ramp that takes the short way round), `label` (text) |
| | `/generator/loops` | whole numbers: `loops` (any within limits), `steps` (one among `1 2 4 8 16`), `loop_count` (only read) |
| **Values** | `/values/position` | a value (`ff`) that holds its components `x` and `y`: writing one says it to the other |
| | `/values/color` | a colour (`fff`) that holds `r` `g` `b`, a switch `enable` and an action `set`; its widget is the colour picker |
| | `/values/rgba`, `origin` | a colour with alpha (`ffff`) and three plain numbers (`fff`), neither with components |
| **Types** | `/types/…` | one parameter of each kind: `switch` (`T`), `int`, `int_among`, `choice`, `float`, `wide` (a wide range), `text`, `double` (`d`), `int64` (`h`), `blob` (`b`) |
| **Actions** | `/actions/…` | nodes that are only actions: with no value (`none`), with one of each type, with several (`point`, `color`, `several`, `mixed`); `counter` is a parameter that holds the actions `increment`, `decrement`, `reset` |
| **Signals** | `/signals/wave` | a signal that holds the others, one of each type: `loop` (`i`), `cycle` and `done` (impulses, `N`), `above` (`T`), `phase`, `seconds` (`d`), `xy`, `rgb`, `text`, `stamp` (`h`), `digest` (`b`); `/signals/last_action` says the last action received |
| **Access** | `/access/…` | `read_only` (a write is refused), `write_only` (no value to read), `locked` (the device refuses every write), `transient` (never saved), `critical`, `tagged` (tags and an attribute of its own), `trigger` (a parameter that is an action) |
| **Structure** | `/structure/…` | nodes that are only nodes: `empty`, four levels deep (`a/b/c/d`), side by side (`b/sibling`), and `mixed`: a parameter, an action and a signal in one node |
| **Interface** | `/ui/tab` | the window's own tab, and the actions `next` and `previous` that it holds |

Every numeric parameter says its range and default (a reset is writing `DEFAULT_VALUE`); the tree is checked at start (`Tree::check()`), and `--check` fails when it lacks something.
