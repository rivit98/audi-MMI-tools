# hello12

Live SBC audio through io-media's Bluetooth reader on the DM642, the path
the MMI uses for phone music ([docs/bt-audio-path.md](../../docs/bt-audio-path.md)).
io-media turns the input `sbc:<path>` into `BT@44100.16.2.SBC:<path>` and
decodes it on the DSP. The path can be hello12's own resource manager, so
an app can play a live stream without the Bluetooth stack. Output:
`mixer:DSPOUT_TDM1_20_SAD?delay=130`, so the radio goes quiet while a
sound plays.

- **Run:** `hello12 <sd_path> [steps]` (default `wfsS`). Needs
  `clips/a1_sbc_ping.sbc` and `clips/a2_sbc_sweep.sbc` (`make sd`; made by
  `media/mkclips.sh` with ffmpeg's SBC encoder: 44.1 kHz stereo, bitpool
  54, 120-byte frames, 20 s each).
- **Output:** `var/hello12.log` (`RESULT` lines, io-media events, the
  stream server's notes and stats), `var/hello12/wms.log` (step `w`),
  `var/hello12/slog.txt`.
- **Screen:** a page before each step names the sound to listen for, then
  "Listening: …" while it plays, then the results. Given back on every exit.

## Steps

- `w`: **read only.** Lists `/dev/wms`, then opens the Bluetooth stack's
  `/dev/wms/player1` read-only and sends the two devctls io-media sends
  before every Bluetooth play: `0x4004051e` (connected?) and `0x40100520`
  (state, codec, rate). It reads nothing from the stream and writes
  nothing. Runs in a child (`hello12 <sd> @wms`); a child that hasn't
  finished after 6 s is killed and left behind.
- `f`: control. The ping clip from the card as `sbc:<sd>/clips/a1_sbc_ping.sbc`:
  a 0.5 s 880 Hz ping every 2 s.
- `s`: live. The rising-tone clip served at `/dev/mmi_apps/live.sbc` by
  hello12's resource manager (`sbcsrv.cpp`), in real time from io-media's
  first read, ~200 ms ahead. A read past what is out gets what is there
  (a short read), or waits if nothing new is out yet.
- `S`: the same, with each read held until everything it asked for is out.

The server logs what io-media asks of a stream: opens (pid, flags), the
first 30 reads (size, offset, wait), every devctl code (answered by
`iofunc_devctl_default`), unblocks and closes. That is what a stand-in for
`/dev/wms/player1` would have to answer.

## Results

Built, not yet run on the car.
