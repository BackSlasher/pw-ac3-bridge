#!/usr/bin/env bash
# Test B, inside the container: measure one bridge implementation black-box.
#
#   container-test.sh shell  [RUNS]   the ffmpeg | mpv chain, flags copied from
#                                     athena's ac3-bridge.sh
#   container-test.sh native [RUNS]   pw-ac3-bridge --decode-to
#   container-test.sh scenarios       sink vanish and idle teardown (native only)
#
# Three private null sinks: the probe signal goes into measure_in, the bridge
# reads its monitor and puts decoded 5.1 into measure_out, and both monitors are
# summed into the two channels of measure_mix — input on the left, bridge output
# on the right. The gap between a tone's onset on the two sides is the bridge's
# software latency, measured without trusting either implementation's own
# reporting.
#
# One capture of the one mix sink is what puts both sides on one clock. Two
# separate pulse captures in a single ffmpeg do NOT share a timeline: the second
# device opens tens of milliseconds after the first while its timestamps still
# start at zero, a bias large enough to swamp a latency this small. The
# monitor-to-mix hop is identical on both sides and cancels.
#
# mpv DECODES the AC-3 here rather than bitstreaming it: a null sink cannot
# accept iec958, and the decode keeps the same buffers in the path as the real
# passthrough leg. The native bridge's --decode-to does the same for the same
# reason. Neither number includes the receiver.
set -u

MODE=${1:-native}
RUNS=${2:-5}
BIN=${BIN:-/src/pw-ac3-bridge}
CORE=${CORE:-/src/tests/core-test}
W=/tmp/probe
mkdir -p "$W"

# ---------------------------------------------------------------- session

start_session() {
    pipewire >"$W/pipewire.log" 2>&1 &
    pipewire-pulse >"$W/pulse.log" 2>&1 &
    wireplumber >"$W/wireplumber.log" 2>&1 &
    for _ in $(seq 1 50); do
        pactl info >/dev/null 2>&1 && return 0
        sleep 0.2
    done
    echo "PipeWire did not come up" >&2
    cat "$W/pipewire.log" "$W/wireplumber.log" >&2
    exit 1
}

SURROUND_MAP=front-left,front-right,front-center,lfe,rear-left,rear-right

load_sinks() {
    pactl load-module module-null-sink sink_name=measure_in channels=6 \
        channel_map=$SURROUND_MAP rate=48000 >"$W/mod_in"
    pactl load-module module-null-sink sink_name=measure_out channels=6 \
        channel_map=$SURROUND_MAP rate=48000 >"$W/mod_out"
    pactl load-module module-null-sink sink_name=measure_mix channels=2 \
        rate=48000 >"$W/mod_mix"
    wire_mix
}

# Sum all six channels of each side into one channel of the mix sink. Ports show
# up a moment after the module loads, hence the retries.
link_retry() {
    local n
    for n in $(seq 1 25); do
        pw-link "$1" "$2" 2>/dev/null && return 0
        sleep 0.2
    done
    echo "could not link $1 -> $2" >&2
    return 1
}

wire_mix() {
    local c
    for c in FL FR FC LFE RL RR; do
        link_retry "measure_in:monitor_$c"  measure_mix:playback_FL
        link_retry "measure_out:monitor_$c" measure_mix:playback_FR
    done
}

make_signal() {
    "$CORE" probe "$W/probe.s16le" >/dev/null
    ffmpeg -v error -f s16le -ar 48000 -ac 6 -channel_layout 5.1 \
           -i "$W/probe.s16le" -c:a pcm_s16le -y "$W/probe.wav"
}

# A silent client keeps a link on measure_in, so the bridge is warmed up and
# streaming before the probe tones start — the state the real bridge is in when
# a film is already playing. Both implementations get it.
start_keepalive() {
    ffmpeg -v error -f lavfi -i anullsrc=r=48000:cl=5.1 \
           -f pulse -device measure_in keepalive >"$W/keepalive.log" 2>&1 &
    echo $! >"$W/keepalive.pid"
}
stop_keepalive() {
    [ -f "$W/keepalive.pid" ] && kill "$(cat "$W/keepalive.pid")" 2>/dev/null
    rm -f "$W/keepalive.pid"
}

# ----------------------------------------------------------------- bridges

