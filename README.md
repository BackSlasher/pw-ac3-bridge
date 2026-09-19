# pw-ac3-bridge

Turns a 5.1 PCM sink into an AC-3 bitstream for an HDMI receiver, as one native
PipeWire client.

A soundbar that decodes AC-3 but will not play multichannel LPCM leaves exactly
one route to 5.1 for content with no AC-3 track of its own: encode on the fly.
The session's default sink is a 6-channel null sink; this program captures that
sink's monitor, encodes each 1536-sample block to AC-3 with libavcodec, packs it
into an IEC 61937 burst, and plays the bursts into the real sink as an encoded
passthrough stream.

It replaces a shell supervisor around `ffmpeg -f pulse … | mpv --audio-spdif=ac3`.
That pipeline works, but everything between the two processes — the capture
buffer, the pipe, mpv's demuxer and its output buffer — is latency that has to be
tuned down flag by flag and still ends up an order of magnitude above the codec's
own floor. Doing the encode inside the graph removes all of it: one AC-3 frame is
produced per capture cycle and handed straight to the output stream.

## Usage

```
pw-ac3-bridge [options]
  --source NAME      5.1 null sink whose monitor is captured (default: surround51)
  --sink NAME        sink to play the AC-3 bitstream into
                     (default: alsa_output.pci-0000_00_1f.3.hdmi-stereo)
  --decode-to NAME   test mode: decode the packed frames back to 6-channel PCM
                     and play that into NAME instead of bitstreaming
  --idle SECONDS     keep the streams up this long after the last client leaves,
                     so the receiver keeps its lock (default: 1800)
  --bitrate BPS      AC-3 bitrate (default: 640000)
  --verbose          log format negotiation and buffer counters
```

Both `NAME` arguments are PipeWire `node.name` values — what `pw-cli ls Node` or
`pactl list short sinks` prints, never an ALSA device string.

On the target host:

```
pw-ac3-bridge --source surround51 \
              --sink alsa_output.pci-0000_00_1f.3.hdmi-stereo \
              --idle 1800
```

The sink must already advertise `ac3-iec61937`. This program does not set that
(see *Things it deliberately does not do* below).

## Design

**Finding the endpoints.** Both sinks are looked up by `node.name` through the
registry, and the program reacts to them appearing and disappearing. The HDMI
output moves between ALSA PCM numbers when the display renegotiates — that is
why the old alsa-plugins `a52` approach, which binds to a hardware address, was
not an option — and only the session manager follows it across that. Both
streams carry `node.dont-reconnect`, so when the target goes away the stream
errors out instead of being quietly moved to some fallback sink; a bitstream
played into a sink that is not in passthrough mode is noise. The registry then
reports the node back under a new id and both streams are rebuilt.

**Capture, encode, play.** The capture stream attaches to the null sink's
monitor (`stream.capture.sink`) as F32, 6 channels, 48 kHz, FL FR FC LFE RL RR,
and asks for `node.latency = 1536/48000` so that one process callback yields
exactly one AC-3 frame's worth of audio. The encode and the IEC 61937 packing run
in that callback: at 640 kbps a frame is a fraction of a millisecond of work
against a 32 ms cycle, and libavcodec's encoder allocates from a pool rather than
per frame once it is running. The playback stream offers a single format,
iec958/AC3/48000. Offering only an encoded format is what makes the session
manager treat the node as encoded-only: it will link it to a sink that advertises
the same codec and put that sink into passthrough mode, or refuse to link it at
all.

**The ring.** Capture and playback are two graph nodes with their own clocks, so
a byte ring sits between them; it is the only buffering in the program. It is not
prefilled. The playback side takes whatever is there and zero-fills the rest,
which means the steady-state fill settles at the phase offset between the two
nodes — the smallest value the graph allows — after a single zero-fill at start-up.
Zeroes are silence on the decoded path, and on the bitstream path they are the
null data IEC 61937 already carries between bursts, which a receiver rides out and
resyncs from on the next preamble. The ring is eight frames deep, as
headroom against a scheduling hiccup rather than as latency. Two 48 kHz clocks on
one machine drift by a few ppm, which works out to a dropped or repeated frame on
the order of once every few minutes of continuous play; at 32 ms per frame that
is below the threshold of notice, and there is no resampler in the path to hide
it.

**Activity.** A client playing into the null sink appears as a link whose input
node is that sink. The bridge's own capture attaches to the monitor — the output
side — so it never counts itself, the same distinction the shell version made
between sink-inputs and source-outputs. While clients are linked the bridge runs.
When the last one leaves it keeps encoding, which with an idle null sink means
encoding silence, for `--idle` seconds: the wire format never changes, so the
receiver never re-locks between playbacks. After that both streams are torn down
and the sink can go back to plain PCM, and the next client brings the bridge back.

### Things it deliberately does not do

*Set the sink's codec list.* `pactl set-sink-formats <sink> 'pcm;ac3-iec61937'`
does more than write a node property: it writes through the session manager's
saved route state, which is what stops an ordinary volume change from restoring a
PCM-only list over it. Reimplementing that over the PipeWire device API means
driving `SPA_PARAM_Route` with the same save semantics, and getting it subtly
wrong is worse than not doing it — a stale saved list silently drops the box to
stereo. The existing `hdmi-passthrough-codecs` service keeps this correct and is
left in place.

*Resample or drift-correct.* Both ends are 48 kHz by construction. If the sink
ever negotiates another rate the format negotiation fails loudly rather than
silently inserting a resampler that a bitstream cannot survive.

## Tests

### A. Core, headless, no PipeWire

```
tests/core-test.sh
```

