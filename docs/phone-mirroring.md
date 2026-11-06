# Phone screen mirroring on the MMI 3G+: options and feasibility

How to show an Android phone's screen (Redmi Note 8 Pro, 1080×2340) on this
unit's 800×480 display. This doc assumes `audi_stream_receiver` works, i.e. we have a
working Android Open Accessory (AOA) bulk link between the phone and our own
SH4 app. The phone is used in **landscape only**.

Every claim is labelled:

- **[confirmed]**: read from the P1001 rev 51 binaries (Ghidra or disassembly,
  with an address where there is one) or measured on the car.
- **[inferred]**: follows from confirmed facts but hasn't been exercised.
- **[unverified]**: an estimate or assumption.

An HDMI→composite converter into the ADV7180 camera input is rejected and
not covered here.

---

## 1. Summary

| # | Option | Feasibility | Effort | Main risk |
|---|---|---|---|---|
| 0 | JPEG / PNG pictures over AOA, CPU-decoded (`audi_stream_receiver` + the Android app; src/audi_stream_receiver/README.md) | **Works on the car**: 2–4 fps with the MMI ~30 % busy (~10 fps ceiling) | done | Text blurs if the phone downscales in one bilinear step; fixed by k× capture + box average. PNG decode cost on the SH4 is in the receiver README |
| 1 | Raw pixels (dirty tiles + LZ4) over AOA, drawn with GL | **High** | ~1–2 weeks | Throughput: VFB writes from the CPU are slow (~10 MB/s); GL upload ~50 MB/s, one upload in flight |
| 1b | Same, but DXT1 tiles uploaded as compressed textures | **High** | +3–5 days on top of 1 | Text quality (block artifacts) |
| 2a | Hardware H.264 (DM642 DSP), stream fed to `io-media-nvidia` | **Works on the car**: a live stream from our own resource manager (§7.10) and the phone's H.264 over AOA (§7.13) | built | Every frame must fit one frame size S and the rate must stay steady; ~0.5 s latency from io-media |
| 2b | Hardware H.264, talking to the DSP driver directly | **Low** | months | Private NVIDIA protocol (`/dev/nvdsprm0`, `/dev/nvminirm0`, `NVshm-*`) |
| 3 | Software H.264 on the SH4 | **Rejected** | — | ~5–10 fps at best [unverified estimate] |
| 4 | scrcpy over adb (MMI acts as adb host) | **Medium** | ~2–3 weeks + 2a | Needs USB debugging always on. Video still needs 2a or 2b. |
| 5 | Android Auto receiver | **Low–medium** | months | TLS, protobuf, head-unit cert, audio. Video still needs 2a or 2b. |

**Where things stand:**

- **Option 0** is a deliberately simple stopgap built into
  `audi_stream_receiver`. It sends the phone's screen as JPEG or PNG
  pictures, one in flight, with portrait shown pillarboxed on the left
  (which departs from the landscape-only assumption above). It also
  measures real AOA receive and per-picture costs on the car (`video:`
  lines in `audi_stream_receiver.log`).
- **Option 2a** is the path that is built. What it rests on:
  - The DSP decodes H.264 Baseline (720×480, 720×336) and the picture
    reaches the screen through VCAP 0 input 1 (§7.4, §7.5).
  - io-media refuses anything wider than 720 or taller than 576 on purpose
    (§7.8).
  - None of the streamable inputs start: fragmented MP4, H.264 in MPEG-PS,
    growing files (§7.5, §7.10).
  - A plain MP4 with a constant sample size (CS-MP4) has a tiny header that
    can be written before the frames exist (§7.8). With a 32-bit `mdat`
    header (io-media's own demuxer can't read the 64-bit one, §7.9) it plays
    from a file and as a live stream served through blocking reads, at
    25 fps with a steady ~0.5 s lag and ~10 % CPU. Pauses in the stream are
    waited out (§7.10).
  - The phone's own H.264 played on the car through `audi_stream_receiver`
    (§7.13).
- **Option 1** is not built. Options 4 and 5 build on 2a.

---

## 2. What the unit gives us

### 2.1 A hardware video decoder: TI DM642 DSP [confirmed]

- **Boot.** srv-starter runs `/sbin/NvElfLoader -f /etc/NvDspCodecs.elf`.
  NvElfLoader looks for a "TI DM642 DSP device" on PCI and loads the
  `.nvidia.dspcode` image into it.
- **Driver.** `devg-NVTS` provides `/dev/nvdsprm0` (`dm642_channel_alloc` /
  `_program` / `_free`).
- **Codecs.** `NvDspCodecs.elf` contains TI C64x codecs:
  - `IS_H264_BP_Dec_C64x` (plus `TSK_H264BP` and a `TSK_H264MP` task)
  - `IS_MPEG2_Dec_C64x`, `IS_MPEG4_Dec_C64x`, `DIVXVDEC`
  - `WMV9_MP_AP_DEC`
  - audio: AAC-LC, MP3, AC3, DTS, WMA, SBC
  - DTCP, for DVD video arriving over MOST
- **Host side.** `io-media-nvidia -c /etc/iom.cfg` (on efs-system) wraps the
  DSP as NVIDIA's "nMSDK" player (`CMediaVideoH264Dsp`, `CNvH264dec`). It opens
  `/dev/nvdsprm%d`, `/dev/nvminirm%d` and `/dev/shmem/NVshm-%04X`.
- **H.264 limits** (io-media strings at `0x0828fbdc`…`0x0828fd90`):
  - Only **Baseline profile at Level 3 or below** is accepted: "Baseline
    profile H.264 bitstreams exceeding level 3 are not supported".
  - Main, High, Extended and the other profiles are each rejected with their
    own message.
  - Level 3 allows 1620 macroblocks per frame and 40 500 macroblocks per
    second.
- **Size.** io-media refuses width > 720 or height > 576, and more than
  25 fps above 480 lines (`FUN_080d1760`, §7.8). Both the host
  (`vmixer.c`) and the DSP (`NvMixer_Video.c`) also contain
  `assert(width < 720)`. 800×480 H.264 does not play on the car (§7.5).
- **Containers.** `iom.cfg` maps file extensions to parsers (full table in
  §7.8):

  | Extensions | Parser |
  |---|---|
  | mp4 / m4v / mov | `mp4_parser` |
  | avi / divx / xvid | `parser_avi` |
  | mpg / mpeg | `parser_mpgvfile` |
  | wmv / asf | `wma9_parser` |

  - io-media also has its own demuxers (`CDemuxMp4`, `CDemuxRaw`,
    `CDemuxMpgPs`); `.mp4` in the trackplayer goes through `CDemuxMp4`,
    not `mp4_parser` (§7.9).
  - io-media also has an MPEG-PS stream parser that resyncs on start codes
    (`CNvMpegPsParser`, "Failed resync!").
  - `mp4_parser.so` contains a full ISO-BMFF box-name table including the
    fragment boxes (`moof`, `traf`, `trun`, `tfhd`, `mvex`). The same table
    also lists image boxes (`iloc`, `pitm`), so it's a generic list.
    Fragmented MP4 does not play (§7.5).
  - The only URL scheme seen is `file://`.

### 2.2 How decoded video reaches the screen [confirmed on the car]

```
DM642 (decode) ──video port──> NV VCAP 0, input 1 ──> video layer ──> display
                                      ▲                    ▲
   dev-videoctrl: SET_SOURCE 1 + ENABLE + UPDATE on VP 1   │
         layermanagerV2: video displayable, flags 0x101 (gf_vcap_bind_to_layer)
```

