# Phone screen mirroring on the MMI 3G+: options and feasibility

How to show an Android phone's screen (Redmi Note 8 Pro, 1080×2340) on this
unit's 800×480 display. This doc assumes `usb_tx_test` works, i.e. we have a
working Android Open Accessory (AOA) bulk link between the phone and our own
SH4 app. The phone is used in **landscape only**.

Every claim is labelled:

- **[confirmed]**: read from the P1001 rev 51 binaries (Ghidra or disassembly,
  with an address where there is one) or measured on the car.
- **[inferred]**: follows from confirmed facts but hasn't been exercised.
- **[unverified]**: an estimate or assumption. The test plan below checks it.

Option 6 from the first draft (an HDMI→composite converter into the ADV7180
camera input) is rejected and not covered here.

---

## 1. Summary

| # | Option | Feasibility | Effort | Main risk |
|---|---|---|---|---|
| 1 | Raw pixels (dirty tiles + LZ4) over AOA, drawn with GL | **High** | ~1–2 weeks | Throughput: VFB writes from the CPU are slow, GL upload speed is unmeasured |
| 1b | Same, but DXT1 tiles uploaded as compressed textures | **High** | +3–5 days on top of 1 | Text quality (block artifacts) |
| 2a | Hardware H.264 (DM642 DSP), stream fed to `io-media-nvidia` | **Medium** | ~2–4 weeks | Does io-media accept a live, growing input? Latency 0.5–2 s? |
| 2b | Hardware H.264, talking to the DSP driver directly | **Low** | months | Private NVIDIA protocol (`/dev/nvdsprm0`, `/dev/nvminirm0`, `NVshm-*`) |
| 3 | Software H.264 on the SH4 | **Rejected** | — | ~5–10 fps at best [unverified estimate] |
| 4 | scrcpy over adb (MMI acts as adb host) | **Medium** | ~2–3 weeks + 2a | Needs USB debugging always on. Video still needs 2a or 2b. |
| 5 | Android Auto receiver | **Low–medium** | months | TLS, protobuf, head-unit cert, audio. Video still needs 2a or 2b. |

**Recommendation:**
1. Run the two cheap car tests in §7 first: the decoder test and the
   throughput benchmarks. They settle the biggest unknowns in one SD-card run.
2. Build **option 1** (with AOA v2 HID input). It is the shortest path to a
   working mirror.
3. Move to **2a** only if test T1 shows the DSP plays our streams. Options 4
   and 5 build on 2a and aren't worth starting before it works.

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
    second. So 800×480 (1500 MB) can run at up to 27 fps, and 720×336 at
    30 fps.
- **Width.** Both the host (`vmixer.c`) and the DSP (`NvMixer_Video.c`)
  contain `assert(width < 720)`. Which path it guards (perhaps the DVD
  sub-picture mixer) is **[unverified]**. Assume an encoded width of ≤ 704 or
  720 until T1 shows 800 works.
- **Containers.** `iom.cfg` maps file extensions to parsers:

  | Extensions | Parser |
  |---|---|
  | mp4 / m4v / mov | `mp4_parser` |
  | avi / divx / xvid | `parser_avi` |
  | mpg / mpeg | `parser_mpgvfile` |
  | wmv / asf | `wma9_parser` |

  - io-media also has an MPEG-PS stream parser that resyncs on start codes
    (`CNvMpegPsParser`, "Failed resync!").
  - `mp4_parser.so` contains a full ISO-BMFF box-name table including the
    fragment boxes (`moof`, `traf`, `trun`, `tfhd`, `mvex`). The same table
    also lists image boxes (`iloc`, `pitm`), so it's a generic list. **Whether
    fragmented MP4 actually plays is [unverified].**
  - The only URL scheme seen is `file://`.

### 2.2 How decoded video reaches the screen [confirmed, except which VCAP]

