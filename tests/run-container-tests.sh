#!/usr/bin/env bash
# Test B, from the host: build the image once, then measure each bridge in its
# own container so neither can disturb the other's graph.
#
#   tests/run-container-tests.sh [RUNS]
set -eu

RUNS=${1:-5}
here=$(cd "$(dirname "$0")" && pwd)
root=$(dirname "$here")
IMAGE=${IMAGE:-localhost/pw-ac3-bridge-test}

echo "=== building $IMAGE ==="
podman build -t "$IMAGE" -f "$root/Containerfile" "$root"

run() {
    local name=$1; shift
    podman run --rm --replace --name "$name" "$IMAGE" /src/tests/container-test.sh "$@"
}

echo
echo "=== container 1: the current shell bridge (ffmpeg | mpv) ==="
run pw-ac3-shell shell "$RUNS"

echo
echo "=== container 2: pw-ac3-bridge --decode-to ==="
run pw-ac3-native native "$RUNS"

echo
echo "=== container 2: rebuild and idle scenarios ==="
run pw-ac3-scenarios scenarios
