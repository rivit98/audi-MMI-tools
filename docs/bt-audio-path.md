# Bluetooth audio (A2DP): how the MMI plays it, and how an app can use the path

How phone music over Bluetooth reaches the speakers on the 9411 / P1001
unit, and which parts of that path an app can feed with its own stream.
The goal is sound that the HMI treats as a media source (Media →
Bluetooth audio). Then audio management switches the source, sets the
volume and lowers the radio for announcements itself, unlike
`mixer:DSPOUT_TDM1_20_SAD`, which mutes the radio
([platform-notes.md](platform-notes.md) §9).

Sources: efs-system extracted with `tools/f3swalk.py`, P1001 rev 51.
io-media-nvidia addresses are virtual addresses in
`/mnt/efs-system/sbin/io-media-nvidia` (text at `0x08040000`); library
addresses are offsets in the `.so`.

## 1. The path in one picture

```
phone ──A2DP (SBC)──► PSSBSSProcess (Harman BT stack, BSS)
                        │ A2DP sink, SBC NOT decoded here
                        ▼
                      MediaRMService: resource manager /dev/wms/player1
                        ▲ open, devctl, read()
                        │
io-fs-media -d mediabt ─┤ (mediafs: AVRCP control, track info,
  (iofs-mediabt.so)     │  DCMD_MEDIA_URL -> "wms:/dev/wms/player1")
                        │
MME (mme-becker) ── storage type 12 ──► io-media graph "a2dp_player"
                                          │ "BT@44100.16.2.SBC:/dev/wms/player1"
                                          ▼
                                        DM642 SBC decoder ──► mixer output ──► amplifier
```

## 2. The Bluetooth stack side

**PSSBSSProcess** (efs-system `pss/pssbss/usr/bin/`) hosts the Bluetooth
stack and its services as plug-in libraries (`pss/pssbss/usr/lib/`). Its
config is `pss/pssbss/usr/data/pss.cfg`. [confirmed in files]

- **The A2DP sink does not decode SBC.** `pss.cfg`:
  `BSS_A2DP.UseInternalSbcDecoder = NO`, `BSS_A2DP.AdditionalDecoders = 0,
  0xFFFF02F0` (the SBC capabilities it advertises), `BSS_A2DP.AudioSink1 =
  0, 0, Sample, 3, 672, 20, 8192, 20, STREAM`. The comment above it says
  "should external encoders/decoders be used for SBC" (German). The
  phone's SBC frames leave the stack as they are. [confirmed in config]
- **`/dev/wms/<name>` is the stream's resource manager.**
  `libmediarmservice.so` ("MediaRMService: Harman/Becker", ID_MR 9001)
  imports `resmgr_attach`, `iofunc_*`, `MediaStreamOut_Read` and
  `MediaStreamIn_Write`, and has the string `/dev/wms`. `pss.cfg` gives it
  two devices:

  | component | `DeviceName` | media | direction |
  |---|---|---|---|
  | `MediaRm1` `MediaRmPlayer1` | `player1` | `MediaPlayer` (A2DP sink: phone → MMI) | io-media reads |
  | `MediaRm2` `MediaRmHeadphone1` | `headphone1` | `MediaHeadphone` (A2DP source: MMI → headphones) | io-media writes |

  So phone music is read from **`/dev/wms/player1`** **[inferred: path
  from `/dev/wms` + `DeviceName`]**. The MME's output table has
  `a2dp://dev/wms/headphone1|2` for the other direction (mme_data.sql).
- Its devctl switch (`libmediarmservice.so` `0x6488`) compares the command
  with, among others, `0x4004051e`, `0x40100520`, `0x532`, `0x40040525`,
  `0x40010522`, `0x400c0520`. The first three are the ones io-media sends
  (§4). [confirmed in code]
- The events it handles (`MEDIA_EV_STREAMING_STATE_CHANGED`,
  `CONNECTION_STATE_CONNECTED_STREAM`, `codecType` `CODED`/`DECODED`,
  `rc_handle`) come from the stack over Harman's GCF message router.
  [confirmed in strings]

## 3. The MME side

- **io-fs-media with the `mediabt` driver** (srv-starter process 88:
  `/sbin/io-fs-media -d mediabt,verbose=5`; the driver is
  `pss/pssbss/usr/lib/iofs-mediabt.so`) presents the phone as a QNX
  mediafs device to the MME: AVRCP play/pause/skip, track metadata, player
  lists (`DCMD_MEDIA_*` log strings). It answers **`DCMD_MEDIA_URL`** with
  a URL built from the string `wms:` (two copies in the driver).
  [confirmed in strings; the exact URL is inferred]