- **Wiring.** `/etc/videoctrl-Audi3G.cfg`: VCAP 0 has two inputs, the
  ADV7180 (composite camera and RSE inputs, input 0) and the DSP's video
  port (input 1). VCAP 1 is fed by "Partial Decode" (possibly the GPU's
  MPEG-2 path, devg-NVMiniRM's `mpCreateDecoderObj` **[unverified]**).
  **On the car, DSP-decoded MPEG-2, MPEG-4 Part 2 and H.264 Baseline all
  show on VCAP 0 input 1; VCAP 1 shows nothing for any codec
  [confirmed]** (§7.4, §7.5).
- **io-media side.** io-media's render manager acquires a capture unit and a
  layer over IPC: "NEDL: Acquiring Capture Unit" and "Acquiring Layer with
  Width = %d, Height = %d" (`0x08296c78`…).
- **Layer manager side.** `layermanagerV2` has a `CVideoDisplayable` class.
  Its constructor (`FUN_08053238`) decodes the register flags:

  | Flag bit | Meaning |
  |---|---|
  | bit 0 | `LMGR_VIDEO`: this is a video displayable. Bit 0 is the "different displayable class" noted in the README. |
  | bit 8 | bind to VCAP 0 |
  | bit 9 | bind to VCAP 1 |

  Registering a displayable with flags `0x101` (VCAP 0) or `0x201`
  (VCAP 1) and showing it binds the capture unit to a layer, but the
  picture stays black until dev-videoctrl enables the capture (§7.3). With
  both, the decoder's output is on screen **[confirmed]**.
  Rendering calls `gf_vcap_set_output_dimensions(w, h)`, so VCAP can scale a
  smaller decoded picture up to the displayable's size.
- **Exclusivity.** io-media treats video as one global resource ("release
  internal_video lock (global video resource)"). Our graph and the HMI's
  video playback exclude each other.

### 2.3 Test clients for io-media already on the unit [confirmed]

efs-system ships QNX's io-media test tools, UCL-compressed in the "iwlyfmbp"
format. Inflate them with `tools/qnx_inflator.c`; it needs a one-line shim
that makes `lzo/minilzo.h` include `lzo/lzo1x.h`.

- **`testapp-file`:** `%C [options] URL...`
  - `-t` graph type, `-f` graph name, `-o` output device (default
    `/dev/snd/pcmPreferredp`, which **cannot be attached on this unit**:
    `iom_attach_output_to_graph: iom_errno=0x10001`, even with no URL. The
    unit's MME sends media to io-media **mixer inputs** instead. From
    the `outputdevices` table in efs-system's mme SQL:
    `mixer:DSPOUT_TDM1_51_SAD?delay=130` (`defaultoutput`),
    `mixer:DSPOUT_TDM2_20?delay=130`, `mixer:DSPOUT_TDM3_20?delay=130`, and
    `a2dp://dev/wms/headphone1|2?...` for BT. MMI3GApplication owns the
    `/dev/io-media/graphs/mixer/<pid>_0..2` graphs.)
  - `-s` play speed, `-n` notification interval, `-l` volume 0–100
    (undocumented; default 100, balance/fade 50)
  - Default graph type `trackplayer`. Attaches **one** output only, as
    audio (`iom_attach_output(graph, name, 1)`).
  - It enqueues URLs and logs io-media events (`INPUT_EOF`, `FINISHED`,
    `STATUS`, `TIME`).
  - It ignores SIGTERM, so stopping it means SIGKILL, and its buffered
    stdout is lost (§7.7).
- **`testapp-cmdline`:** an interactive client that also runs command files
  (`%C [options] [cmdfile]...`).
  - Options: `-t` graph type (default `trackplayer`), `-n` create a graph
    with that name, **`-a` attach to an existing graph by name**, `-o` audio
    output (default `snd:/dev/snd/pcmPreferredp`, which does not attach
    here, so pass `-O` for none), `-v` video output, `-e` don't read events,
    `-l` log file, `-c` control point.
  - Output types for `iom_attach_output`: 1 = audio, 2 = video. But
    io-media-nvidia's `nVAudio_attach_output` / `discplayer_attach_output`
    only know audio targets (`mixer:`, `a2dp:`, `file:`, `wms:`). Video
    is not an output: FileVideo hands it to the render manager inside
    io-media ("NEDL: Acquiring Capture Unit", "Acquiring Layer").
  - Commands, one per line: `AttachInput <url> <id>`, `Play`, `Stop`,
    `GetStatus`, **`GetTrackInfo`** (prints `, video ('<codec>', ...
    WxH)` or `, no video`), **`GetVideoInfo`** (`w=.. h=..`),
    `GetErrorInfo`, `GetTime`, `SetVolume`, `sleep <ms>`, `quit`, and
    others (`list`, `help`).

These let us test the decoder without writing any io-media client code.

### 2.4 Graphics paths for raw pixels

- **CPU writes to the VFB are slow [confirmed, roughly].** lmprobe's
  `fill_bars` (one 32-bit store per pixel, 800×480) takes about 150 ms.
  That's about 10 MB/s. The per-pixel divide in the loop accounts for only
  a fraction of it. This looks like uncached, word-at-a-time writes across
  PCI into GPU memory **[inferred]**. A full-screen CPU blit would therefore
  cap at about 6 fps.
- **GL path [confirmed exports].** `libGLES_CM.so.6` exports:
  - `glTexSubImage2D`
  - `glCompressedTexImage2D` / `glCompressedTexSubImage2D`
  - `glDrawTexiOES` (`GL_OES_draw_texture`: a screen-aligned blit with no
    vertex setup)

  The extensions include `GL_EXT_texture_compression_s3tc` and
  `GL_EXT_texture_compression_dxt1`. Upload goes through the NV driver, which
  can use DMA (`devg-NVMiniRM -d 11` is the DMA channel).
- **Measured (hello6 T3, details in [`gpu-results.md`](gpu-results.md))
  [confirmed]:** 800×480 RGBA upload **31 ms ≈ 50 MB/s** with `glFinish`,
  CPU RGB565→BGRA 11.9 ms/frame, LZ4 UI-frame decompress 8.8 ms.
  Back-to-back uploads froze the unit, so a client keeps one upload in
  flight.

### 2.5 USB

`io-usb -d ehci -d ohci`: the host controller supports high speed
**[confirmed]**. Bulk throughput through `libusbdi` on this CPU is
**[unverified]**; my guess is 5–20 MB/s. Every compressed-video option needs
less than 1 MB/s, so only options 1 and 1b depend on this number.

---

## 3. Phone side (shared by options 1, 1b and 2a)

| Piece | How | Feasibility |
|---|---|---|
| Capture | MediaProjection → VirtualDisplay. One consent dialog per session, no root. Needs a foreground service. | High: standard API since Android 5 |
| Raw frames (1, 1b) | VirtualDisplay → `ImageReader` (RGBA_8888) → tile diff → RGB565 + LZ4, or DXT1 | High. The phone CPU handles 800×480 easily. |
| H.264 (2a) | VirtualDisplay → `MediaCodec` encoder surface, Baseline Level 3. Built and working on the car (§7.12, §7.13). | Confirmed on this phone |
| Transport | Existing AOA bulk link (`audi_stream_receiver` protocol, with new frame types) | Confirmed working |
| Input | **AOA v2 HID**: the accessory registers a HID device with the phone over control transfers (54 `REGISTER_HID`, 56 `SET_HID_REPORT_DESC`, 57 `SEND_HID_EVENT`). This is what scrcpy's OTG mode uses. The knob from `/dev/ipc/ch8` maps to keyboard D-pad, Enter and Back, or to a mouse. No adb, no root. | High. The MMI side reuses the existing `usbd_setup_vendor` path. |

### Landscape only

There are two ways to force it, neither needs root:

- (a) An app with the `WRITE_SETTINGS` special permission (granted once by
  the user) sets `ACCELEROMETER_ROTATION=0` and `USER_ROTATION=1`.
- (b) A `SYSTEM_ALERT_WINDOW` overlay declared with `screenOrientation=landscape`.
  "Rotation control" apps use this trick.

Use (a) and restore the old settings when the session ends.

Geometry for the 2340×1080 panel (aspect 2.17) on 800×480 (aspect 1.67):

| Mode | Phone area captured | Shown on MMI | Encode at (H.264, ×16) |
|---|---|---|---|
| Letterbox | whole screen | 800×369, black bars top and bottom | 720×336, scaled up by VCAP |
| Fill (crop sides) | centre 1800×1080 | 800×480 | 704×416 (800 wide is refused, §7.8) |

Letterbox is the safe default: nothing is cut off, and 720×336 is far inside
Level 3. A MediaProjection VirtualDisplay of a different aspect ratio
letterboxes by itself. The built H.264 path encodes 720×432 (5:3, the
screen's shape) with the letterbox inside it (§7.12).

---

## 4. Options in detail

### Option 1: dirty tiles + LZ4, drawn with GL

**Design.**
- **Phone:** capture → split into 32×32 tiles → hash each tile → send only the
  changed ones as RGB565, LZ4-compressed, with a tile index.
- **MMI:**
  1. decompress the tiles into a cached CPU-side copy of the frame
  2. `glTexSubImage2D` the changed rows into a 1024×512 RGB565 texture
  3. `glDrawTexiOES` the 800×369 region
  4. `eglSwapBuffers`

  The CPU never writes the VFB directly (see §2.4).

| | |
|---|---|
| Confirmed | AOA link, EGL/GL bring-up (hello2–4), `glTexSubImage2D` / `glDrawTex*OES` exported, 60 Hz vsync, knob input, GL upload ~50 MB/s with one upload in flight (§2.4), LZ4 UI-frame decompress 8.8 ms |
| Unverified | USB bulk MB/s |
| Pros | Sharpest image. Lowest latency (one frame of encoding). Almost no new unknowns. The phone app is plain Kotlin. |
| Cons | Busy content (scrolling, video, map panning) sends whole frames: 800×369×2 = 590 KB before LZ4. At 10 MB/s of upload or USB that's about 15 fps. |
| Effort | MMI: ~1 week (GL path from hello3, protocol from audi_stream_receiver). Phone: ~1 week (MediaProjection, tiles, LZ4 via `lz4-java`, HID). |

### Option 1b: DXT1 tiles

Same pipeline. The phone encodes changed tiles to DXT1 (4 bits per pixel,
fixed: about 150 KB per full letterboxed frame) using stb_dxt through the
NDK, or a GLES 3.1 compute shader. The MMI uploads with
`glCompressedTexSubImage2D` and does no pixel work at all.

| | |
|---|---|
| Confirmed | S3TC/DXT1 extensions and the compressed-upload entry points |
| Unverified | Whether the NV18 driver accepts sub-image updates of DXT textures at 4×4-aligned offsets (GL requires it to; drivers vary). Upload speed. |
| Pros | Bandwidth fixed at a quarter of RGB565. Zero CPU decode work. Can be mixed per tile: DXT1 for "photo" tiles, LZ4 RGB565 for text tiles. |
| Cons | DXT1 block artifacts on small text. Phone-side encoder needs the NDK or a compute shader. |
| Effort | +3–5 days on top of option 1 |

### Option 2a: hardware H.264 through io-media

**Design.** The phone sends Baseline Level 3 H.264 over AOA. The MMI app:

1. runs a QNX resource manager at e.g. `/dev/mirror/live.mp4` that serves
   the incoming stream as a **constant-sample-size MP4** (§7.8): a fixed
   `moov` declaring hours of frames, then frame k at `mdat + k·S`, each
   padded to S bytes with a filler NAL. Reads block until frame k has
   arrived. (Fragmented MP4, MPEG-PS and growing files don't work, §7.5,
   §7.8, §7.10.)
2. tells io-media to play `file:///dev/mirror/live.mp4` (the same calls
   `testapp-cmdline` makes: attach input, attach output, play).
3. registers and shows a **video displayable with flags 0x101** (VCAP 0)
   through `lmgr.c`, then enables VCAP 0 input 1 through `/dev/videoctrl`
   (VP 1: SET_SOURCE 1, ENABLE 1, UPDATE), as hello6 does (§7.4).

| | |
|---|---|
| Confirmed | The DSP H.264 BP decoder exists. io-media's profile/level checks. The DSP → VCAP 0 input 1 → layer path, on screen, with files played by `testapp-file` outside the HMI: MPEG-2 720×576 (§7.4), MPEG-4 640×480, **H.264 BP 720×480 and 720×336 in plain MP4** (§7.5). The `LMGR_VIDEO` / VCAP flag bits in layermanagerV2. The test clients. `file://` input. **Not playing:** 800×480 H.264, fragmented MP4, H.264 in MPEG-PS, growing files (io-media error `0x80002` at once). |
| Explained by RE (§7.8, §7.9) | 800 wide: io-media's own check refuses width > 720 or height > 576. CS-MP4 needs a 32-bit `mdat` header: io-media's `CDemuxMp4` can't read the 64-bit one. |
| Confirmed on the car (§7.10, §7.13) | CS-MP4 with filler-padded frames and a `moov` over-declaring 10 min plays. A live stream from our own resource manager plays for 30 s with reads that block ~90 ms each; io-media reads sequentially in 64 KB and only ~2.7 frames ahead. Lag ~0.5 s, steady. Pauses of 2 s and 5 s are waited out (no unblock); after one the picture skips the ~9 queued frames and goes on at 1×. ~10 % unit CPU. A growing file on the card does not work (EIO). The phone's own encoder output, streamed over AOA, plays (720×432, lag 600–650 ms). |
| Unverified | Where the ~9-frame queue lives and whether S or the frame rate changes it. Sessions longer than a few minutes, and the 4 GB limit (~1.6 h at 28 KB × 25 fps). Coexistence with MMI3GMedia, which owns io-media and the global video lock, and with the rear camera on VCAP 0 input 0. |
| Pros | Bandwidth set by S, whatever the content. 25 fps. The SH4 is mostly idle. Smooth for video and map panning. |
| Cons | Latency ~0.5 s from io-media plus the phone's encoder and USB: fine for viewing, sluggish for interaction. Fragile next to the HMI's own media playback. A DSP crash path exists ("dsp crash, remaining retries"). |
| State | Built: resource manager, iom client and video displayable (hello9, `src/common`), the receiver's side (§7.11) and the phone's encoder (§7.12). |

### Option 2b: direct DSP access

Reimplement the nMSDK client: the `/dev/nvdsprm0` channel API, the
`/dev/nvminirm0` resource manager, `NVshm-*` buffers, and the H.264
bitstream-buffer API (`BufAPI(H264)`). This gives the lowest-latency hardware
path, but it means reverse engineering a large C++ binary that talks a
private protocol to a C64x DSP with no symbols. **Feasibility: low.** Only
consider it if 2a's latency is unacceptable.

### Option 3: software H.264 on the SH4: rejected

There's no SIMD, the CPU is 792 MHz, and the GPU has no shaders for YUV→RGB.
Register combiners could do the colour conversion, but the decode alone is
estimated at 5–10 fps at 720×336 **[unverified estimate]**. Options 1 and 2a
dominate it.

### Option 4: scrcpy over adb

**Design.**
1. The MMI implements the adb host protocol (CNXN / AUTH / OPEN / WRTE /
   OKAY) on the phone's adb interface (`ff/42/01`; `audi_stream_receiver` currently
   skips it).
2. It pushes the official `scrcpy-server` (Apache-2.0) and starts it with
   `app_process`.
3. It reads the H.264 stream (Baseline/Level 3 via the codec options,
   `max-size 720`, `max-fps 25`, orientation locked) and injects input through
   scrcpy's control socket.

| | |
|---|---|
| Confirmed | Our USB stack handles arbitrary interfaces. The adb interface is visible to us. |
| Unverified | Everything on the MMI side. RSA-2048 signing for AUTH needs a small bignum or BearSSL; it runs once per connection, so SH4 speed is fine. |
| Pros | No custom phone app and no consent dialog. Full touch-level input injection. Can create a separate virtual display, so the phone screen stays free. Audio capture on Android 11+. |
| Cons | **USB debugging must stay enabled** (a security cost in a car). Replaces the AOA work. Video still needs 2a/2b, or a raw mode instead of H.264. |
| Effort | 2–3 weeks for adb plus scrcpy, on top of 2a |

### Option 5: Android Auto receiver

**Design.** AOA with the strings "Android" / "Android Auto". The MMI is the
TLS client (we'd need a head-unit certificate taken from a commercial unit,
which is legally grey). Then protobuf channels for video, input (with
**rotary-controller** support, which maps directly onto the MMI knob),
sensors, and audio.

| | |
|---|---|
| Confirmed | AOA works. **No TLS library on the unit** (searched ifs and efs), so BearSSL or mbedTLS gets compiled in; both are plain C. C++ stacks (aasdk/OpenAuto) can't be used: the unit's libstdc++ is GCC 3.3-era. |
| Unverified | The AA video level at 800×480 may be signalled above 3, which io-media rejects; we'd patch `level_idc` in the SPS. AA's 800×480 is also wider than io-media's 720 limit. Media and guidance audio need a way out: QSA devices are never heard, but io-media's `mixer:DSPOUT_TDM1_20_SAD` is (hello5; platform-notes §9). |
| Pros | The best end result: a car UI built for a knob, no portrait/landscape problem, no mirroring artifacts. |
| Cons | Biggest effort. Video depends on 2a/2b, and 2a's latency hurts AA's interactivity. Certificate legality. |
| Effort | Months |

---

## 5. Input mapping (any option)

`/dev/ipc/ch8` already gives us main-knob detents (bytes 6 and 7) and button
presses **[confirmed]**. Send them to the phone as AOA v2 HID events:

| Knob | HID report |
|---|---|
| detent one way | key Tab, or D-pad Down |
| detent other way | Shift+Tab, or D-pad Up |
| press | Enter |
| BACK button | Esc (Android maps it to Back) |

Decoding the remaining buttons in ch8 is still open (see platform-notes §8).

---

## 6. Notes on the RE sources

- **Damaged io-media binary.** A copy of `io-media-nvidia` carved from an
  efs extraction has a corrupt section table, and its data segment sits
  0x1dc8 bytes past where its program headers say. Ghidra loads the code
  correctly, but the GOT, the dynamic table and string cross-references are
  wrong. Use `tools/f3swalk.py`, which extracts efs-system by following F3S
  extents: its io-media-nvidia has consistent program headers (md5
  `6b21d49a…`). All io-media addresses in this doc come from that copy.
- **Output devices.** MME fills its `outputdevices` table at runtime. The
  mixer names from §2.3 attach (§7.1); the default in `testapp-file`,
  `/dev/snd/pcmPreferredp`, does not.

---

## 7. Hardware video: tests and results

Two test apps on the car, both run outside the HMI:

- **hello6** (`src/hello6`, `make sd SD_APPS=hello6`, ~6 min, parked):
  plays clips from `media/mkclips.sh` through the unit's own
  `testapp-file`, and runs throughput benchmarks. Results: `hello6.log`
  (`RESULT` lines and a summary) and `hello6/` on the card.
- **hello9** ([src/hello9/README.md](../src/hello9/README.md)): its own
  io-media client and resource manager, for CS-MP4 files and live streams
  (§7.8, §7.10).

**hello6 T1: hardware decoder.** Clips of 22 s each (moving `testsrc2`
pattern with the clip's name and a frame counter; the audio track is
generated silence, `anullsrc`):

| # | Clip | Question |
|---|---|---|
| 1 | MPEG-2 720×576 in MPEG-PS | Control: the DVD player's format |
| 2 | MPEG-4 Part 2 640×480 in AVI | Control: another DSP decoder |
| 3 | H.264 BP L3 720×480 MP4 | H.264 inside every known limit |
| 4 | H.264 BP L3 800×480 MP4 | Does the width limit bite? |
| 5 | H.264 BP L3 720×336 MP4 | The letterbox size |
| 6 | Same, no audio track | Does the player need audio? |
| 7 | Same, fragmented MP4 | Streamable MP4 layout |
| 8 | H.264 + MP2 in MPEG-PS | The other streamable container |
| 9, 10 | Clips 7 and 8 written to the card at real-time rate while they play, after a 2 s head start (**T2: growing input**) | Does io-media read past the end of a growing file? |

Each plays through `testapp-file` (graph `trackplayer`, `-n 500` for
progress events every 500 ms, `-o mixer:DSPOUT_…`: the mixer outputs from
§2.3 are tried in turn until one attaches). While it plays, hello6 shows
only a video displayable (cid 41), first on VCAP 1 (`0x201`, VP 2 source
0), then on VCAP 0 input 1 (`0x101`, VP 1 source 1), 7 s each. Per clip it
records:
- io-media `TIME` events and the furthest position (playback progress)
- knob events per attempt (what the driver saw)
- register/show/videoctrl return codes
- a query of the playing graph through `testapp-cmdline -a hello6 -O -e`
  (`GetStatus`, `GetTrackInfo`, `GetVideoInfo`, `GetErrorInfo`) into
  `hello6/query_<k>.log`; RESULT lines carry `video=` and
  `video_info=WxH`
- per-thread CPU load (`hbhogs`) while decoding

`sloginfo` runs right after the video phase into
`hello6/slog_video.txt`, before the GPU phase.

**hello6 T3: throughput** (`bench.c`), before the videos:
- (a) CPU: memcpy, RGB565→BGRA conversion, LZ4 compress/decompress on
  UI-like, photo-like and random frames (`lz4mini.c`, verified against
  liblz4 by `make test`)
- (b) VFB: 32-bit stores, memcpy in, memcpy out
- (c) GL: `glTexSubImage2D` RGB565 / RGBA / BGRA and
  `glCompressedTexSubImage2D` DXT1 MB/s
- (d) GL: visible upload + draw + swap fps, for full RGB565 frames, a
  moving 120-line RGB565 band (tile-diff-like) and full DXT1 frames

Key numbers are in §2.4, the rest in [`gpu-results.md`](gpu-results.md).
AOA bulk receive throughput needs the phone, so it belongs in
`audi_stream_receiver`.

### 7.1 Audio output: the default device is refused

With the default output `/dev/snd/pcmPreferredp` (→ `pcmC0D0p`, `ent3`)
`testapp-file` exits at once with `iom_attach_output_to_graph:
iom_errno=0x10001`, for every clip and also with no URL at all. So it is
the output io-media refuses, not the clips, the containers or the
decoder. With `-o mixer:DSPOUT_TDM2_20?delay=130` every output attaches
(§2.3).

### 7.2 io-media takes the clips as video

The graph query shows io-media opened the clips as video: clip 1
`GetTrackInfo` = `video ('MPG ', 25Hz 720x576)`, `iom_get_video_info`
720×576; clips 2 and 3 the same at 640×480 and 720×480.

io-media-nvidia's trackplayer (`nVAudio`) sends a file it sees as video to
the DSP's `FileVideo` player ("This is a VIDEO source"). It falls back to
audio only on "Video is in use... reverting to audio only" (the global
video lock) or "Attempted video play FAILED, trying source as AUDIO...".

Playing to the end (TIME events, `INPUT_EOF`, `FINISHED`) does not mean a
picture is on screen: without capture enabled (§7.3) both VCAPs stay black
while the clips play.

### 7.3 Capture must be enabled through dev-videoctrl

From io-media-nvidia (the `tools/f3swalk.py` copy, §6), dev-videoctrl and
layermanagerV2:

```
layermanagerV2  CVideoDisplayable::renderLayer: gf_vcap_attach, gf_vcap_bind_to_layer,
                gf_vcap_set_output_dimensions, gf_vcap_update  (never gf_vcap_enable)
dev-videoctrl   /dev/videoctrl: DEVCTL_VP_SET_SOURCE / _ENABLE / _UPDATE
                -> gf_vcap_set_input_source / gf_vcap_enable / gf_vcap_update
devg-nv.so      nv_vcap_*  ->  libned nedvVideoUnitAcquire / LayerAcquire / Enable ...
                -> IPC "nMSDKIPC"
io-media        CNvmRenderManagerIpc: OnIpcCaptureEnable ("Layer must be connected
                before enabling capture"), "NEDL: Enabling Capture", LayerAcquire, ...
```

A capture unit only runs once someone enables it, and on the stock unit that
is the HMI (MMI3GApplication `CVideoControlInterface::setCapActivity`,
`DEVCTL_VP_ENABLE`) through dev-videoctrl. A video displayable on its own
binds the VCAP to the layer and shows black. dev-videoctrl's devctls
(io_devctl at `0x08042640`, P1001), class `_DCMD_MISC`, payload
`{int vp, int value}`, exactly 8 bytes:

| dcmd | name (HMI's log) | effect on VCAP VPs |
|---|---|---|
| `0x80080501` | `DEVCTL_VP_SET_SOURCE` | `gf_vcap_set_input_source(vcap, value)` |
| `0x8008050b` | `DEVCTL_VP_ENABLE` | value ≠ 0 `gf_vcap_enable`, 0 `gf_vcap_disable` |
| `0x80040507` | `DEVCTL_UPDATE` (4 bytes) | applies all pending values, then `gf_vcap_update` |
| `0x80080500/02..05`, `0x8014_0509`, `0x800c050a`, `0x8008050c` | standard, colour, viewport, input dimensions, deinterlacing | (not used) |
| `0xc0080506` | `DEVCTL_VP_GET_STATUS` | |

Setters only store the value; nothing happens before UPDATE. A payload that
fails validation is **ignored with EOK**. Only the slog (`VP<n>: source
changed to <s>`, `VP<n>: vcap enabled`; dev-videoctrl runs with `-v`)
confirms. VP numbers follow `/etc/videoctrl-Audi3G.cfg`: VP 0 = ADV7180, VP 1
= VCAP 0 (input 0 = ADV7180, input 1 = DM642 video port), VP 2 = VCAP 1
(input 0 = Partial Decode). io-media runs with `NMSDK_DSP_PD=1`
(srv-starter environment), and its render manager is the server behind
VCAP 1, so VCAP 1 looked like the likely path for file video. **On the car
file video comes out on VCAP 0 input 1 (§7.4).**

### 7.4 The display path: VCAP 0 input 1

Clip 1 (MPEG-2 720×576 in MPEG-PS) through `testapp-file -o
mixer:DSPOUT_TDM2_20?delay=130`, with the video displayable (cid 41) shown
and capture enabled through `/dev/videoctrl`. Every devctl returns 0, and
dev-videoctrl confirms each one in the slog:

| attempt | slog | on screen |
|---|---|---|
| VCAP 1, flags `0x201`, VP 2 source 0 | `VP2: source changed to 0`, `VP2: vcap enabled` … `vcap disabled` | **black**. Every second or so, one thin grey line with a pink dot near the bottom edge |
| VCAP 0, flags `0x101`, VP 1 source 1 | `VP1: source changed to 1`, `VP1: vcap enabled` … `source changed to 0`, `vcap disabled` | **the moving test picture**, ~1.5 s after ENABLE until DISABLE |

What the picture shows:

- **Real time, smooth.** The burnt-in frame counter advances 25 per
  second, the clip's rate. io-media's TIME events agree.
- **Whole frame, 4:3, centred.** The 720×576 frame fills the panel's
  height and about 640 of its 800 columns, with black bars left and right.
  The displayable is registered at 800×480, so the aspect-preserving fit
  is done further down (io-media's render manager or the VCAP output
  dimensions). Which one, and whether it can be told to fill 800×480, is
  **[unverified]**. (720×336 and 720×432 H.264 are scaled to the panel's
  width, §7.10, §7.13.)
- **Correct colours**, readable small text (the timecode in the top-left
  corner of the clip).
- io-media logs no `NEDL:` lines at all, so dev-videoctrl's slog lines
  and the knob are the only signals on the unit.

The HMI's own sequence for VP 1 (slog at boot) also sets an input viewport
(x 6, width 708, height 480), input dimensions 720×480 and `WEAVE`
deinterlacing before enabling. None of these are needed for a correct
picture. Those devctls are available if a stream needs them (§7.3 table).

**Conclusions**

1. **The display path for hardware-decoded video:** register a
   video displayable with `0x101`, show it, then on `/dev/videoctrl`
   `SET_SOURCE(1, 1)`, `ENABLE(1, 1)`, `UPDATE`. To stop: `ENABLE(1, 0)`,
   `SET_SOURCE(1, 0)`, `UPDATE`, unregister. The HMI picture comes back
   cleanly afterwards.
2. VCAP 1 ("Partial Decode") is not where io-media's file video goes. The
   grey line is what an enabled VCAP 1 shows with no source feeding it
   **[inferred]**.

### 7.5 What plays and what doesn't

All clips through `testapp-file -o mixer:DSPOUT_TDM2_20?delay=130`, each
shown on VCAP 1 and then on VCAP 0 input 1. Every register, show and
videoctrl call returns 0. "Seen" means the driver saw the moving picture
(knob clicks during that attempt). The result is the same on every
repeat:

| # | clip | io-media (query, TIME events) | VCAP 1 | VCAP 0 in 1 |
|---|---|---|---|---|
| 1 | MPEG-2 720×576, PS | `MPG` 25 Hz 720×576, plays to the end | black | **seen** |
| 2 | MPEG-4 Part 2 640×480, AVI | `AVI` 640×480, plays to the end | black | **seen** |
| 3 | H.264 BP L3 720×480, MP4 | `MP4` 720×480, plays to the end | black | **seen** |
| 4 | H.264 BP L3 800×480, MP4 | `GetTrackInfo` EIO, `iom_errno 0x80002`, 0 events | black | black |
| 5 | H.264 BP L3 720×336, MP4 | `MP4` 720×336, plays to the end | black | **seen** |
| 6 | same, no audio track | `MP4` 720×336, plays to the end | black | **seen** |
| 7 | same, fragmented MP4 | `0x80002`, 0 events | black | black |
| 8 | H.264 + MP2 in MPEG-PS | `0x80002`, 0 events | black | black |
| 9 | clip 7 growing on the card | `0x80002`, 0 events | black | black |
| 10 | clip 8 growing on the card | `0x80002`, 0 events | black | black |

The picture appears 1.5–4 s after capture is enabled.

- **H.264 Baseline plays on screen**: 720×480 and the 720×336 letterbox
  size, with and without an audio track (clips 5 and 6: an audio track
  makes no difference). That is the decode-and-display half of option 2a,
  done with the unit's own player and no RE. `GetTrackInfo` reports the
  clips' silent audio as `0ch 0Hz`, so these tests say nothing about clip
  audio.
- **Every failing clip fails the same way, at once**, before the first
  TIME event, so the player never gets as far as the decoder or the VCAP.
  `0x80002` itself is only a symptom (§7.7). The reasons: 800 wide is
  refused by io-media's size check (§7.8); fMP4 and PS-H.264 fail at
  open. Clips 9–10 are 7–8 growing, so they say nothing about growing
  input as such; a growing CS-MP4 fails too (§7.10).
- VCAP 1 stays black for every clip, so it plays no part in file
  playback.

### 7.6 CPU load during hardware decode

While the DSP decodes a clip from the SD card, the unit's CPU is 48–61 %
idle (`procnto Idle` in `hogs_<k>.txt`). The busiest threads are
`devb-sdc-hbfpga` (SD reads, ~15 %) and `devg-NVMiniRM` (~6 %); io-media
itself is under 1 %. So a stream decoded by the DSP doesn't compete with
the HMI for CPU. For a live stream see §7.10 (cost).

### 7.7 Reading the test results

- **TIME events only show that the graph's clock ran**, not that a picture
  is on screen (§7.2).
- **`0x80002` is a symptom.** `Error 80002: No metadata available` in the
  slog is io-media answering hello6's own `GetTrackInfo` query: the track
  never got opened, so it has no metadata. The failing tracks sit in
  `state=1` (playing ones show `state=4`).
- **Empty play logs are not "no output" results.** `testapp-file` ignores
  SIGTERM, dies by SIGKILL and loses its buffered stdout.
- **`ShowDisplayables` is a setter** (`ShowDisplayables <display>
  [cid...]`), not a query.
- **dev-videoctrl ignores a bad payload with EOK**; only its slog lines
  confirm a devctl (§7.3).
- **io-media logs no `NEDL:` lines** while playing file video (§7.4).

### 7.8 RE: why clips fail, and a container that should stream

Static analysis of the P1001 efs-system (rev 51), extracted whole with
`tools/f3swalk.py`: `sbin/io-media-nvidia` (uncompressed ELF, the clean
copy from §6), `lib/dll/mmedia/mp4_parser.so`, `etc/iom.cfg` and
`etc/NvDspCodecs.elf`. Decompiled with headless Ghidra 12 (IDA has no SH4
decompiler). The SH4 PIC idiom `(code *)(literal + pc)` was resolved to
call targets and GOT-relative strings by a post-processing script.

> **Note:** the `mp4_parser` analysis below turned out not to be the path
> io-media's trackplayer uses for `.mp4`: that is io-media's own
> `CDemuxMp4` (§7.9). The width limit and the CS-MP4 layout still hold.

**Parser choice is by file name.** `iom.cfg` maps URL patterns to parsers
(`strict = yes`):

| pattern | parser |
|---|---|
| `*.mp4 *.m4v *.mov *.mpeg4 *.m4a *.m4b` | `mp4_parser` (efs `.so`) |
| `*.mpg *.mpeg *.mp2` | `parser_mpgvfile` (built in) |
| `*.avi *.xvid *.divx` | `parser_avi` (built in) |
| `*.wmv *.wma *.asf` | `wma9_parser` (efs `.so`) |
| `*.mp3`, `*.aac`, `*.wav` | `mpega_parser`, `aac_parser`, `wav_parser` |

**800 wide (clip 4) is refused on purpose.** io-media-nvidia
`FUN_080d1760` (Ghidra address), before anything reaches the DSP:

```c
if (width > 720 || height > 576)  log "*** Unsupported video resolution", return 0x8000001a;
if (height > 480 && fps > 25.1)   log "*** Unsupported video frame rate", return 0x8000001a;
```

The DSP image agrees: `NvDspCodecs.elf` (and a copy inside io-media) has
`Assertion failed, (width < 720), file NvMixer_Video.c`. **720×576 is the
ceiling.** For a landscape phone that means 720×336 letterboxed, or a
taller crop up to 720×576 (25 fps above 480 lines). The VCAP scales to the
panel (§7.4).

**Fragmented MP4 in `mp4_parser` adds no samples.** `mp4_parser` is
table-driven. One list holds the containers it descends into, including
`moov trak mdia minf stbl mvex moof traf`. A second table maps leaf boxes
to handlers: `mvhd tkhd mdhd hdlr elst stts stsc stco co64 stsz stz2 ctts
stss stsh`, the sample entries `avc1 mp4v mp4a ...` and the iTunes
metadata. It has **no handler for `trun`, `tfhd`, `mfhd` or `trex`**, so
fragments are walked and skipped. With fMP4's empty `moov` sample tables,
`validate_parsed_info` fails (`0x30`) and so does the parse. (Not
re-checked in `CDemuxMp4`; fMP4 fails at open either way.)

**What `mp4_parser` needs** (`validate_parsed_info`, per track): an `stts`
entry, an `stsc` entry, `stco` or `co64`, and `stsz` with a sample count
or a constant sample size, plus the track headers. `stss` (no `stss` = all
frames are sync) and `ctts` are optional. Box handlers:

- `stsz` with a non-zero constant `sample_size` reads **no per-sample
  table** (no malloc). The sample count limit is 16 M.
- `stco` reads `entry_count × 4` bytes, and `stts`/`stsc` read their
  entries (limit 4 M).
- The parser stops at `mdat` once `moov` is in (`parse_mp4file`), so `moov`
  must come first.
- Reads go through `vstream_*`, a buffer over io-media's file streamer
  (`read`/`lseek` on the URL) with **no file-length or offset checks**.

**The container that follows: constant-sample-size MP4 (CS-MP4).**

- **Header:** `ftyp` + `moov`, a few hundred bytes. `stts` is one entry
  (N frames × delta), `stsc` one entry (one chunk holds all N), `stco` one
  entry (the start of `mdat`'s payload), `stsz` = (S, N), and no `stss`.
  N can be hours' worth.
- **Data:** frame k lives at `mdat + k·S`. Each frame (length-prefixed
  NALs) is padded to S bytes with one filler NAL (type 12, `ff…ff 80`),
  which decoders discard.
- **Live:** the header is fixed before any frame exists, so a resource
  manager can serve `/dev/mirror/live.mp4`: header from memory, frame k's
  bytes when frame k has arrived over AOA, with reads blocking until then.
  The phone sends unpadded frames; the MMI pads. A frame larger than
  S − 6 is dropped and a sync frame requested.
- **Cost:** S × fps bytes/s pass through io-media and over PCI to the DSP,
  not over USB. With x264's VBV at 400 kbit the largest 720×336 frame was
  26 KB, so S = 28 KB (5.7 Mbit/s inside the unit). The phone encoder
  needs a frame-size cap (bitrate mode CBR, short VBV, or intra refresh)
  to keep S small.

`media/csmp4.py` writes this layout from an Annex-B stream. ffmpeg decodes
c9 cleanly (550/550 frames), and c10 is reported as 600 s with the first
550 frames present. On the car it plays once `mdat` has a 32-bit header
(§7.9, §7.10).

### 7.9 io-media's own MP4 demuxer: 32-bit box sizes only

A CS-MP4 written with a 64-bit `mdat` header (c9, c10g, live, stall in
hello9) fails ~40 ms after play: an ERROR event "Input/output error", no
TIME event, `iom_errno 0x80002` "No metadata available" for the query, a
black screen. A live server sees io-media open the stream three times and
read **64 KB at offset 0 once**, then nothing more. So the file is refused
while it is being opened, from its first 64 KB, before any frame reaches
the DSP. A plain MP4 (c5) plays through the same client.

**Cause (Ghidra, io-media-nvidia rev 51) [confirmed by simulation on the
clip files]:**

- **Who parses the file.** The trackplayer's `.mp4` input isn't parsed by
  `mp4_parser.so`. io-media has its own demuxers:
  - `CDemuxMp4`: vtable `0x82924a0`, built on an embedded copy of the ISO
    reference MP4 library.
  - `CDemuxRaw` and `CDemuxMpgPs`.
  - Its own extension table: `.mp4`, `.m4v`, `.264`, …

  The query's format string `'MP4 '` comes from io-media too. Where the
  graph picks `CDemuxMp4` over `iom.cfg`'s `mp4_parser` wasn't traced.
- **The open path.** `CDemuxMp4` → `FUN_08129e00` → `FUN_0812a260` →
  `FUN_08139220` (MP4OpenMovieFile, with a 10 MB header budget
  `0xa00000`) → `FUN_08225880` → `FUN_08225700` → **`FUN_08225a20`**,
  which walks the top-level boxes.
- **The walk reads 32-bit box sizes only.** It handles `size == 0` (to
  the end of the file) but has no branch for `size == 1` (the 64-bit
  largesize), and it fails if `pos + size > st_size`.
- **Why a 64-bit `mdat` fails** (c9):
  1. The walk takes `mdat` at 646 as a 1-byte box and moves to 647.
  2. At 647 it reads size 365, type `dat\0`, and moves to 1012, inside
     frame data.
  3. There it reads a 1.9 GB "size", which runs past the end of the file,
     so the walk returns -8.
  4. `FUN_0812a260` turns -8 into `0x80000018`, and the track never opens
     (hence "No metadata").

  The same walk passes c6 (ftyp, moov, free, mdat) and fails c9 and c10
  at 1012. Every read it makes falls inside the first 64 KB.
- **Other limits found in `CDemuxMp4`** [confirmed in code, not all
  paths traced]:
  - Everything except `mdat` is loaded into memory at open, so nothing
    may follow `mdat` in a live stream.
  - Header bytes plus sample tables must fit the 10 MB budget.
  - A constant `stsz` size builds no table. A per-sample table costs 4
    bytes per frame and fails at once above the budget.
  - Nothing requires `stss`, `edts`/`elst` or more than one chunk.
  - The SPS check uses `avcC`.
  - Sample reads use a growing buffer, so there is no per-sample cap.
  - A read error -4 re-checks the file size, which looks like
    growing-file support.

**What the container has to be:**

- `mdat` with an 8-byte (32-bit) header. A file of known length gets its
  exact size. An over-declared or growing file gets size 0 ("to the end of
  the file", which the walk resolves against `st_size`): a box that ends
  past the current file size is refused.
- A live server reports `st_size` = the declared total, so its `mdat`
  ends there.
- A live stream has to stay under 4 GB, about 1.6 h at 28 KB × 25 fps.
  A longer session can reopen.
- hello9's header parser refuses what this walk refuses.
- io-media's first read asks for 64 KB at offset 0, which reaches ~2.3
  frames into `mdat`. Open needs only the ~650-byte header, so answering
  that read short (with what exists) instead of waiting would save about 3
  frames of start-up. How io-media takes a short read is untested.

### 7.10 Live H.264 streaming through io-media

hello9's tests, played through its own io-media client on VCAP 0 input 1
only. Details per test: [src/hello9/README.md](../src/hello9/README.md),
"On the car".

| test | result |
|---|---|
| c5 (plain MP4) | Seen |
| **c9 (CS-MP4 file)** | **Seen**, 720×336. First TIME 567 ms (c5: 369) |
| c10g (CS-MP4 whose `moov` declares 10 min, growing on the card, 2 s head start) | Header opens ("0 of 600000 ms"), then EIO at 813 ms: io-media read past the written end. Black |
| **live (our resource manager, 30 s, frame k released at k/25 s)** | **Seen, 30 s, no errors.** 341 sequential 64 KB reads, all waited (avg 89, max 140 ms), read-ahead ≤ 107 ms. Lag 514–560 ms, steady |
| **stall (pauses 2 s at 8 s, 5 s at 16 s)** | **Seen, no errors, no unblocked reads.** Both pauses waited out; picture moved again after each |

**What this settles [confirmed on the car]:**

- **Option 2a works without touching io-media.** The container is CS-MP4
  as built by `media/csmp4.py`: `ftyp`, `moov`, then a 32-bit `mdat`
  (size 0 for the live stream, with `st_size` = the declared total); one
  constant `stsz` size; every frame padded to S bytes with a filler NAL;
  `moov` declaring far more frames than exist. The DSP decodes the padded
  frames, and `CDemuxMp4` takes the over-declared header.
- **io-media reads like a simple sequential reader.** At start it opens
  the path three times (two probes, then the real one). After that it
  only reads 64 KB at a time, in order, with no `pread`s and no seeks, and
  never more than ~2.7 frames past what has been released. A read can
  wait at least 5 s without io-media giving up.
- **Latency added by io-media: ~0.5 s, steady.** Two measures agree:
  - TIME positions trail the frame release by 514–560 ms over the whole
    30 s, with no drift.
  - When starved, the picture stops ~9 frames (~0.35 s) behind the
    newest frame io-media has read (frozen on 190 with 198 served, on 391
    with 399 served). The rest is decode and display.
- **A pause in the stream freezes the picture, then playback skips and
  goes on.** The freeze lasts as long as the pause (2.0 s, 5.2 s). When
  frames come back, the picture **jumps ~10 frames** (190 → 202,
  391 → 401) and runs at 25 fps again. So gaps don't add latency, but the
  ~9 frames queued when the stream ran dry are never shown. io-media's
  clock runs on through a pause, so its TIME positions are no use for
  measuring after one.
- **Cost:** the unit's CPU (the MMI's own slog `CPU:` line) is 20–23 %
  during `live` against 12–13 % idle. Option 0 (JPEG) uses ~27 % at 3 fps.
- **Growing files are out:** io-media reads ahead of the writer and gets
  EIO. Only a resource manager can make reads wait.

**Consequences for the design:**

1. **The stream must never go quiet.** If the phone stops sending when its
   screen is still, the last ~0.35 s of a change stays hidden until the
   next frame, and then it is skipped. The sender has to keep a steady
   frame rate: Android's `KEY_REPEAT_PREVIOUS_FRAME_AFTER` on the encoder,
   or the MMI inserting skip P-frames (all macroblocks skipped, a few
   bytes plus filler) whenever no frame has come for 1/25 s (§7.11).
2. **Frame size S is a hard cap.** Every frame, keyframes included, must
   fit S. The test clip used S = 28 KB at 1.5 Mbit/s. The encoder needs a
   strict per-frame cap (CBR, small VBV buffer; possibly intra refresh
   instead of IDR frames), and an oversized frame has to be dropped and a
   sync frame asked for. Bandwidth inside the unit is S × 25 per second,
   whatever the content (28 KB → 5.7 Mbit/s); the phone sends unpadded
   frames.
3. **The header comes from the phone.** The `avcC` in `moov` must carry
   the encoder's own SPS/PPS, and the size is fixed for the session. The
   phone sends its codec config first; the MMI builds the header
   (`src/common/csmp4`, §7.11) and only then starts the graph. A rotation
   or size change means a new header and a graph restart.
4. **Picture size:** at most 720 wide and 576 high, H.264 Baseline ≤ L3.
   VCAP scales to the displayable, so 720×432 (5:3, = 800×480) fills the
   screen.
5. **Session length:** `mdat` must stay under 4 GB: ~1.6 h at
   28 KB × 25 fps, ~2.9 h at 16 KB. A longer session needs a graph
   restart (a short black gap) or a smaller S.

**Open questions:**

- **Latency tuning:** does the ~9-frame queue shrink with a smaller S (is
  it counted in bytes or frames)? With a higher declared rate (30 fps)?
  hello9 `s56`, `s112` and `f30` test it (§7.12), not yet run on the car.
- **Robustness:** a session of 10+ minutes; the HMI starting its own
  media or video (MMI3GMedia owns the video lock); reverse gear (the
  camera is VCAP 0 input 0); unplugging mid-stream; a graph restart.

### 7.11 The MMI side: shared stream code and gap fill

The receiver's use of this code played the phone's stream on the car
(§7.13). Gap fill and hello9's `gap`, `slow` and `burst` tests are built
and checked on the build host, **not yet confirmed on the car**.

- **`src/common`, C++ (Makefile group `LIVE`):**
  - `h264`: Annex-B → NAL units and frames; SPS / PPS / slice header
    fields; **skip frames**: a non-reference P slice with every macroblock
    skipped (`mb_skip_run` = all), `frame_num` = the last reference + 1,
    so the encoder's own frames after it stay valid unchanged.
  - `csmp4`: the CS-MP4 header built on the MMI (a port of `csmp4.py`;
    `make test` checks it byte for byte against `csmp4.py`'s for the same
    clip), frame padding, the parser.
  - `live_stream`: the stream in memory (the header, a ring of the last
    128 frames), push (drops everything up to the first IDR, a too-big
    frame and, if it was a reference, the frames up to the next IDR;
    in-band SPS/PPS that differ from the config are refused), and **gap
    fill**: when the stream is 2+ frames behind its frame clock and
    nothing came for a frame period, skips are appended, up to one frame
    short of the clock, never past it. That answers consequence 1 of
    §7.10: a pause shows the last real frame and doesn't skip afterwards,
    and a sender slower than the stream's rate is made up to it.
  - `stream_server` (serving a `live_stream`), `player`, `vout`, and
    `hw_stream`, which ties them together.
- **On the host** (`make test`, `tests/check_live.py`): ffmpeg decodes a
  stream with a 600 ms gap filled by 14 skips without errors, as raw
  H.264 and as the MMI-built CS-MP4, and every skip is an exact copy of
  the frame before. The same simulated on the card's clip (`gap`
  schedule): frozen on frame 199 for 49 frames and on 399 for 125.
- **POC type:** x264 Baseline uses POC type 2, where two non-reference
  frames in a row are outside the spec. ffmpeg doesn't mind; whether the
  DSP does is what hello9's `gap` test answers. If it minds, the skips
  become reference frames and the following frames' `frame_num` is
  rewritten. The phone's encoder uses POC type 0 (§7.13), where skip
  frames carry their own POC LSB.
- **hello9** pushes a raw clip (c11) through `HwStream` the way the
  receiver pushes the phone's frames: `live`, `gap` (fill on), `slow`
  (15 fps), `burst` (5 frames every 200 ms), with `c5` as the control and
  `c9` / `stall` on request (src/hello9/README.md).
- **`audi_stream_receiver`** has the MMI side of the protocol and runs
  the stream on its own thread (`hwvideo.cpp`,
  src/audi_stream_receiver/README.md). It is a C++ app.
- **Protocol** (`proto.h` / `Proto.kt`): `H264_CONFIG` 0x0B (codec
  config: SPS/PPS, size, S), format 3 in `VIDEO_START`, `H264_STATUS`
  0x0C, H.264 limits in `VIDEO_CAPS`.

### 7.12 The phone side, latency and tuning tools

- **Phone (Audi streaming app, Format "H.264", Android 10+):**
  `HwEncoder.kt` is MediaCodec with a Surface input at 720×432
  (`H264.pictureSize`: as wide as the MMI allows, the screen's 5:3).
  Baseline 3.0, CBR where offered, an IDR every 10 s at most (they are the
  big frames; the MMI asks for one when it needs it). The **Max fps
  slider** (1–25) sets the encoder's rate: `KEY_MAX_FPS_TO_ENCODER` caps
  it and `KEY_REPEAT_PREVIOUS_FRAME_AFTER` (1/fps) keeps it up on a still
  screen. The `H264_CONFIG` always says 25 fps, the MMI's clock: timing
  the stream at the slider's rate would make io-media's ~9-frame queue
  cost 9/fps seconds (1.8 s at 5 fps), while at 25 the MMI's skip frames
  make up the difference (hello9 `slow`). The capture path into the
  encoder is in §7.13.
- **Frames are never lost silently** (the ones after would be garbage):
  they queue on the phone while acks are outstanding (up to 12 in
  flight); 25 queued = something is stuck, so the queue is dropped and an
  IDR asked for. A frame bigger than `frame_bytes` (64 KB at first) raises
  it (×2 or ×1.5 the frame, 16 KB steps, at most the MMI's 256 KB) with a
  new config - the MMI restarts its stream, ~1-2 s black, once. In-band
  SPS/PPS that differ from the codec config's become the config.
- **MMI** (`hwvideo.cpp`): keeps 8 MB of frames (16-128). Because the
  rear camera shares VCAP 0 and an app can't tell whether it is on
  (docs/rear-camera.md §9, §11), `/dev/videoctrl` is touched only when
  the user starts or stops a stream: restarts (new config, stream full)
  keep the picture and swap only the graph; an io-media ERROR ends the
  stream without touching VCAP and sends the phone back to pictures
  (`REFUSED`). Starting or stopping H.264 while reversing still hides the
  camera until its next session.
- **Desk:** `make aoa-host` speaks H.264 like the MMI (limits, config,
  the same frame checks and statuses) and writes
  `build/aoa-host/stream.h264`.
- **Latency clock:** a switch in the phone app shows seconds.milliseconds,
  redrawn every display frame. Stream with it on screen, photograph the
  phone next to the MMI: phone clock − MMI clock = the whole delay
  (capture, encoder, USB, MMI, io-media, DSP, display). Several photos;
  the camera's own exposure blurs the last digit.
- **Tuning (hello9):** `s56` / `s112` (frames padded 2× / 4×) and `f30`
  (stream at 30 fps) against `live`, from the `lag` in their RESULT lines.
  Lag the same with bigger frames = the queue is frames (bigger frames
  only cost io-media reading); lower = bytes. `f30` lower by ~1/6 =
  frames, and a 30 fps stream would cut ~60 ms. Built, not yet run on
  the car.

**Open:** whether repeat-previous-frame really keeps a still screen at
the slider's rate on this phone, and the robustness list (§7.10).

### 7.13 Phone H.264 on the car; text sharpness

**On the car [confirmed]:** the phone's H.264 plays on the MMI: 720×432
Baseline, POC type 0, lag 600–650 ms steady, MMI CPU ~24 % (receiver 4 %,
against ~45 % for JPEG). IDRs go over 64 KB, so the phone raises the frame
size to 128 KB (a new `H264_CONFIG`; the MMI restarts, and a second
identical config queued during the first restart causes a second restart
~300 ms later).

**Direct capture into the encoder blurs text.** With the virtual display
drawing straight into the encoder at 720×432, Android scales the 2340×1080
screen 3.25:1 in one bilinear step (2×2 taps per output pixel out of ~3×3
covered: thin strokes are hit or missed). That is the same blur the
pictures' k× capture fixes (option 0). Smaller parts: the letterbox (the
screen is 720×332 of the 720×432), 4:2:0 chroma, 2.5 Mbit/s CBR (~12 KB a
frame at 25 fps) and the VCAP's ×1.11 to 800×480.

**k× capture + GPU box filter (built, not yet confirmed on the car):**

- `GlScaler.kt` / `GlBox.kt`: the virtual display renders at k× the
  picture into a SurfaceTexture (k = the largest ≤ 4 with k× the mirrored
  screen within the screen: ×3 = 2160×1296 landscape, Android's step then
  2340 → 2160; ×4 portrait). A GLES2 pass box-filters it k:1 into the
  encoder's input surface (EGL_RECORDABLE_ANDROID, the capture's
  timestamps): even k with (k/2)² bilinear taps at 2×2 corners, odd k
  with k² nearest taps, highp coordinates. The encoder's rate limit and
  repeat-previous-frame are unchanged. GL setup failing (or drawing
  failing later) falls back to the direct path for the rest of the
  session. Shaders checked with glslang; `GlBoxTest` covers the taps.
- Switch **Sharp H.264** (default on) for an A/B; drop-down **H.264
  rate** 2.5 / 4 (default) / 6 / 8 Mbit/s. Either change restarts the
  encoder like the fps slider. The status line shows `(x3)` or `(direct)`
  and the rate.

**Open questions:**

- Small-text sharpness with Sharp on/off at 4 and 6 Mbit/s, Max fps 10
  and 25.
- IDR sizes at the higher rates (frames over 256 KB can't be sent at all)
  and the MMI's `h264:` lag at those rates.
- Whether the video displayable can be shown 1:1 (720×432 centred, no
  VCAP resample) instead of scaled to 800×480.
