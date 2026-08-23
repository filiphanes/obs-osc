# obs-osc

Open Sound Control (OSC 1.0) remote control plugin for OBS Studio.

Listens for OSC commands over UDP and switches scenes, controls streaming /
recording, mixes audio and drives media sources. State changes are reported
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

All commands live under `/obs/`. Numeric arguments accept int or float.

**Reading state:** any known route sent **without arguments** is a read
request in the style of the Behringer Wing OSC server: instead of acting,
OBS replies to the sending address and port with the current value(s),
using the same addresses and payload types as the feedback namespace
(e.g. `/obs/stream` answers `<0|1>`, `/obs/mute/Mic*` answers once per
matching input). Replies are always sent to the requester directly,
regardless of `feedback_enabled` or a configured fixed feedback target.

Routes that used their no-argument form as a toggle keep that behavior
through an explicit `-1` argument (int or float): `/obs/studio ,i -1`
toggles studio mode and `/obs/mute/<source> ,i -1` toggles matching
inputs. `toggle` variants also accept an explicit bool/int argument to
force a state.

| Address | Effect |
| --- | --- |
| `/obs/program/<sceneName>` | switch program scene by name |
| `/obs/program/index/<n>` | switch program scene by index (1-based) |
| `/obs/preview/<sceneName>` | set preview scene |
| `/obs/studio` [\-1\|bool] | set studio mode; `-1` toggles; no argument polls state |
| `/obs/transition` | trigger preview-to-program transition; no argument polls current transition name |
| `/obs/transition/<name>` | select current transition |
| `/obs/transition/go` | trigger transition |
| `/obs/stream[/start\|/stop\|/toggle]` [bool] | streaming control; no argument polls state |
| `/obs/record[/start\|/stop\|/toggle]` [bool] | recording control; no argument polls state |
| `/obs/record/pause` [bool] | pause/resume/toggle recording pause |
| `/obs/replay[/start\|/stop\|/toggle]` [bool] | replay buffer control; no argument polls state |
| `/obs/replay/save` | save a replay |
| `/obs/virtualcam[/start\|/stop\|/toggle]` [bool] | virtual camera control; no argument polls state |
| `/obs/screenshot` | take a screenshot |
| `/obs/mute/<source>` [\-1\|bool] | mute (`-1` toggles); `*` and `?` globs allowed; no argument polls all matching inputs |
| `/obs/volume/<source>` \[<0..1>] | set volume via perceptual fader curve; no argument polls the 0..1 position |
| `/obs/db/<source>` \[<dB>] | set volume in dBFS; no argument polls it back |
| `/obs/media/<source>/play\|pause\|toggle\|stop\|restart\|next\|prev` | media source transport |
| `/obs/media/<source>` [/state] | poll media playback state |
| `/obs/profile/<name>` | switch profile |
| `/obs/profile` | poll current profile name |
| `/obs/collection/<name>` | switch scene collection |
| `/obs/collection` | poll current scene collection name |
| `/obs/subscribe` `,s <pattern>` [`,i port`] | register this client for matching feedback topics |
| `/obs/unsubscribe` [`<pattern>`] | drop one topic filter, or all of this client's |

Example QLab-style cue: send `/obs/program/Main` to port 9000.
TouchOSC: bind buttons to e.g. `/obs/stream/toggle` and faders to
`/obs/volume/Music`; on reconnect, poll `/obs/stream`, `/obs/record`,
`/obs/studio` and `/obs/program` once each to sync button LEDs and the
current-scene label.

## Feedback namespace

| Address | Payload |
| --- | --- |
| `/obs/program` \<name\> | current program scene changed |
| `/obs/preview` \<name\> | preview scene changed |
| `/obs/studio` \<0\|1\> | studio mode state |
| `/obs/transition` \<name\> | current transition changed |
| `/obs/stream`, `/obs/record`, `/obs/replay`, `/obs/virtualcam` \<0\|1\> | output states |
| `/obs/record/paused` \<0\|1\> | recording pause state |
| `/obs/replay/saved` \<1\> | replay was saved |
| `/obs/mute/<source>` \<0\|1\> | input mute state |
| `/obs/volume/<source>` \<0..1\> | input volume (fader position) |
| `/obs/media/<source>/state` \<int\> | media state, see below |
| `/obs/profile` \<name\> | profile switched |
| `/obs/collection` \<name\> | scene collection switched |

Media states follow `enum obs_media_state`: 0 none, 1 playing, 2 opening,
3 buffering, 4 paused, 5 stopped, 6 ended, 7 error.

## Localization

The settings dialog ships in English plus 21 European languages (de, es,
fr, it, nl, pt, ca, pl, cs, sk, hu, ro, bg, el, tr, sv, da, fi, no, ru,
uk). Locale files live in `data/locale/`.

Poll replies reuse these exact addresses and payload types, so a client
can treat feedback and poll responses identically. `/obs/preview` answers
an empty string while studio mode is off; `/obs/replay/saved` is momentary
and cannot be polled.

## Topic subscriptions

Instead of receiving everything, a UDP controller can register interest
in specific topics:

```
/obs/subscribe   ,s <pattern> [,i port]   # add a topic filter for this client
/obs/unsubscribe ,s <pattern>             # remove one filter
/obs/unsubscribe                           # remove all filters of this client
```

- The client is identified by the source address and port of the datagram;
  matching feedback messages are delivered there as ordinary OSC.
- `<pattern>` is matched against full event addresses with `*` and `?`
  wildcards (e.g. `/obs/mute/*`, `/obs/stream`).
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
2. Send `{"COMMAND":"LISTEN","DATA":"/obs/stream"}` as a text frame.
3. Value changes arrive as binary frames containing raw OSC packets
   (same encoding as feedback); exact paths only, no subtree matching.
4. Send `IGNORE` to stop; closing the connection drops everything.

Every address that produces feedback can be listened to, including
`/obs/profile` and `/obs/collection`. Like all unsolicited feedback,
LISTEN delivery is gated on the `feedback_enabled` setting.

## Notes and limitations

- UDP only; TCP + SLIP framing is not implemented.
- Bundle timetags are ignored: bundle elements execute immediately.
- Scene/source names containing `/` cannot be addressed by name (use
  `/obs/program/index/<n>` for such scenes).
- OSC has no authentication. Anyone on the network who can reach the port
  can control OBS; keep the host firewalled accordingly.
- Volume feedback is rate limited to roughly one message per 33 ms per
  source.