```
DM642 (H.264 decode) ──video port──> NV VCAP 0 or 1 ──> video layer ──> display
                                              ▲
            layermanagerV2: video displayable bound to a VCAP (gf_vcap_bind_to_layer)
```

- **Wiring.** `/etc/videoctrl-Audi3G.cfg`: VCAP 0 has two inputs, the
  ADV7180 (composite camera and RSE inputs, input 0) and the DSP's video
  port (input 1). VCAP 1 is fed by "Partial Decode". So the DM642 output
  may arrive on **VCAP 0** (its own port), while "partial decode" on VCAP 1
  may be the GPU's MPEG-2 path (devg-NVMiniRM's `mpCreateDecoderObj`).
  **Which unit carries H.264 is [unverified];** hello6 tries both.
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

  So **registering a displayable with flags `0x101` (VCAP 0) or `0x201`
  (VCAP 1) and showing it** should bring the decoder's output on screen
  through the same `/dev/layermanager` devctls that `lmgr.c` already uses
  **[inferred]**.
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
    `iom_attach_output_to_graph: iom_errno=0x10001`, even with no URL, 12.10
    v2. The unit's MME sends media to io-media **mixer inputs** instead. From
    the `outputdevices` table in efs-system's mme SQL:
    `mixer:DSPOUT_TDM1_51_SAD?delay=130` (`defaultoutput`),
    `mixer:DSPOUT_TDM2_20?delay=130`, `mixer:DSPOUT_TDM3_20?delay=130`, and
    `a2dp://dev/wms/headphone1|2?...` for BT. MMI3GApplication owns the
    `/dev/io-media/graphs/mixer/<pid>_0..2` graphs.)
  - `-s` play speed, `-n` notification interval
  - It enqueues URLs and logs io-media events (`INPUT_EOF`, `FINISHED`,
    `STATUS`, `TIME`).
- **`testapp-cmdline`:** an interactive client.
  - Graph control: `iom_attach_input`, `iom_attach_output`, `iom_play`,
    `iom_stop`
  - Queries: **`iom_get_video_info` (returns w/h)**, `iom_get_status`,
    `iom_get_error_info`

These let us test the decoder without writing any io-media client code.

### 2.4 Graphics paths for raw pixels

- **CPU writes to the VFB are slow [confirmed, roughly].** In the second car
  run, lmprobe's `fill_bars` (one 32-bit store per pixel, 800×480) took about
  150 ms between the "VFB:" and "filled colour bars" log lines. That's about
  10 MB/s. The per-pixel divide in the loop accounts for only a fraction of
  it. This looks like uncached, word-at-a-time writes across PCI into GPU
  memory **[inferred]**. A full-screen CPU blit would therefore cap at about
  6 fps.
- **GL path [confirmed exports].** `libGLES_CM.so.6` exports:
  - `glTexSubImage2D`
  - `glCompressedTexImage2D` / `glCompressedTexSubImage2D`
  - `glDrawTexiOES` (`GL_OES_draw_texture`: a screen-aligned blit with no
    vertex setup)

  The extensions include `GL_EXT_texture_compression_s3tc` and
  `GL_EXT_texture_compression_dxt1`. Upload goes through the NV driver, which
  can use DMA (`devg-NVMiniRM -d 11` is the DMA channel). **Upload throughput
  is unmeasured.** hello3 uploaded 320 KB of textures in 60 ms, but that time
  also includes generating the textures, so it says little.

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
| H.264 (2a) | VirtualDisplay → `MediaCodec` encoder surface. `KEY_PROFILE=AVCProfileBaseline`, `KEY_LEVEL=AVCLevel3`, 25–30 fps, 2–4 Mbit/s, I-frame every 1–2 s, `KEY_LATENCY=0` where supported. | High, but check `MediaCodecList` for a hardware Baseline encoder at the chosen size **[unverified on this phone]**. |
| Transport | Existing AOA bulk link (`usb_tx_test` protocol, with new frame types) | Confirmed working (assumed for this doc) |
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
| Fill (crop sides) | centre 1800×1080 | 800×480 | 800×480 if T1 allows it, else 704×416 |

