# OSCQlock

**A Pixel Stereo application**, built on [pxs-qt](https://github.com/PixelStereo/pxs-qt).

<!-- release-downloads:start -->
No release yet.
<!-- release-downloads:end -->

On a **Mac**, unzip, move OSCQlock to Applications and Control-click › Open the first time (it is signed ad hoc, not notarized). On **Windows**, run the installer. On **Linux** and the **Raspberry Pi** (4 and 5, 64-bit), make the AppImage executable and run it (or unpack the tar.gz).

**A show sequencer for OSC and OSCQuery.** Timelines (curves, waves, filters) that drive the devices of the network, played by a cue list
with a GO button (NyxMap's sequencer, shared through pxs-qt), by hand or by OSC: lights, video, sound, anything that takes OSC. OSCQuery devices are found on the network and their
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

The window can be resized. **Open…**, **Save**, **Save as…** keep a show in a file (`.oscqlock`: its timelines, its sequence, the devices it
drives); what is done is also kept between launches. `--show file` opens one at start, with or without a window (`--no-window`).

OSCQlock publishes its own controls through OSCQuery. The tab is `/ui/tab` (`timelines`, `sequence`, `presets`),
with the actions `/ui/tab/next` and `/ui/tab/previous`.

| Tab | Under | What |
|---|---|---|
| **Timelines** | `/timelines/shown` | The displayed timeline, selected by its address name. Actions `next` and `previous` cycle through the available timelines. |
| | `/timelines/new`, `new-named`, `remove` | Create a timeline, create one with a given name (`s`), or remove one by its address name (`s`). |
| | `/timelines/<name>/settings` | Duration in seconds, loop mode, pass count (0 is endless), and speed. |
| | `/timelines/<name>/transport` | Actions `play`, `pause`, `stop`, `rewind`, and `seek` (seconds in the current pass). Parameters show playback state, position, time played, and completed passes. |
| | `/timelines/<name>/tracks/<track>` | Each track has a short name and a stable address. The branch contains the destination, enable state, curve points, wave settings, and filters. `output` shows the value after filters. |
| | `/timelines/<name>/signals` | `cycle` and `done` impulses, progress in the current pass, and total seconds played. |
| **Sequence** | the cue list | NyxMap's sequencer (pxs-qt's `pxs::sequence`), shown by pxs-qt's `pxs::sequenceview`: a step has a text, a **pre-wait** after its GO, what it **does** to a timeline (play, pause, stop, rewind, seek, loop mode, speed), a **post-wait**, and **then**: **GO** (wait for GO), or **AUTO** (a double-click flips it) (the next step gets its GO once this one's action, the time its timeline plays, and its post-wait are over). **GO** (space bar) plays the step on standby (red), **GO BACK** the one before, **Stop** stops the waits, **Stop all** (Escape) the waits and every timeline; a double-click on a step's number plays it |
| | `/sequence/go`, `back`, `stop`, `stop_all` | actions (`N`), as the buttons |
| | `/sequence/step` | `i`: plays the step of that number |
| | `/sequence/position`, `next`, `steps` | the number of the step played (0: none), the text of the one GO plays, how many steps (only read) |
| | `/sequence/played` | a signal (`s`): the text of a step that was played |

## Devices: where the tracks send

The **Device Manager** tab shows the devices a track can drive. Device namespaces are kept inside OSCQlock. The **OSCQuery** devices of the network appear by themselves (mDNS):
their namespace is read, and a track can drive any number of them that can be written (sent over OSC to the port they say). When a discovered
device leaves the network, it stays visible offline only while a timeline or preset references it. One that is
not announced is added by its address (**+ OSCQuery…**, `host:port`). A **plain OSC** device is made here (**+ OSC**): a name, a host, a
UDP port, and the addresses it takes, each with its type (a number `f`, a whole number `i`, on / off `T`/`F`) and its range. In a track,
the button above its settings chooses what it drives: OSCQlock's own output, or a device and one of its numbers. A value is sent when it
changes, after the track's filters. The OSC devices, the OSCQuery ones added by hand, the timelines and the sequence are kept between launches, and in a show file.

`--screenshot file.png` saves a picture of the window and exits (`--tab sequence`: that tab; `--tab devices`: the Device Manager tab).

Every numeric parameter says its range and default (a reset is writing `DEFAULT_VALUE`); the tree is checked at start (`Tree::check()`), and `--check` fails when it lacks something.
