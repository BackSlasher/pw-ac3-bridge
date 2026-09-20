# SPDX-License-Identifier: LGPL-2.1-or-later
# Test environment for pw-ac3-bridge: a headless PipeWire graph with no sound card.
#
# Both the current shell bridge (ffmpeg | mpv) and the native program are measured
# in containers from this one image, so the comparison is not confounded by
# different ffmpeg, mpv or PipeWire builds. There is no /dev/snd: the sinks are
# null sinks plus tests/iec958-sink, which is all the tests need.
FROM docker.io/library/archlinux:base-devel

RUN pacman -Sy --noconfirm --needed \
        pipewire pipewire-audio pipewire-pulse wireplumber libpulse \
        ffmpeg mpv \
        gcc make pkgconf dbus \
    && pacman -Scc --noconfirm

# PipeWire wants a runtime directory it owns. dbus is here because WirePlumber
# links against it; no session bus is started and none is needed.
ENV XDG_RUNTIME_DIR=/run/pw
RUN mkdir -p /run/pw && chmod 700 /run/pw

WORKDIR /src
COPY Makefile /src/Makefile
COPY src /src/src
COPY tests /src/tests
RUN make

ENTRYPOINT ["/bin/bash"]