Letterbox is the safe default: nothing is cut off, and 720×336 is far inside
Level 3. A MediaProjection VirtualDisplay of a different aspect ratio
letterboxes by itself.

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
| Confirmed | AOA link (assumed), EGL/GL bring-up (hello2–4), `glTexSubImage2D` / `glDrawTex*OES` exported, 60 Hz vsync, knob input |
| Unverified | GL upload MB/s, USB bulk MB/s, LZ4 decompression speed on the SH4 (expect ≥ 100 MB/s, no FPU involved) |
| Pros | Sharpest image. Lowest latency (one frame of encoding). Almost no new unknowns. The phone app is plain Kotlin. |
| Cons | Busy content (scrolling, video, map panning) sends whole frames: 800×369×2 = 590 KB before LZ4. At 10 MB/s of upload or USB that's about 15 fps. |
| Effort | MMI: ~1 week (GL path from hello3, protocol from usb_tx_test). Phone: ~1 week (MediaProjection, tiles, LZ4 via `lz4-java`, HID). |

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

1. runs a QNX resource manager at e.g. `/dev/rivit/live.mp4` (or `.mpg`)
   that serves the incoming stream wrapped in a streamable container: a
   fragmented MP4, or MPEG-PS carrying H.264 (stream id 0xE0). Reads block
   until new data arrives.
2. tells io-media to play `file:///dev/rivit/live.mp4` (the same calls
   `testapp-cmdline` makes: attach input, attach output, play).
3. registers and shows a **video displayable with flags 0x101 or 0x201**
   (VCAP 0 or 1, whichever hello6 shows to work) through `lmgr.c`.

| | |
|---|---|
| Confirmed | The DSP H.264 BP decoder exists. io-media's profile/level checks. The DSP → VCAP → layer path (which VCAP: hello6). The `LMGR_VIDEO` / VCAP flag bits in layermanagerV2. The test clients. `file://` input. |
| Unverified | Whether video files play at all outside the HMI. The width limit (800 vs 720). Whether any parser tolerates a growing, unseekable input: fMP4 support is unconfirmed, and mp4_parser calls `vstream_length` and seeks. Prefetch latency (`queue-pre-hw = 50`, `queue-time = 1000` ms for the mediafs graph). Coexistence with MMI3GMedia, which owns io-media and the global video lock. |
| Pros | 2–4 Mbit/s whatever the content. 25–30 fps. The SH4 is idle. Smooth for video and map panning. |
| Cons | Latency probably 0.5–2 s from io-media buffering, so it's poor for interactive use. Fragile next to the HMI's own media playback. A DSP crash path exists ("dsp crash, remaining retries"). |
| Effort | 2–4 weeks: resource manager, fMP4 or PS muxer (on the phone or the MMI), iom client, video displayable. Plus 1 week of RE if the stream won't play as-is. |

**First reverse-engineering step if T1 passes but streaming fails:** re-carve
`io-media-nvidia` cleanly (the current copy's data segment is shifted by
`0x1dc8` bytes, see §6), then trace `vstream_read` /
`vstream_bytes_available` in `mp4_parser.so` and the `file://` reader's EOF
handling.

### Option 2b: direct DSP access

Reimplement the nMSDK client: the `/dev/nvdsprm0` channel API, the
`/dev/nvminirm0` resource manager, `NVshm-*` buffers, and the H.264
bitstream-buffer API (`BufAPI(H264)`). This gives the lowest-latency hardware
path, but it means reverse engineering a large C++ binary that talks a
private protocol to a C64x DSP with no symbols. **Feasibility: low.** Only
consider it if 2a works but its latency is unacceptable.

### Option 3: software H.264 on the SH4: rejected

