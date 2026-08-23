# obs-osc

Open Sound Control (OSC 1.0) remote control plugin for OBS Studio.

Listens for OSC commands over UDP and switches scenes, controls streaming /
recording, mixes audio, drives media sources, toggles scene items and
filters, fires hotkeys and reports input tally. State changes are reported
back to the controller as feedback messages, which makes two-way layouts
(TouchOSC, QLab, Bitfocus Companion, ...) show live button and fader states.

The plugin is self-contained: it talks directly to libobs and the frontend
API. obs-websocket is not required.

## OSCQuery discovery

The plugin implements enough of the [OSCQuery proposal](
https://github.com/Vidvox/oscquery-proposal) for controllers like TouchOSC
to auto-configure: an embedded HTTP server serves `HOST_INFO` and the full
address tree as JSON (default port: OSC port + 1), and on macOS the
`_oscjson._tcp` service is advertised via Bonjour. In TouchOSC, the OBS
host then shows up automatically with every scene, source and control as
buttons/faders with live values (values refresh when the controller polls
or reconnects).

`HOST_INFO` also advertises the `LISTEN` extension: websocket clients can
subscribe to exact paths and receive value changes as raw OSC packets,
see [OSCQuery LISTEN](#oscquery-listen) below.

## Building

Registered like any other in-tree plugin (`add_obs_plugin(obs-osc)`):

```bash
cmake --preset macos   # or ubuntu / windows-x64 ...
cmake --build --preset macos --target obs-osc
```

CI builds packages for macOS (arm64/x86_64), Ubuntu x86_64 (.deb + portable
tar.gz) and Windows x64/arm64 via `.github/workflows/build-obs-osc.yaml`; tag
a release with `v*` and the artifacts are published automatically.

## Configuration

Open **Tools ▸ OSC Settings...** to change the settings at runtime. They are
stored in `plugin-config.json` next to the module config of OBS
(`obs_module_config_path()`), created with defaults on first shutdown:

```json
{
  "port": 9000,
  "feedback_enabled": true,
  "feedback_host": "",
  "feedback_port": 0,
  "query_enabled": true,
  "query_port": 0
}
```

- `port` - inbound UDP port (bound on all interfaces).
- `feedback_enabled` - send state changes back to the controller.
- `feedback_host` / `feedback_port` - fixed feedback target. When empty,
  feedback goes to the address of the most recently received packet, so no
  configuration is needed for typical controllers (this implicit fallback
  pauses while any client holds a topic subscription, see below).
- `query_enabled` / `query_port` - OSCQuery HTTP server (0 = OSC port + 1).

## Command namespace

All commands live at the OSC root (`/studio`, `/stream`, ... - no vendor
prefix). Numeric arguments accept int or float.

**Reading state:** any known route sent **without arguments** is a read
request in the style of the Behringer Wing OSC server: instead of acting,
OBS replies to the sending address and port with the current value(s),
using the same addresses and payload types as the feedback namespace
(e.g. `/stream` answers `<0|1>`, `/mute/Mic*` answers once per
matching input). Replies are always sent to the requester directly,
regardless of `feedback_enabled` or a configured fixed feedback target.

Routes that used their no-argument form as a toggle keep that behavior
through an explicit `-1` argument (int or float): `/studio ,i -1`
toggles studio mode and `/mute/<source> ,i -1` toggles matching
inputs. `toggle` variants also accept an explicit bool/int argument to
force a state.

| Address | Effect |
| --- | --- |
| `/program/<sceneName>` | switch program scene by name |
| `/program/index/<n>` | switch program scene by index (1-based) |
| `/preview/<sceneName>` | set preview scene |
| `/studio` [\-1\|bool] | set studio mode; `-1` toggles; no argument polls state |
| `/transition` | trigger preview-to-program transition; no argument polls current transition name |
| `/transition/<name>` | select current transition |
| `/transition/go` | trigger transition |
| `/stream[/start\|/stop\|/toggle]` [bool] | streaming control; no argument polls state |
| `/record[/start\|/stop\|/toggle]` [bool] | recording control; no argument polls state |
| `/record/pause` [bool] | pause/resume/toggle recording pause |
| `/replay[/start\|/stop\|/toggle]` [bool] | replay buffer control; no argument polls state |
| `/replay/save` | save a replay |
| `/virtualcam[/start\|/stop\|/toggle]` [bool] | virtual camera control; no argument polls state |
| `/screenshot` | take a screenshot |
| `/mute/<source>` [\-1\|bool] | mute (`-1` toggles); `*` and `?` globs allowed; no argument polls all matching inputs |
| `/volume/<source>` \[<0..1>] | set volume via perceptual fader curve; no argument polls the 0..1 position |
| `/db/<source>` \[<dB>] | set volume in dBFS; no argument polls it back |
| `/visible/<scene>/<item>` [\-1\|bool] | show/hide a scene item (`-1` toggles); both names accept `*`/`?` globs; no argument polls every match |
| `/visible/<scene>` | poll visibility of every item in the matching scenes |
| `/locked/<scene>/<item>` [\-1\|bool] | lock/unlock a scene item against UI edits (`-1` toggles); polls like `/visible` |
| `/order/<scene>/<item>` [<int>] | move an item within its scene (0 = bottom); no argument polls the position |
| `/filter/<source>/<filter>` [\-1\|bool] | enable/disable a filter of any source, scenes included (`-1` toggles); `/filter/<source>` polls all of its filters; a digits-only segment selects by chain position instead of name (e.g. `/filter/Cam/0`) |
| `/filter/<source>/<filter\|index>/<param>` [<val>] | read/write one scalar settings parameter of a filter by its property name (bool/int/float/text/path); `/filter/<source>/<filter>/params` polls all of them |
| `/transform/<scene>/<item>/<field>` [<float>] | move, scale, rotate or crop an item; fields: `x`, `y`, `sx`, `sy` (scale), `rot`, `crop_left`, `crop_top`, `crop_right`, `crop_bottom`; a digits-only segment selects the item by stacking position |
| `/transform/<scene>[/<item>]` | poll every transform field of each matching item |
| `/active/<input>` | read-only tally: input is rendered in the program chain; no argument polls all inputs |
| `/showing/<input>` | read-only tally: input appears anywhere (program or preview); polls like `/active` |
| `/hotkey/<name>` | trigger the OBS hotkey registered under this exact name (e.g. `/hotkey/OBSBasic.StartRecording`) |
| `/media/<source>/play\|pause\|toggle\|stop\|restart\|next\|prev` | media source transport |
| `/media/<source>` [/state] | poll media playback state |
| `/profile/<name>` | switch profile |
| `/profile` | poll current profile name |
| `/collection/<name>` | switch scene collection |
| `/collection` | poll current scene collection name |
| `/subscribe` `,s <pattern>` [`,i port`] | register this client for matching feedback topics |
| `/unsubscribe` [`<pattern>`] | drop one topic filter, or all of this client's |

Example QLab-style cue: send `/program/Main` to port 9000.
TouchOSC: bind buttons to e.g. `/stream/toggle` and faders to
`/volume/Music`; on reconnect, poll `/stream`, `/record`,
`/studio` and `/program` once each to sync button LEDs and the
current-scene label.

## Feedback namespace

| Address | Payload |
| --- | --- |
| `/program` \<name\> | current program scene changed |
| `/preview` \<name\> | preview scene changed |
| `/studio` \<0\|1\> | studio mode state |
| `/transition` \<name\> | current transition changed |
| `/stream`, `/record`, `/replay`, `/virtualcam` \<0\|1\> | output states |
| `/record/paused` \<0\|1\> | recording pause state |
| `/replay/saved` \<1\> | replay was saved |
| `/mute/<source>` \<0\|1\> | input mute state |
| `/volume/<source>` \<0..1\> | input volume (fader position) |
| `/media/<source>/state` \<int\> | media state, see below |
| `/visible/<scene>/<item>` \<0\|1\> | scene item shown or hidden |
| `/locked/<scene>/<item>` \<0\|1\> | scene item locked state |
| `/order/<scene>/<item>` \<int\> | item stacking order (0 = bottom), sent when a scene is reordered |
| `/filter/<source>/<filter>` \<0\|1\> | filter enabled state |
| `/active/<input>` \<0\|1\> | input entered/left the program chain |
| `/showing/<input>` \<0\|1\> | input appeared/disappeared on screen |
| `/transform/<scene>/<item>/<field>` \<float\> | one transform field changed (all nine are sent when an item is moved in the UI, rate limited) |
| `/profile` \<name\> | profile switched |
| `/collection` \<name\> | scene collection switched |

Media states follow `enum obs_media_state`: 0 none, 1 playing, 2 opening,
3 buffering, 4 paused, 5 stopped, 6 ended, 7 error.

## Localization

The settings dialog ships in English plus 21 European languages (de, es,
fr, it, nl, pt, ca, pl, cs, sk, hu, ro, bg, el, tr, sv, da, fi, no, ru,
uk). Locale files live in `data/locale/`.

Poll replies reuse these exact addresses and payload types, so a client
can treat feedback and poll responses identically. `/preview` answers
an empty string while studio mode is off; `/replay/saved` is momentary
and cannot be polled.

## Topic subscriptions

Instead of receiving everything, a UDP controller can register interest
in specific topics:

```
/subscribe   ,s <pattern> [,i port]   # add a topic filter for this client
/unsubscribe ,s <pattern>             # remove one filter
/unsubscribe                           # remove all filters of this client
```

- The client is identified by the source address and port of the datagram;
  matching feedback messages are delivered there as ordinary OSC.
- `<pattern>` is matched against full event addresses with `*` and `?`
  wildcards (e.g. `/mute/*`, `/stream`).
- The optional port argument redirects delivery to another local port,
  keeping the sender's IP - useful when the OSC listener differs from the
  sending socket.
- Delivery happens only when `feedback_enabled` is on. A configured fixed
  feedback host still receives everything; the implicit reply-to-last-
  controller broadcast stays only while no client has subscribed.
- Subscriptions persist until unsubscribed or OBS exits (no keepalive
  needed). State is capped at 32 clients x 64 patterns.

## OSCQuery LISTEN

The OSCQuery server advertises the `LISTEN` extension (`HOST_INFO`), so
OSCQuery-aware clients can stream values over websockets as defined by
the proposal:

1. Open a websocket to the OSCQuery HTTP port.
2. Send `{"COMMAND":"LISTEN","DATA":"/stream"}` as a text frame.
3. Value changes arrive as binary frames containing raw OSC packets
   (same encoding as feedback); exact paths only, no subtree matching.
4. Send `IGNORE` to stop; closing the connection drops everything.

Every address that produces feedback can be listened to, including
`/profile` and `/collection`. Like all unsolicited feedback,
LISTEN delivery is gated on the `feedback_enabled` setting.

## Notes and limitations

- UDP only; TCP + SLIP framing is not implemented.
- Bundle timetags are ignored: bundle elements execute immediately.
- Scene/source names containing `/` cannot be addressed by name (use
  `/program/index/<n>` for such scenes).
- On routes that accept a filter or scene item selector, a digits-only
  segment always means an array index (chain order for filters,
  stacking order for items, both 0-based), never a name. These indexed
  and parameter-level routes are intentionally not listed in the
  OSCQuery tree: indexes shift whenever arrays change.
- Filter settings parameters only cover scalar property types (bool,
  int, float, text, path); lists, colors and buttons cannot be driven
  over OSC.
- OSC has no authentication. Anyone on the network who can reach the port
  can control OBS; keep the host firewalled accordingly.
- Volume feedback is rate limited to roughly one message per 33 ms per
  source.