Synthesizes a six-tone sequence — 48 kHz, 6 channels, one tone per channel in
FL FR FC LFE RL RR order, two seconds each — runs it through the encode+pack
path, and feeds the bytes to ffmpeg's spdif demuxer. It asserts the stream
demuxes as AC-3 with 6 channels at 48 kHz and that each segment's energy came
back on the channel it was written to, and reports the encoder's delay.

The LFE tone is 55 Hz on purpose: AC-3 band-limits the LFE channel to 120 Hz, so
a mid-band tone there would be filtered away by the codec and look like a bug.

### B. Container integration

```
tests/run-container-tests.sh [RUNS]
```

Builds one image and runs four containers from it: the current shell bridge, the
native program with `--decode-to`, the rebuild/idle scenarios, and the real
iec958 output path. Everything runs against a headless PipeWire with no
`/dev/snd`.

The probe is black-box. Three private sinks: the signal goes into `measure_in`,
the bridge reads its monitor and puts 5.1 into `measure_out`, and all twelve
monitor channels are summed into a two-channel `measure_mix` — input on the left,
bridge output on the right. One capture of that one sink gives both sides on one
clock; two `-f pulse` inputs in a single ffmpeg do **not**, because the second
device opens tens of milliseconds after the first while its timestamps still start
at zero, and that bias is larger than the latency being measured. The monitor-to-mix
hop is identical on both sides and cancels. Tone onsets come from
`silencedetect`, each input onset is paired with the nearest later output onset,
and the run's figure is the median.

mpv decodes the AC-3 rather than bitstreaming it, and the native bridge is run
with `--decode-to` for the same reason: a null sink cannot accept iec958. The
decode keeps the same buffers in the path as the real passthrough leg. Neither
number includes the receiver.

Both implementations get a silent keepalive client on `measure_in` before the
measurement, so both are warmed up and streaming when the tones start — the state
the real bridge is in when a film is already playing.

The scenario container checks two things the shell version handled by polling
`pactl`: unloading and reloading the target sink (the bridge must notice, rebuild,
and pass audio again), and the idle window (still streaming right after the last
client leaves, torn down once the window expires, back up for the next client).

The passthrough container runs the bridge with no `--decode-to` at all, against
`tests/iec958-sink` — a sink node that advertises iec958/AC3 and nothing else and
writes whatever it is handed to a file. That covers the part of the output path a
null sink cannot: the session manager seeing an encoded-only stream and an
encoded-capable sink, agreeing on a non-raw format, and linking them. The
received bytes are then fed to ffmpeg's spdif demuxer, which must see AC-3 at
48 kHz with 6 channels and the tones still in it. The same container also points
the bitstream at a plain null sink and requires that it fail to link rather than
play noise into a sink that cannot carry it.

### C. What still cannot be tested here

Two things, both on the far side of the stand-in sink.

*The sink's adapter going into passthrough mode.* `tests/iec958-sink` is a client
node with no audioadapter in front of it, so what the passthrough container
proves is that the session manager recognises an encoded-only stream, finds a
target that shares the format, negotiates iec958/AC3/48000 and links the two, and
that the bytes that arrive are a valid IEC 61937 stream. A real ALSA sink has an
adapter, and the session manager sets
`SPA_PARAM_PORT_CONFIG_MODE_passthrough` on it so the converter is bypassed;
that step has no stand-in here.

*The ALSA device open and the receiver.* PipeWire opens an iec958/AC3 sink as
`SND_PCM_FORMAT_S16_LE`, 2 channels, 48 kHz with the AES3 rate bits set, and the
soundbar has to lock onto what comes out. Neither can be reached without hardware.

The other thing worth watching on the first real run is the codec list on the HDMI
sink: if the separate `hdmi-passthrough-codecs` service has not kept it at
`pcm;ac3-iec61937`, the sink advertises no encoded format, the link is simply not
made, and the bridge logs the error rather than playing anything — which is the
intended failure mode, but it looks like the bridge being broken.

The design was written against the target's own `pw-dump`: the HDMI
sink `alsa_output.pci-0000_00_1f.3.hdmi-stereo` lists a second `EnumFormat` entry
with `mediaSubtype: iec958` and `iec958Codec` alternatives `PCM`, `DTS`, `AC3` at
rates 32000–192000 with 48000 the default. The program offers exactly one format,
iec958/AC3/48000, which is inside that set; rate and codec are the only two fields
an iec958 format carries. The sink's `iec958.codecs` property reads
`[ PCM AC3 DTS ]`, which is what the separate `pactl set-sink-formats` service
maintains and what makes the entry appear at all. The 5.1 null sink's own
`EnumFormat` is F32P/F32LE at 48000 with position FL FR FC LFE RL RR, which is
what the capture stream asks for.

## Building

```
make                 # pw-ac3-bridge, tests/core-test, tests/iec958-sink
make install PREFIX=/usr/local
```

Needs `libpipewire-0.3`, `libavcodec` and `libavutil`.

## Deploying on NixOS

`package.nix` is a callPackage-style derivation. It is **untested**: nix is not
installed on the machine this was written on.

```nix
ac3-bridge = pkgs.callPackage ./pw-ac3-bridge/package.nix { };
```

It drops into the existing user service in place of the shell script, keeping the
same ordering — the bridge needs the sink to be advertising `ac3-iec61937` before
it can link, which `hdmi-passthrough-codecs` is what guarantees:

```nix
systemd.user.services.ac3-bridge.serviceConfig.ExecStart =
  "${ac3-bridge}/bin/pw-ac3-bridge"
  + " --source surround51"
  + " --sink alsa_output.pci-0000_00_1f.3.hdmi-stereo"
  + " --idle 1800";
```

`Restart = "always"` stays worth keeping, but it should no longer be doing much:
a sink that comes and goes is handled inside the process, and the only thing that
ends it now is PipeWire itself going away.