# Flags copied from athena's ac3-bridge.sh, minus --audio-spdif=ac3 (see above)
# and plus --audio-channels=5.1 so the decode reaches measure_out as 6 channels.
start_shell_bridge() {
    (
        ffmpeg -v error -fflags nobuffer \
               -f pulse -fragment_size 4608 -channels 6 -sample_rate 48000 \
               -i measure_in.monitor \
               -c:a ac3 -b:a 640k -flush_packets 1 -f ac3 - \
        | mpv --no-video --ao=pipewire --audio-device=pipewire/measure_out \
              --audio-channels=5.1 \
              --demuxer-lavf-format=ac3 --demuxer-lavf-probesize=32 \
              --demuxer-lavf-analyzeduration=0 --demuxer-readahead-secs=0 \
              --cache=no --audio-buffer=0.02 --pipewire-buffer=10 \
              --msg-level=all=error -
    ) >"$W/bridge.log" 2>&1 &
    echo $! >"$W/bridge.pid"
}
stop_shell_bridge() {
    pkill -f "measure_in.monitor" 2>/dev/null
    pkill -f "demuxer-lavf-format=ac3" 2>/dev/null
    [ -f "$W/bridge.pid" ] && wait "$(cat "$W/bridge.pid")" 2>/dev/null
    rm -f "$W/bridge.pid"
}

start_native_bridge() {
    "$BIN" --source measure_in --decode-to measure_out --idle 3600 --verbose \
        >"$W/bridge.log" 2>&1 &
    echo $! >"$W/bridge.pid"
}
stop_native_bridge() {
    [ -f "$W/bridge.pid" ] && kill "$(cat "$W/bridge.pid")" 2>/dev/null
    [ -f "$W/bridge.pid" ] && wait "$(cat "$W/bridge.pid")" 2>/dev/null
    rm -f "$W/bridge.pid"
}

start_bridge() { [ "$MODE" = shell ] && start_shell_bridge || start_native_bridge; }
stop_bridge()  { [ "$MODE" = shell ] && stop_shell_bridge  || stop_native_bridge; }

# ------------------------------------------------------------------- probe

# Every tone onset in each stream. Pair each source onset with the NEAREST LATER
# output onset (within 600 ms) and take the median, so one spurious event cannot
# shift the pairing of all the others.
onsets() {
    ffmpeg -i "$W/cap.wav" \
           -af "pan=mono|c0=c$1,silencedetect=n=-30dB:d=0.05" -f null - 2>&1 \
    | grep -oE "silence_end: [0-9.]+" | awk '{print $2}' | tr '\n' ' '
}

measure_underruns() {
    if [ "$MODE" = shell ]; then
        grep -ciE 'underrun|xrun|drop' "$W/bridge.log"
    else
        grep -oE 'underruns [0-9]+' "$W/bridge.log" | tail -1 | awk '{print $2+0}'
    fi
}

one_run() {
    local n=$1
    start_bridge
    sleep 4

    ffmpeg -v error -y -f pulse -channels 2 -i measure_mix.monitor \
                       -t 18 -c:a pcm_s16le "$W/cap.wav" &
    local cap=$!
    sleep 1.5
    mpv --no-video --ao=pipewire --audio-device=pipewire/measure_in \
        --audio-channels=5.1 --msg-level=all=error "$W/probe.wav" \
        >/dev/null 2>&1
    wait "$cap"

    local A B U
    A=$(onsets 0); B=$(onsets 1)
    U=$(measure_underruns); U=${U:-0}
    stop_bridge

    awk -v A="$A" -v B="$B" -v u="$U" -v run="$n" 'BEGIN{
      na=split(A,a,/[[:space:]]+/); nb=split(B,b,/[[:space:]]+/); n=0
      for(i=1;i<=na;i++){ if(a[i]=="")continue; best=-1
        for(j=1;j<=nb;j++){ if(b[j]=="")continue; d=b[j]-a[i]
          if(d>=0 && d<0.6 && (best<0||d<best)) best=d }
        if(best>=0){ n++; v[n]=best*1000 } }
      if(n==0){ printf "run %d: no pairs (in:%d out:%d onsets)\n", run, na, nb; exit }
      for(i=1;i<=n;i++)for(j=i+1;j<=n;j++)if(v[j]<v[i]){t=v[i];v[i]=v[j];v[j]=t}
      med=(n%2)?v[(n+1)/2]:(v[n/2]+v[n/2+1])/2
      printf "run %d: median %.0f ms over %d pairs (min %.0f, max %.0f)  underruns %s\n",
             run, med, n, v[1], v[n], u
      print med >> "/tmp/probe/medians" }'
}

# --------------------------------------------------------------- scenarios

