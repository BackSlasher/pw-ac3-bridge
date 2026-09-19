# callPackage-style derivation for the target NixOS host.
#
#   ac3-bridge = pkgs.callPackage ./pw-ac3-bridge/package.nix { };
#
# ffmpeg is a build input for its headers and a runtime input for libavcodec and
# libavutil; only the AC-3 encoder and decoder are used, so ffmpeg-headless is
# enough and is what the default argument picks.
{ lib
, stdenv
, pkg-config
, pipewire
, ffmpeg-headless
}:

stdenv.mkDerivation (finalAttrs: {
  pname = "pw-ac3-bridge";
  version = "0.1.0";

  # .jj is not in cleanSourceFilter's list, and leaving it in the source would
  # make the store path change on every jj operation.
  src = lib.cleanSourceWith {
    src = ./.;
    filter = path: type:
      lib.cleanSourceFilter path type && baseNameOf path != ".jj";
  };

  nativeBuildInputs = [ pkg-config ];
  buildInputs = [ pipewire ffmpeg-headless ];

  makeFlags = [ "PREFIX=$(out)" ];

  # The Makefile's default target also builds the two test binaries, which only
  # the test scripts want.
  buildFlags = [ "pw-ac3-bridge" ];

  enableParallelBuilding = true;

  meta = {
    description = "Encode a 5.1 PipeWire sink to an AC-3 IEC 61937 passthrough stream";
    longDescription = ''
      Captures the monitor of a 6-channel null sink, encodes each 1536-sample
      block to AC-3 with libavcodec, packs it into an IEC 61937 burst and plays
      the bursts into a sink that advertises the ac3-iec61937 format. A receiver
      that decodes AC-3 but not multichannel LPCM gets 5.1 that way, at one
      AC-3 frame of buffering rather than the several hundred milliseconds an
      ffmpeg-into-mpv pipeline costs.

      Both endpoints are addressed by node.name, so the target may move between
      ALSA PCM devices — as an HDMI output does when the display renegotiates —
      without the bridge losing it or being moved onto a sink that cannot carry
      a bitstream.
    '';
    # No license is declared in the tree yet; set this when one is.
    mainProgram = "pw-ac3-bridge";
    platforms = lib.platforms.linux;
  };
})