- **The MME plays Bluetooth with io-media's `a2dp_player` graph**:
  `etc/mme.conf` `<RendererOverride><renderer storagetype="12"
  name="a2dp_player"/>`, and `etc/iom.cfg` has a `module-options` block for
  `a2dp_player` (`metadata-timeout = 2000`). [confirmed in config]
- MMI3GMedia's `A2DPPlayerPresCtrl` drives it through the MME like any
  other source (strings: `MME.getPlayState()`, `requestSetActiveMedia()
  with msid=%lld`, `starting SQL query on library for a FID with
  msid=%lld`, `mTracksessionMananger.setDeviceIsReadyForPlayback()`). The
  phone is an MME media store (msid) with one track (FID) whose playback
  is the A2DP stream. [inferred from strings]
- MMI3GTelephone owns the Bluetooth connection side (`CBluetoothPresCtrl`,
  `A2DP_SINK` / `A2DP_SOURCE`, `requestConnectService`), talking to the
  stack through a proxy, not to the stream. [confirmed in strings]

## 4. io-media: how a Bluetooth input is opened and read

All in io-media-nvidia, `nv-audio.cc` (its assert paths).

- **Graph type 2 = `a2dp_player`.** The graph descriptor table has
  `a2dp_player` → create function `0x0805fd80`, which stores `2` at graph
  `+0x20` (`mov #2,r1; mov.l r1,@(32,r2)` at `0x0805fd98`).
  `trackplayer` and `cdtrackplayer` use `0x0805fad4`. [confirmed in code]
- **`attach_input` (`FUN_08063a74`) sorts the input URL by prefix**:
  `file:`, `cdda:`, **`sbc:`** (`0x0827cf24`), `tmpfile:` and **`wms:`**
  (`0x0827cf2c`). Each prefix is cut off at the first `:` before use.
  Its log line names them: `tmpfile = %d, cdda = %d, wms = %d, sbc = %d,
  target url = %s`. [confirmed in code]
  - `sbc:<path>` (any graph): the input becomes
    **`BT@44100.16.2.SBC:<path>`**, file format `0x27`
    (`eFileFormat = NV_FILE_FORMAT_SBC`, in `FUN_08063a74`). Only
    44.1 kHz, 16 bit, 2 channels, SBC.
  - `wms:<path>`: `open64(path, O_RDONLY)` (`BLUETOOTH:: url = %s`); the
    fd is kept at graph `+0x13c+0x1c`.
  - Any other URL on an `a2dp_player` graph goes to the mediafs object
    (`FUN_0808c860`, under `gr->bt_mutex`): open, an ionotify thread for
    `DCMD_MEDIA_READ_EVENTS`, play/pause devctls (`0x0E` class =
    `_DCMD_MEDIA`). This is the path the MME uses.
- **Before playing a type-2 graph** (`FUN_08065c1c`, `play_prep`), up to 5
  tries 250 ms apart of `FUN_08065908`:
  1. open the wms path if not open (`wms:` cut off, trailing `/` removed;
     `BT_DEVCHECK open of %s succeeded`);
  2. `devctl(fd, 0x4004051e, int, 4)`: non-zero = `bt connected`;
  3. `devctl(fd, 0x40100520, 16 bytes)`: `{i32 state; i32 codec; i32
     rate; i32 unused}`. `state == 1` = streaming. `codec` indexes the
     table at `0x082a860c`: **0 `PCM`, 1 `SBC`, 2 `MPG`, 3 `AAC`, 4 `ATC`**
     (above 4 is refused, `0x10004`). `rate` 0 = 44100, 1 = 48000.
  4. The input becomes **`BT@<rate>.16.2.<codec>:<path>`** (sprintf in `FUN_08065c1c`).
  [confirmed in code]
- **On stop of a type-2 graph** io-media sends `devctl(fd, 0x532)` with
  no data (`BT_EOF devctl to BT stack failed!`, `FUN_08058c50`).
  [confirmed in code]
