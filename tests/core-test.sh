#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-2.1-or-later
# Test A: the encode+pack path as a pure function, no PipeWire.
#
# core-test synthesizes the six-tone sequence and writes the IEC 61937 byte
# stream; ffmpeg's spdif demuxer and AC-3 decoder are the independent oracle that
# the framing is real; core-test then checks that each segment's energy came back
# on the channel it was written to.
set -eu

here=$(cd "$(dirname "$0")" && pwd)
root=$(dirname "$here")
work=${TMPDIR:-/tmp}/pw-ac3-core-test.$$
mkdir -p "$work"
trap 'rm -rf "$work"' EXIT

make -C "$root" tests/core-test >/dev/null

echo "--- encode + pack ---"
"$here/core-test" gen "$work/out.spdif"

echo
echo "--- ffmpeg spdif demuxer ---"
ffprobe -v error -f spdif -i "$work/out.spdif" \
        -show_entries stream=codec_name,sample_rate,channels,channel_layout \
        -of default=noprint_wrappers=1
ffmpeg -v error -f spdif -i "$work/out.spdif" \
       -f f32le -ac 6 -ar 48000 -y "$work/decoded.raw"

echo
echo "--- channel routing ---"
"$here/core-test" check "$work/decoded.raw"
