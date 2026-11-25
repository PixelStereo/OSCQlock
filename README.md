# OSCQlock

**A Pixel Stereo application**, built on [pxs-qt](https://github.com/PixelStereo/pxs-qt).

<!-- release-downloads:start -->
No release yet.
<!-- release-downloads:end -->

On a **Mac**, unzip, move OSCQlock to Applications and Control-click › Open the first time (it is signed ad hoc, not notarized). On **Windows**, run the installer. On **Linux** and the **Raspberry Pi** (4 and 5, 64-bit), make the AppImage executable and run it (or unpack the tar.gz).

**A show sequencer for OSC and OSCQuery.** Timelines (curves, waves, filters) that drive the devices of the network, played by a cue list
with a GO button, by hand or by OSC: lights, video, sound, anything that takes OSC. OSCQuery devices are found on the network and their
parameters listed, nothing to type; plain OSC devices are declared once. OSCQlock is an OSCQuery device itself, so a controller (Companion,
QLab, OSCQontrol on a phone…) drives it like anything else.

The device made to test controllers against, with every kind of parameter, action and signal, is now
[OSCQbench](https://github.com/PixelStereo/OSCQbench).

```sh
git clone --recurse-submodules https://github.com/PixelStereo/OSCQlock.git
cd OSCQlock
./build.sh                                  # build into build/ and open it (Qt 6.4 or later; --no-run, --debug, --clean)
# a window, OSCQuery on :5678, OSC on udp 9000, announced as "OSCQlock"
./build.sh -- --no-window --show my.oscqlock  # a show box: no window, the show opened at start (what follows -- goes to OSCQlock)
curl http://127.0.0.1:5678/
```

The executable carries its version in its name (`build/OSCQlock 0.1.0`, an application bundle on macOS). `--check` starts the device,
asks its own OSCQuery server for the tree and exits: what CI runs on every package.

It is built on [pxs-qt](https://github.com/PixelStereo/pxs-qt) (a submodule, which carries pxs-core): the OSC and OSCQuery server, the
parameter tree and the dark look of the Pixel Stereo applications.

## The window and the namespace

The window can be resized. **Open…**, **Save**, **Save as…** keep a show in a file (`.oscqlock`: its timelines, its cues, the devices it
drives); what is done is also kept between launches. `--show file` opens one at start, with or without a window (`--no-window`).

Everything is a node of OSCQlock's own tree, so a controller drives it as it drives anything else. The tab is `/ui/tab` (`generator`,
`cues`), with the actions `/ui/tab/next` and `/ui/tab/previous`.

| Tab | Under | What |
|---|---|---|
| **Generator** | the timelines | **as many timelines as one wants**, a sub-tab each (**+** makes one, double-click renames, **×** removes; the last one stays), NyxMap's (pxs-core's logic, pxs-qt's editor). Each has **tracks**: keys or a wave, and **filters** (pxs-core's: smoothing, shape, timing, triggers), each track driving OSCQlock's own output or **a number of a device of the network** (the **Devices** window, below). The editor and the nodes below edit the same timeline: each change is told to the tree (OSC feedback) |
| | `/generator/shown` | the sub-tab shown (a choice of the slugs, `timeline1`, `timeline2`…), that holds the actions `next` and `previous` |
| | `/generator/new`, `new_named`, `remove` | actions: a timeline more (`N`, or named: `s`), shown; a timeline removed by its slug (`s`). `last_action` (`s`): a signal, the last action received |
| | `/generator/<slug>` | one timeline: `name` (text); `transport/play` (on: plays, off: pauses), `stop`, `rewind`, `seek` (`f`, in the pass); `timeline/duration` (s, of a pass), `loop` (`once loop pingpong`), `passes` (0: endless), `speed`, `passes_done` (only read); `remove` |
| | `/generator/<slug>/tracks`, `signals` | signals: `tracks/1`, `2`… what each track says now, after its filters (`f`); `signals/cycle` (a pass starts) and `done` (`N`), `progress` (`f`, in the pass), `seconds` (`d`, played) |
| **Cues** | the cue list | a cue: a name, a **wait** after GO, what it **does** (start a timeline from its beginning, play, pause, stop it, or stop all) and whether GO **goes on to the next** one by itself (cues fired together, each after its own wait). **GO** (space bar) fires the cue on standby and moves the standby on; **Back**; **Stop all** (Escape); a double-click on a cue's number puts it on standby |
| | `/cues/go`, `back`, `stop_all` | actions (`N`), as the buttons |
| | `/cues/fire` | `i`: fires the cue of that number now, the standby does not move |
| | `/cues/standby`, `next`, `count` | the number of the cue on standby (`i`, written: another one), its name and how many cues (only read) |
| | `/cues/fired` | a signal (`s`): the name of a cue that acts, after its wait |

## Devices: where the tracks send

The **Devices** button (top right) opens the devices a track can drive. The **OSCQuery** devices of the network appear by themselves (mDNS):
their namespace is read, and a track can drive any number of them that can be written (sent over OSC to the port they say). One that is
not announced is added by its address (**+ OSCQuery…**, `host:port`). A **plain OSC** device is made here (**+ OSC**): a name, a host, a
UDP port, and the addresses it takes, each with its type (a number `f`, a whole number `i`, on / off `T`/`F`) and its range. In a track,
the button above its settings chooses what it drives: OSCQlock's own output, or a device and one of its numbers. A value is sent when it
changes, after the track's filters. The OSC devices, the OSCQuery ones added by hand, the timelines and the cues are kept between launches, and in a show file.

`--screenshot file.png` saves a picture of the window and exits (`--tab cues`: that tab; `--tab devices`: the Devices window).

Every numeric parameter says its range and default (a reset is writing `DEFAULT_VALUE`); the tree is checked at start (`Tree::check()`), and `--check` fails when it lacks something.
