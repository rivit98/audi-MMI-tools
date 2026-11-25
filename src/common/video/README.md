# video

A C++ library: a live H.264 stream on the screen through the unit's own
io-media and DM642 decoder. Not an app: link the Makefile's `VIDEO` group
(`src/common/video` is on every app's include path; it brings `CAMERA` and
`IOM` along). The why: [docs/native-apps.md](../../../docs/native-apps.md)
§10 and [docs/phone-mirroring.md](../../../docs/phone-mirroring.md) §7.10.

- `h264.h` / `.cpp`: Annex-B parsing, SPS / PPS / slice header fields, skip
  frames. Pure.
- `csmp4.h` / `.cpp`: the constant-sample-size MP4 header, frame padding,
  the parser. Pure.
- `live_stream.h` / `.cpp`: the stream in memory, push, gap fill. Pure.
- `stream_source.h`: what `StreamServer` serves.
- `stream_server.h` / `.cpp`: the QNX resource manager io-media reads from.
- `player.h` / `.cpp`: one io-media trackplayer graph (over `iom.c`).
- `vout.h` / `.cpp`: the video displayable and dev-videoctrl (VCAP 0 input 1).
- `hw_stream.h` / `.cpp`: `HwStream`, all of the above as one class.
- Tests: [tests/test_live.cpp](../../../tests/test_live.cpp) and
  `tests/check_live.py` (ffmpeg decodes what the pure parts produce).
