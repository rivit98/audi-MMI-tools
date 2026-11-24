# hello12

Live SBC audio through io-media's Bluetooth reader on the DM642, the path
the MMI uses for phone music ([docs/bt-audio-path.md](../../docs/bt-audio-path.md)).
io-media turns the input `sbc:<path>` into `BT@44100.16.2.SBC:<path>` and
decodes it on the DSP. The path can be hello12's own resource manager, so
an app can play a live stream without the Bluetooth stack. Output:
`mixer:DSPOUT_TDM1_20_SAD?delay=130`, so the radio goes quiet while a
sound plays.

- **Run:** `hello12 <sd_path> [steps]` (default `wfsSW`). Needs
  `clips/a1_sbc_ping.sbc` and `clips/a2_sbc_sweep.sbc` (`make sd`; made by
  `media/mkclips.sh` with ffmpeg's SBC encoder: 44.1 kHz stereo, bitpool
  54, 120-byte frames, 20 s each).
- **Output:** `var/hello12.log` (`RESULT` lines, io-media events, the
  stream server's notes and stats), `var/hello12/wms.log` (step `w`), `var/hello12/wms_W.log` (step `W`'s look
  at the stack before it takes the path),
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
- `W`: **stand-in for the Bluetooth stack** (needs a phone connected over
  Bluetooth, playing music in Media → Bluetooth audio). First the same
  read-only look as `w`. Then hello12's resource manager attaches at
  `/dev/wms/player1` (the newest attach of a path wins, so the MMI's next
  open reaches us) and serves the rising-tone clip in a loop, live, with
  short reads. It answers what io-media's `a2dp_player` graph asks
  (`wms.h`): `0x4004051e` → 1 (connected), `0x40100520` → streaming, SBC,
  44.1 kHz, `0x532` → EOK. Every other devctl gets `ENOSYS` and is logged
  with the caller's pid. The driver switches Media to another source and
  back: the graph opens the path again and should play the tone instead of
  the music. After 2 minutes hello12 detaches (the stack's node is back;
  a read still waiting gets end of stream), and the driver switches once
  more: the phone's music should come back. Our screen is away while the
  driver uses the HMI. Nothing is killed or written to the stack.

The server logs what io-media asks of a stream: opens (pid, flags), the
first 30 reads (size, offset, wait), every devctl code (answered by
`iofunc_devctl_default`), unblocks and closes. That is what a stand-in for
`/dev/wms/player1` would have to answer.

## Results

All four steps work [confirmed: hello12.log, a video with sound]:

- `w`: `/dev/wms` holds `headphone1` and `player1`. On `player1`,
  `0x4004051e` answers 0 (no phone streaming) and `0x40100520` answers
  state 1, codec 1 (SBC), rate 0 (44100 Hz).
- `f`, `s`, `S`: io-media plays all three on `mixer:DSPOUT_TDM1_20_SAD`:
  `FINISHED`, 31 `TIME` events, first after ~140 ms, ~20.1–20.3 s for a
  19,998 ms clip. On the video the pings and both rising tones are heard
  without gaps; the radio is muted while they play (as for every
  `TDM1_20_SAD` sound, hello5).
- io-media reads a live stream in **512-byte reads**, never `pread`. It
  opens the file twice (ioflag `0x8001`, then `0x8880`, closed at once),
  sends one devctl `0x40600c00` (96 bytes), and plays on when it gets
  `ENOSYS`. Short reads are fine: 2,827 of 2,846 reads were short with no
  audible effect; held reads (up to 22 ms) are fine too.

So a resource manager can feed io-media live SBC through the `sbc:` input,
which is what a stand-in for `/dev/wms/player1` needs.
