# The client's source list, shared by scripts/build-app.sh (device) and
# scripts/build-host.sh (host simulator) so the two cannot drift: a file
# added here is built for both. Sourced INSIDE the build container, with the
# repository at /src as the working directory.
#
# The DRM output is deliberately not here. The device links
# src/video/drm_output.c and the host links src/video/drm_output_sim.c; each
# build script names its own in front of XCLOUD_C_SOURCES.
CORE=src/gnx

XCLOUD_C_SOURCES="src/video/nv12.c src/ui/text.c src/ui/screen.c \
  src/input/evdev_pad.c src/media/decoder.c src/net/peer_compat.c \
  src/net/wifi_tune.c"

XCLOUD_CXX_SOURCES="src/app/main.cpp src/app/present.cpp src/app/stream.cpp \
  src/app/options.cpp \
  src/app/replay.cpp src/app/library.cpp src/app/library_data.cpp \
  src/app/thumbs.cpp src/app/ui.cpp src/net/engine.cpp \
  src/media/video_jitter.cpp src/media/audio_jitter.cpp \
  src/media/audio_player.cpp src/media/au_recorder.cpp \
  src/media/video_pipeline.cpp \
  $CORE/auth.cpp $CORE/catalog.cpp $CORE/http.cpp \
  $CORE/session.cpp $CORE/xcloud_protocol.cpp"