- **The `BT@` reader** (`0x080f7a00`–`0x080f8bc0`, not in the Ghidra dump)
  parses `BT@<rate>.<bits>.<channels>.<PCM|SBC>:<path>` (`*** No prefix
  BT@`, `*** Not a valid BT filename`), opens the path and reads it
  through `CNvFileHandleQnx::ReadFile` (`0x08195c80`): a plain blocking
  `read()`. If its timeout flag is set it arms `timer_timeout` first
  (`0x08195ea0`; the value is about 21 s, `ETIMEDOUT` →
  `TIMOUT during read data from bluetooth device`). `EINTR`, `EBADF` and
  other errors end the stream (`Bluetooth device has stopped streaming`).
  [confirmed in code; the 21 s value is read from the stack layout,
  inferred]
- The DSP has an SBC decoder and demuxer (`CMediaAudioSbcDsp`, `SBCDEC`,
  `8DemuxSbc`) and an SBC encoder for the headphone direction
  (`CEncodeAudioSbcDsp`, `SBCENC`). [confirmed in strings]

## 5. What this means for an app

**A live SBC stream from our own resource manager should play on the DSP
without the Bluetooth stack**: a `trackplayer` graph with the input
`sbc:<our path>` (io-media makes it `BT@44100.16.2.SBC:<our path>`),
output `mixer:DSPOUT_TDM1_20_SAD`. The reader only needs `open` and
blocking `read`, as the H.264 stream server already serves
([phone-mirroring.md](phone-mirroring.md) §7.10). This is still our own
graph on the one audible output, so it still replaces the radio. It solves
live audio, not the source management. **[confirmed: [hello12](../src/hello12/README.md)
`s` and `S` play a live stream from our resource manager, 512-byte reads,
short reads fine]**

**Phone audio over USB as SBC** would play the same way, fed to a live
`sbc:` input like hello12's stream. The phone app doesn't send audio:
Android Auto's music and navigation stay on the phone, over Bluetooth.

**PCM is only reachable on a type-2 graph**: the codec comes from devctl
`0x40100520`, and codec 0 is `PCM`. There is no `pcm:` prefix.

**An `a2dp_player` graph fed `wms:<path>` directly is unsafe.** It skips
the mediafs object, but `play_prep` still calls `FUN_0808d400` on that
object unless graph flag `0x40` is set. `FUN_0808d400` reads
`param_1[8]` with no NULL check, and no code that sets flag `0x40` was
found. If it isn't set, io-media dereferences NULL and crashes, taking all
media and video down. **[inferred from code; not tried]**

**Being the HMI's Bluetooth source** means io-media must read our stream
when the MME plays the phone's A2DP track. Its `a2dp_player` graph opens
the path from `DCMD_MEDIA_URL` (`/dev/wms/player1`) at `play_prep`.
A process that attaches its own resource manager at `/dev/wms/player1`
gets the opens made after it attaches. On QNX the newest attach of the
same path wins unless the first one used `_RESMGR_FLAG_BEFORE`
**[inferred from QNX path resolution, not checked against
MediaRMService's flags]**. That process must answer:

| request | answer |
|---|---|
| `open` (read only) | EOK |
| devctl `0x4004051e` (4 bytes out) | `1` (connected) |
| devctl `0x40100520` (16 bytes out) | `{1, 0 (PCM) or 1 (SBC), 0 (44.1 kHz), 0}` |
| `read` | the stream, blocking until data is there |
| devctl `0x532` | EOK |

What that takes, and costs:
- A phone connected over A2DP with Bluetooth audio chosen in Media, so
  that the MME has a media store and plays it. Without a phone there is
  no msid and the HMI offers no Bluetooth source. **[inferred]**
- The graph opens the path once per `play_prep`. Taking over a stream
  already playing needs a stop and play (another source and back).
- While we serve the path, the real stack's stream is not read. Detaching
  gives the path back to PSSBSSProcess for the next open. Nothing is
  killed or replaced, and nothing is written to the stack.
- hello12 step `W` does this with a looped test clip (built, not yet run
  on the car).

## 6. Open questions

- What the two devctls return while a phone streams (hello12 `w` / `W`
  with music playing). With no phone streaming: connected 0, state 1,
  codec 1 (SBC), rate 0 [confirmed: hello12 `w`].
- Does the MME's graph take our stand-in, and does anything besides
  io-media open or devctl `/dev/wms/player1` (hello12 `W`)?
- What sets graph flag `0x40` (whether a direct `wms:` input is safe)?
- The read framing on `/dev/wms/player1`: whole SBC frames, or with a
  header? The DSP's SBC demuxer syncs on the frame header, so plain
  frames are the likely answer. **[inferred]**