There's no SIMD, the CPU is 792 MHz, and the GPU has no shaders for YUV→RGB.
Register combiners could do the colour conversion, but the decode alone is
estimated at 5–10 fps at 720×336 **[unverified estimate]**. Options 1 and 2a
dominate it.

### Option 4: scrcpy over adb

**Design.**
1. The MMI implements the adb host protocol (CNXN / AUTH / OPEN / WRTE /
   OKAY) on the phone's adb interface (`ff/42/01`; `usb_tx_test` currently
   skips it).
2. It pushes the official `scrcpy-server` (Apache-2.0) and starts it with
   `app_process`.
3. It reads the H.264 stream (Baseline/Level 3 via the codec options,
   `max-size 800`, `max-fps 25`, orientation locked) and injects input through
   scrcpy's control socket.

| | |
|---|---|
| Confirmed | Our USB stack handles arbitrary interfaces. The adb interface is visible to us. |
| Unverified | Everything on the MMI side. RSA-2048 signing for AUTH needs a small bignum or BearSSL; it runs once per connection, so SH4 speed is fine. |
| Pros | No custom phone app and no consent dialog. Full touch-level input injection. Can create a separate 800×480 virtual display, so the phone screen stays free. Audio capture on Android 11+. |
| Cons | **USB debugging must stay enabled** (a security cost in a car). Replaces the AOA work. Video still needs 2a/2b, or a raw mode instead of H.264. |
| Effort | 2–3 weeks for adb plus scrcpy, then whatever 2a costs |

### Option 5: Android Auto receiver

**Design.** AOA with the strings "Android" / "Android Auto". The MMI is the
TLS client (we'd need a head-unit certificate taken from a commercial unit,
which is legally grey). Then protobuf channels for video, input (with
**rotary-controller** support, which maps directly onto the MMI knob),
sensors, and audio.

| | |
|---|---|
| Confirmed | AOA works. **No TLS library on the unit** (searched ifs and efs), so BearSSL or mbedTLS gets compiled in; both are plain C. C++ stacks (aasdk/OpenAuto) can't be used: the unit's libstdc++ is GCC 3.3-era. |
| Unverified | The AA video level at 800×480 may be signalled above 3, which io-media rejects; we'd patch `level_idc` in the SPS. Media and guidance audio need QSA to work (hello5 isn't there yet). |
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

## 6. Gaps in what was checked

- **Damaged io-media binary.** The carved `io-media-nvidia` (from the
  earlier efs extraction) has a corrupt section table, and its data segment
  sits 0x1dc8 bytes past where its program headers say. Ghidra loads the code
  correctly, but the GOT, the dynamic table and string cross-references are
  wrong. That's why the H.264-level check and the `width < 720` assert are
  backed by strings only, not traced call sites. Re-carve it before any
  serious RE of 2a or 2b.
- **Output devices.** MME fills its `outputdevices` table at runtime, so the
  audio output name to use with `iom_attach_output` comes from the car
  (`mmecli` or `testapp-cmdline`). The default in `testapp-file` is
  `/dev/snd/pcmPreferredp`.

---

## 7. Test plan: hello6 (one SD run)

`src/hello6` runs T1–T3 in one go, about 5 minutes, parked:
`make sd SD_APPS=hello6`. Text screens explain each step; the driver turns
the knob once whenever a moving test picture is visible. Results:
`hello6.log` (`RESULT` lines and a summary) and `hello6/` on the card.