scenario_sink_vanish() {
    echo "--- sink vanish: unload and reload the target sink ---"
    start_keepalive
    start_native_bridge
    sleep 3
    grep -q "streams up" "$W/bridge.log" \
        && echo "  bridge came up" || { echo "  FAIL: bridge never came up"; return 1; }

    pactl unload-module "$(cat "$W/mod_out")"
    sleep 2
    grep -qE "target sink .*removed|stream lost" "$W/bridge.log" \
        && echo "  noticed the sink disappear" \
        || { echo "  FAIL: sink removal not noticed"; return 1; }

    local before
    before=$(grep -c "streams up" "$W/bridge.log")
    pactl load-module module-null-sink sink_name=measure_out channels=6 \
        channel_map=$SURROUND_MAP rate=48000 >"$W/mod_out"
    sleep 3
    local after
    after=$(grep -c "streams up" "$W/bridge.log")
    [ "$after" -gt "$before" ] \
        && echo "  rebuilt its streams on the new node" \
        || { echo "  FAIL: no rebuild after the sink came back"; return 1; }

    # And audio actually flows again.
    ffmpeg -v error -y -f pulse -channels 6 -i measure_out.monitor -t 8 \
           -c:a pcm_s16le "$W/after.wav" &
    local cap=$!
    sleep 0.5
    mpv --no-video --ao=pipewire --audio-device=pipewire/measure_in \
        --audio-channels=5.1 --msg-level=all=error "$W/probe.wav" \
        >/dev/null 2>&1 &
    wait "$cap"
    pkill -f "pipewire/measure_in" 2>/dev/null
    local n
    n=$(ffmpeg -i "$W/after.wav" -af "silencedetect=n=-30dB:d=0.05" -f null - 2>&1 \
        | grep -c "silence_end")
    [ "$n" -gt 0 ] \
        && echo "  audio resumed ($n tone onsets in the rebuilt output)" \
        || { echo "  FAIL: no audio after the rebuild"; return 1; }
    stop_keepalive
    stop_native_bridge
    return 0
}

scenario_idle() {
    echo "--- idle: teardown after the last client, rebuild on the next ---"
    "$BIN" --source measure_in --decode-to measure_out --idle 5 --verbose \
        >"$W/bridge.log" 2>&1 &
    echo $! >"$W/bridge.pid"

    start_keepalive
    sleep 4
    grep -q "streams up" "$W/bridge.log" \
        && echo "  bridge up while a client is linked" \
        || { echo "  FAIL: bridge never came up"; return 1; }

    stop_keepalive
    # Still streaming right after the client leaves: the receiver keeps its lock.
    sleep 2
    local mid
    mid=$(grep -c "streams down" "$W/bridge.log")
    [ "$mid" -eq 0 ] \
        && echo "  still streaming 2 s after the client left" \
        || { echo "  FAIL: tore down before the idle window expired"; return 1; }

    sleep 6
    grep -q "idle for 5s" "$W/bridge.log" && grep -q "streams down" "$W/bridge.log" \
        && echo "  tore down after the 5 s idle window" \
        || { echo "  FAIL: no teardown after the idle window"; return 1; }

    local before
    before=$(grep -c "streams up" "$W/bridge.log")
    start_keepalive
    sleep 3
    local after
    after=$(grep -c "streams up" "$W/bridge.log")
    stop_keepalive
    [ "$after" -gt "$before" ] \
        && echo "  came back up for the next client" \
        || { echo "  FAIL: did not restart for the next client"; return 1; }
    stop_native_bridge
    return 0
}

# -------------------------------------------------------------------- main

start_session
load_sinks
make_signal

if [ "$MODE" = scenarios ]; then
    rc=0
    scenario_sink_vanish || rc=1
    echo
    scenario_idle || rc=1
    echo
    [ "$rc" -eq 0 ] && echo "SCENARIOS PASS" || echo "SCENARIOS FAIL"
    exit "$rc"
fi

echo "=== $MODE bridge, $RUNS runs ==="
rm -f "$W/medians"
start_keepalive
for n in $(seq 1 "$RUNS"); do
    one_run "$n"
done
stop_keepalive

awk -v mode="$MODE" '{v[NR]=$1}
END{
  n=NR; if(n==0){ print "no runs produced a median"; exit 1 }
  for(i=1;i<=n;i++)for(j=i+1;j<=n;j++)if(v[j]<v[i]){t=v[i];v[i]=v[j];v[j]=t}
  med=(n%2)?v[(n+1)/2]:(v[n/2]+v[n/2+1])/2
  printf "%s: median of run medians %.0f ms over %d runs (min %.0f, max %.0f)\n",
         mode, med, n, v[1], v[n] }' "$W/medians"