**T1: hardware decoder.** Ten clips from `media/mkclips.sh` (8 s each,
moving `testsrc2` pattern with the clip's name and a frame counter):

| # | Clip | Question |
|---|---|---|
| 1 | MPEG-2 720×576 in MPEG-PS | Control: the DVD player's format |
| 2 | MPEG-4 Part 2 640×480 in AVI | Control: another DSP decoder |
| 3 | H.264 BP L3 720×480 MP4 | H.264 inside every known limit |
| 4 | H.264 BP L3 800×480 MP4 | Does `width < 720` bite? |
| 5 | H.264 BP L3 720×336 MP4 | The letterbox size |
| 6 | Same, no audio track | Does the player need audio? |
| 7 | Same, fragmented MP4 | Streamable MP4 layout |
| 8 | H.264 + MP2 in MPEG-PS | The other streamable container |

Each plays through the unit's own `testapp-file` (graph `trackplayer`,
`-n 500` for progress events every 500 ms, `-o mixer:DSPOUT_…`: the mixer
outputs from §2.3 are tried in turn until one attaches). If it exits at once
for another reason, it is retried with a `file://` URL. While it plays, hello6 shows
only a video displayable (cid 41): VCAP 0 (`0x101`) for the first half,
VCAP 1 (`0x201`) for the second. Per clip it records:
- io-media `TIME` events and the furthest position (playback progress)
- knob events per half (what the driver saw)
- register/show return codes
- per-thread CPU load (`hbhogs`) while decoding. Not `ShowDisplayables`:
  it is a setter (`ShowDisplayables <display> [cid...]`), not a query.

io-media's own slog lines go to `var/hello6_slog.txt`.

**T2: growing input.** Clips 9 and 10 are clips 7 and 8 written to the
card at real-time rate while they play, after a 2 s head start. If they
play to the end, io-media reads past the end of a file that is still
growing, and the resource-manager design in option 2a is very likely to
work.

**T3: throughput.** Before the videos (`bench.c`):
- (a) CPU: memcpy, RGB565→BGRA conversion, LZ4 compress/decompress on
  UI-like, photo-like and random frames (`lz4mini.c`, verified against
  liblz4 by `make test`)
- (b) VFB: 32-bit stores, memcpy in, memcpy out
- (c) GL: `glTexSubImage2D` RGB565 / RGBA / BGRA and
  `glCompressedTexSubImage2D` DXT1 MB/s
- (d) GL: visible upload + draw + swap fps, for full RGB565 frames, a
  moving 120-line RGB565 band (tile-diff-like) and full DXT1 frames

Still missing: AOA bulk receive throughput. That needs the phone, so it
belongs in `usb_tx_test`.

**How the results decide:**
- If clips 1 and 2 are never seen, the displayable setup is wrong, not the
  decoder. Check `play_*.log` (TIME events) and the slog before drawing any
  H.264 conclusion.
- If the controls are seen but H.264 clips 3–5 aren't (no progress, or an
  io-media error), drop 2a/4/5 and do 1/1b.
- If 3–5 play but 9–10 don't, do 1 first, then 2a with RE of io-media's
  reader.
- If the GL upload or the AOA throughput is under about 8 MB/s, start with
  1b instead of 1.

### 7.1 12.10 v2 results: no clip played; the output was the problem

All 10 clips: `player exited early (exit 1)`, both URL forms, `0 TIME
events`, and `play_<k>.log` = `iom_attach_output_to_graph: iom_errno=0x10001`.
The sysinfo smoke test (`testapp-file` with no URL) fails the same way, so it
is the **default output** `/dev/snd/pcmPreferredp` (→ `pcmC0D0p`, `ent3`)
that io-media refuses, not the clips, the containers or the decoder. No
decoder question was answered. Fix: `-o mixer:DSPOUT_*` (§2.3).

What the video displayable showed: registering cid 41 with `0x101` or
`0x201` and showing `{41}` alone gave a **black screen** for both VCAPs on
every clip (the phone video, 1:30–1:36 etc.). That is consistent with an
empty capture, since nothing was decoding. It says nothing yet about which
VCAP carries the DSP.

The T3 numbers from this run are in [`gpu-results.md`](gpu-results.md) §7:
800×480 RGBA upload **31 ms ≈ 50 MB/s** with `glFinish`, CPU RGB565→BGRA
11.9 ms/frame, LZ4 UI-frame decompress 8.8 ms. Back-to-back uploads froze the
unit, so a client keeps one upload in flight.

