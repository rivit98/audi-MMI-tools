# Rear-view camera: how the MMI 3G+ puts it on screen

How the stock unit (P1001, board rev 51) shows the rear-view camera (RVC):
from the camera ECU's request, through the HMI, `dev-videoctrl`, the ADV7180
decoder and the nVidia capture unit, to the layer the driver sees. It ends
with what this means for our own apps, which share the same capture unit.

Every claim is labelled:

- **[confirmed]**: read from the P1001 rev 51 binaries (address given) or
  seen in a car-run slog (run given).
- **[inferred]**: follows from confirmed facts, not exercised.
- **[unverified]** / **[unknown]**: an assumption, or not found yet.

Ghidra addresses are for `MMI3GApplication` unless another binary is named.

---

## 1. Summary

```
 reverse gear (car)                                   800×480 panel
      │                                                    ▲
 camera ECU (BAP LSG 0x0B)                                 │
      │ DP_PRESENTATION fct 0x17 = "show me"     gf layer 7: HMI surface (ARGB, per-pixel alpha)
      ▼                                          gf layer 6: video, VCAP 0 scaled to 800×480
 MMI3GApplication ── grants, acks VIS_ON ──► ECU           ▲
      │ display context RVC = [HMI 0x10, RVC 0x11]         │ layermanagerV2: displayable 0x11
      │ requestVideoFocus(src RVCAMERA, snk MAIN)          │ (LMGR_VIDEO, 800×480)
      ▼                                                    │
 /dev/videoctrl ──► ADV7180 input 1 (camera A, NTSC) ──► VCAP 0 input 0
                    I2C via GPU          crop 708×480, WEAVE ┘
```

- **The MMI never sees reverse gear.** The camera ECU decides when the
  picture is wanted and asks over BAP. The MMI grants the request, switches
  the display context and acknowledges VIS_ON [confirmed, §3].
- **The picture is analog.** NTSC composite goes into the ADV7180 on its
  input 1 ("camera A"), and from there into nVidia capture unit VCAP 0,
  input 0 [confirmed, §2].
- **The VCAP scaler does all the work.** The HMI crops the 720×480 input
  to 708×480, weaves the two fields, and registers an 800×480 video
  displayable. The picture is therefore **stretched to full screen**: ×1.13
  horizontally and 1:1 vertically, with no black bars and no aspect
  correction [confirmed, §5].
- **The MMI draws nothing on the video natively.** The guide lines are
  almost certainly in the analog picture from the camera ECU [inferred].
  The HMI's own surface sits one layer above the video and can blend
  parking-sensor bars and buttons over it [§6].
- **No per-frame CPU work.** Once the capture is enabled, frames go from
  VCAP to the layer in hardware. The CPU only acts on camera on and off
  [confirmed for layermanagerV2 and dev-videoctrl].

---

## 2. Hardware path

`/etc/videoctrl-Audi3G.cfg` (ifs-root) describes the wiring [confirmed]:

```
                             0 |--------|      |--------|
     FBAS from RSE     --------|  7180  |    0 | VCAP 0 |
                             1 |  VP #0 |------| VP #1  |
     FBAS camera A     --------|        |      |        |-------- to display
                             2 |        |      |        |
     FBAS camera B     --------|        |    1 |        |
     DSP642     -------------------------------|        |
                                               |--------|
     Partial Decode -------------------------- VCAP 1 (VP #2) --- to display
```

The cfg declares three "VPs" (video processors). dev-videoctrl addresses
them by index:

| VP | Device | Inputs |
|---|---|---|
| 0 | ADV7180 composite decoder, I2C 0x40 | 0 = RSE, **1 = camera A**, 2 = camera B (reg 0x00 INSEL = AIN1/2/3) |
| 1 | nVidia VCAP 0 | **0 = ADV7180**, 1 = DM642 DSP video port |
| 2 | nVidia VCAP 1 | 0 = "Partial Decode" |

- **Which I2C bus.** The ADV7180 hangs off the **GPU's I2C bus**.
  dev-videoctrl talks to it through `gf_i2c_write` / `gf_i2c_writeread`
  (bus 0, address 0x40). Audi3G sets no `deviceNameI2C`, so the FPGA I2C
  multiplexer path is never used [confirmed: dev-videoctrl `FUN_08042040`,
  `FUN_08042180`].
- **Init does not show the camera.** At start dev-videoctrl:
  1. checks the chip ID (reg 0x11 = 0x1B/0x1C);
  2. soft-resets the chip;
  3. writes `VPInit` (0x07=0x23: autodetect limited to PAL-BGHID, NTSC-M/J and NTSC-4.43; 0x0C/0x0D: free-run picture black instead of blue);
  4. sets VCAP 0 to input 0 (`VPConnect=0x01:0x00`).

  It does **not** select an ADV7180 input and does not enable any VCAP
  [confirmed: dev-videoctrl `main` 0x08047060].
- **The car's camera is NTSC 720×480.** Our camera ECU delivers NTSC. On
  every camera event the ADV7180 reports reg 0x10 = 0x07 (in lock, FSC
  lock, autodetect result NTSC-M/J) [confirmed: all car-run slogs].
  Whether the cfg's camera B input is wired on this car is [unknown].

---

## 3. What triggers the camera

### 3.1 The request comes from the camera ECU over BAP

The HMI has no reverse-gear input or selector-lever strings at all. The
camera ECU (BAP LSG 0x0B, "RVC") drives the session through function 0x17
`DP_PRESENTATION`. The MMI answers on the same function [confirmed]:

```
camera ECU                          MMI3GApplication (3GPLUS → CarParkingSystemExt path)
 │ LSG 0x0B fct 0x17 [FF,01] START     processStatusRearview FUN_083b60e0
 │──────────────────────────────────►  decode FUN_083b2cdc: (FF,01) START, (01,01) ACTIVE,
 │                                       (02,01) QUEUED, (xx,00) END, (00,00) OFF
 │                                     gates: isDPPresStdCaseHandled FUN_083b4bbc  (§8)
 │                                     setCurrentHMIPopup(7) FUN_083b4af4
 │                                     → responseRequestParkingPopup(popup, screen, view, mode)
 │                                         before Java is up: COnOffPresCtrl (§3.2)
 │                                         after: DSI → Java HMI ParkingProcessor(V2)
 │                                     → display context switch → video path (§4)
 │  fct 0x17 ASG = 1 VIS_ON            sendShowPopup FUN_083b7098
 │◄──────────────────────────────────  sendDeviceState FUN_083b217c
 │                                     (2 = QueuedUp, 0 = VIS_OFF)
 │ … reversing …
 │ fct 0x17 [xx,00] END / [00,00] OFF
 │──────────────────────────────────►  popup 0 → stopParkingPopup FUN_08413f88
 │                                     → stopRVC FUN_08413de4 → context 0x62 (splash)
 │                                     → video connection released (§4.3)
```

- **What ends a session.** The camera ECU decides when to drop its request
  [inferred]. In the car slogs a session lasts 8–90 s, so it covers the
  reversing itself plus a follow-up time.
  - The follow-up time can be set over BAP (fct 0x10).
  - A speed limit comes back as status fct 0x13, bit 2 "SpeedToHigh"
    [confirmed: `FUN_083a9220`].
  - An abnormal OPState (fct 0x0F) makes the MMI shut the park cycle down
    [confirmed: `FUN_083b5eec`].
- **A reset echo is ignored.** A `00 00` that the ECU sends after its own
  reset is ignored [confirmed].
- **The older 3G path is not used here.** It goes through
  `ActiveOpsRvcMode` (0 none, 1 OPS, 2 RVC, 3 OPS in RVC;
  `FUN_08393eb8` → `startRVC FUN_08413b8c`). This unit is 3GPLUS
  [confirmed: `FUN_085dfad8`].

### 3.2 Before the Java HMI is up: native "early RVC"

The camera must work right after the unit wakes, before the Java HMI is
ready. The native `COnOffPresCtrl` / `COnOffFSMHandler` covers that window
[confirmed]:

- **Setup.** `COnOffFSMHandler` init (`FUN_084260e8`):
  - connects to the layer manager;
  - registers displayable 0x11 (`lmgrRegisterDisplayable(0x11, 800, 480, 1)`);
  - declares two native display contexts: **0x61 = {0x11}** (camera only) and **0x62 = {0x28}** (splash);
  - pushes the camera's video settings (§4.2).
- **A request arrives.** `startParkingPopup` (`FUN_08413c64`) maps popup
  7/9/10 to switchContext(0x61). It also sets display power state 6
  (probably `POWERSTATE_ON_REARVIEW`, so the screen comes on for the camera
  even with the MMI off) [the value 6 is confirmed, its meaning inferred].
  In this state the MMI acknowledges VIS_ON at once ("Early-State OPS/RVC
  sendShowPopup continue for VisOn").
- **Handover.** Once Java reports `HMIReadyState == 2`, `FUN_08413e3c`
  drops the native handlers and **Java owns RVC from then on**.
  - Java's StartupManager waits for early RVC (`SYNC_EARLY_RVC`,
    "Waiting for RVC failed!").
  - An `EPHHandover` attribute (NO_REQUEST / ONE_DEVICE / LAST_REQ_RVC /
    LAST_REQ_SIDEVIEW) tells Java what was on screen.
- **The car-run events were not early RVC.** On 3.11 run1
  MMI3GApplication started at 00:00:06 and its parking service at 00:00:12.
  The `'RUN'` line at 00:01:49 is only srv-starter waiting for
  `/tmp/startLVM`, so the 00:01:33 camera event went through the normal
  Java path [confirmed: slog].

---

## 4. The video path inside the HMI

### 4.1 Call chain

These are the steps for the normal (Java) path. The early path enters at
step 3 with context 0x61 [confirmed at each address]:

1. Java `switchContext(3)` (`CONTEXT_HMI_REARVIEW_CAMERA`) → DispMgmt
   `internalSwitchContext`.
2. DispMgmt (`FUN_08214030`) sees that displayable 0x11 is video
   (`FUN_08218294`), maps it to source `eSRC_RVCAMERA` = 2
   (`FUN_0820ed2c`) and display 0 to sink `eSNK_MAIN_DISPLAY` = 1
   (`FUN_0820ee10`).
3. `requestVideoFocus(2, 1, on)` (`FUN_08156624`) → CVideoManager
   `FUN_0812c634` → `CVideoChannel::requestVideoFocus` `FUN_0813a680` →
   `establishConnection` `FUN_081398c4`.
4. `CVideoChannel::switchVideoPath` `FUN_0813937c` →
   `CVideoControlInterface::switchVideoPath` `FUN_08136a34` → path lookup in
   the in/out table (§4.2), collision checks (§9).
5. `setVideoPathInHW` `FUN_08135c10`: `SET_SOURCE` on each VP of the path
   (`FUN_08132878`), then `setCapActivity(ENABLE 1)` `FUN_081368a8`.
6. `setAllFeatures` `FUN_08139784`: standard, viewport, deinterlacing,
   brightness / contrast / saturation / hue, each through `setFeatureAtVP`
   `FUN_08134404`, then `UPDATE`.
7. Signal check (§7), then `lmgrShowDisplayables` with the context's list.

### 4.2 Where the numbers come from: hard-coded

The values come from a static C++ initialiser (0x0823f860) and are pushed
again at every boot by `COnOffFSMHandler` [confirmed]:

| Setting | HMI value | Applied at | dev-videoctrl / ADV7180 effect |
|---|---|---|---|
| Standard | `eSTD_NTSC` → driver value 5 (`FUN_0813296c`) | VP0 | reg 0x00 VID_SEL = NTSC-M (see the gotcha below) |
| Brightness | −12 | VP0 | reg 0x0A = −12 |
| Contrast | 8 | VP0 | reg 0x08 = 136 |
| Saturation ("color") | 12 | VP0 | reg 0xE3 / 0xE4 = 140 |
| Hue ("tint") | 0 | VP0 | reg 0x0B = 0 |
| Input dimension | derived from the standard: NTSC family → 720×480, others → 720×576 | VP1 | VCAP input size |
| Input viewport | x 6, y 0, 708×480 | VP1 | VCAP crops 6 px left and right |
| Deinterlacing | 1 = WEAVE | VP1 | both fields interleaved into one 480-line frame |

- **A second variant exists** for a 400×240 screen: viewport x 9, y 4.
- **Runtime overrides.** The DSI dataset tables
  (`onDataSetProfile…TableChanged`, `VMPersistency`) can change these
  values at run time; that path was not traced [inferred].
- **ADV7180 settings and ECU picture settings are different things.** The
  ADV7180 values above are separate from the brightness / contrast / colour
  % the HMI sends to the camera ECU over BAP (§8).

**In/out mapping table** (0x08a4f5ac, 30 rows of `src, snk, input per VP`)
[confirmed]:

| Source | VP0 (ADV7180 input) | VP1 (VCAP 0 input) | VP2 (VCAP 1) |
|---|---|---|---|
| `RVCAMERA` 2, `SVCAMERA` 15, `TV_RSE…` 22–25 | 1 | 0 | – |
| `AUX1` 11 | 2 | 0 | – |
| `TV` 3, `TV_AUX*` 17–19, `EXT_DVD` 20 | 0 | 0 | – |
| `DVD` 4, `FILE_VIDEO` 21 | – | 1 (DSP) | – |
| `DVD_RSE` 26 | – | – | 0 |

Brightness, contrast, colour, tint and standard are applied at the first
capable VP in the path, which is VP0 (the ADV7180). Viewport, input
dimension, deinterlacing and capture on/off are applied only at VP1/VP2
(the capture units) [confirmed: capability table 0x08a4f4fc].

**dev-videoctrl gotcha.** UPDATE applies the ADV7180 settings in this
order: brightness, contrast, saturation, hue, standard, source. The source
write rewrites all of reg 0x00. That wipes the standard (VID_SEL) written
just before it, so the chip falls back to autodetect
[confirmed: dev-videoctrl `FUN_08042640`]. The HMI sends both in one
UPDATE, so on the car the camera actually runs on autodetect [inferred]. It
makes no difference here, because autodetect finds NTSC.

### 4.3 Stop path

Switching to a context without video ("remove old Video-Connection as no
video is requested") calls `requestVideoFocus(…, 0)` → `FUN_0813a428` →
`stopActiveSource` `FUN_08139fdc` [confirmed]:

- `setCapActivity(snk, 0)` + UPDATE, which is the **only** thing the slog
  shows (`VP1: vcap disabled`);
- the path is removed from the HMI's cache (`FUN_08135a20`). **No
  SET_SOURCE is sent**, so VCAP 0 stays on input 0 and the ADV7180 stays
  on camera A.

---

## 5. Geometry: why there are no black bars

- **The camera displayable is full screen.** It is registered as
  `lmgrRegisterDisplayable(0x11, W, H, flags 1)`. W×H defaults to 800×480
  and is overwritten with the io-display mode, which is 800×480 on this
  unit [confirmed: `FUN_084260e8`, call at 0x084261a0].
  - Flags 1 means `LMGR_VIDEO` with no VCAP bit, and layermanagerV2 then
    uses VCAP 0 [confirmed: layermanagerV2 `FUN_08053238`, `FUN_08057c04`].
  - Native code never resizes it or sets a destination viewport for it
    (`lmgrResize` is only used for TV). Java's `requestSetPosition` passes
    only x/y, and nothing suggests it is used for 0x11
    [confirmed native, Java side inferred].
- **The scaler works on output size only.** layermanagerV2's
  `CVideoDisplayable::renderLayer` (`FUN_080535f4`) runs:
  `gf_vcap_attach`, `gf_vcap_bind_to_layer`,
  `gf_layer_set_dst_viewport(0, 0, 800, 480)`,
  `gf_layer_set_src_viewport(0, 0, 800, 480)`,
  `gf_vcap_set_output_dimensions(800, 480)`, `gf_vcap_update`,
  `gf_layer_enable`. The VCAP scaler turns the cropped 708×480 into
  exactly 800×480, and the layer shows it 1:1. Nothing in this chain keeps
  an aspect ratio [confirmed].
- **The result is a stretch.**

| | Pixels | Display aspect |
|---|---|---|
| Camera picture (NTSC, non-square 10:11 pixels) | 708×480 | ≈ 4:3 (1.34) |
| MMI panel | 800×480 | 5:3 (1.67) |
| Scale factors | ×1.13 horizontal, ×1.0 vertical | everything ~25 % wider than true 4:3 |

  With a wide-angle camera the stretch is hard to notice. The guide lines
  are in the analog picture (§6), so they stretch together with it.

---

## 6. What is drawn over the video, and by whom

**Layer order** [confirmed: layermanagerV2]:

- The video displayable always lands on **gf layer 6** (`nlayers − 2`).
  The show list fills layers top-down: the first entry gets gf 7 and the
  video gets gf 6.
- The video layer uses blend mode `GF_BLEND_DST_0`: it covers everything
  below it.
- Exactly **one** layer (gf 7) can be above it. With register flag 0x2
  ("transparent") it blends per pixel from the ARGB surface. Colour keying
  works on graphics layers only.

**Show lists:**

| Context | Displayables | Result |
|---|---|---|
| Native 0x61 (early RVC) | `{0x11}` | camera only, nothing on top [confirmed] |
| Java 3 `CONTEXT_HMI_REARVIEW_CAMERA` | `[0x10 HMI, 0x11 RVC]` | HMI surface on gf 7 over the camera on gf 6 [confirmed: lsd.jxe `declareContexts`] |
| Java 27 (rear-seat equivalent) | `[0x24 HMI2, 0x11 RVC]` | same, for the second display |

**Who draws what:**

- **Guide lines: the camera ECU.** MMI3GApplication has no guide-line,
  trajectory or steering-angle drawing code. The MMI only sends parking
  mode (BAP fct 0x12, value 0/1, probably PARKBOX / PARALLELTOROAD). The
  ECU reports `guidelinesLeftSide/RideSide` (sic), `calibration`,
  `trailerHitched` and similar in VideoInfo. So the lines come in the
  composite picture [inferred, strong].
- **PDC (OPS) bars, parking-mode buttons, popups: the Java HMI.** It draws
  them into the HMI surface (gf 7), with alpha where the camera should show
  through. The evidence is the templates `tmpRvc`, `tmpOpsRvc2`,
  `tmpOpsRvcLayer`, `tmpApsOpsRvcTopview…` and
  `CAR_OPS_RVC_SCREEN_SELECTOR_CHOICE` [inferred from names; no context
  contains `eDISP_OPS` 0x1C].
- **`OPS_OVERLAY_FKT` is for VPS only.** It asks the VPS (surround view)
  ECU to draw OPS into its own picture, not the RVC [confirmed].

Displayable ids (string table 0x0887ff20): SPLASH 0, HMI 0x10,
**REAR_VIEW_CAM 0x11**, OPS 0x1C, HMI2 0x24, PUBLIC_SPLASH 0x28.

---

## 7. No signal

- **Detection is one-shot per context switch, not continuous** [confirmed:
  around 0x08214ad6–0x08214b18].
  - After the video path is set up, DispMgmt asks for the signal status:
    `GET_STATUS` on VP0, read by `evaluateStatus` `FUN_08132e60`, where bit
    0x10 means locked.
  - The context switch waits for the answer. A no-signal status
    (`0x08000200` from dev-videoctrl) is retried several times on two
    timers ("50", then "125"; the unit is unknown).
  - After that it logs "VideoDetection failed 15 times" and **shows the
    context anyway**, so the driver sees a black picture.
- **No native error screen.** The native code has no "camera not
  available" screen. A Java error popup (`PARTIALPOPUPCARRVCERROR`) exists,
  but what raises it is [unknown].
- **The video watchdog is off.** It is disabled in the HMI config
  (0x088a764c = 0), and nothing on the firmware provides
  `/dev/videowatchdog`. That means no frozen-picture detection
  [confirmed].
- **Lock status on the car.** dev-videoctrl's GET_STATUS for the ADV7180
  is 0x10 (locked), plus 0x20 (FSC lock), plus `AD_RESULT << 24`. The car
  reports `0x130`: locked, FSC lock, NTSC-M. What the 0x100 bit means is
  [unverified]: the driver is said to take it from reg 0x13 bit 2
  (50 Hz), but the car's 0x13 = 0x69 has that bit clear.

---

## 8. Coding gates and BAP settings

The MMI ignores or refuses the ECU's request unless all of these hold
[confirmed]:

- **The camera is coded.** It is item 9 of the car device list
  (`DEVICE_LIST_RVC`, mask bit 0x200; GEM: per3 0x00100019 "Rearview
  camera"), and `isDeviceCoded(LSG 0x0B)` `FUN_087e7c54` is true.
  - Coded but not answering logs "RVC is coded but not built or defect –
    PLEASE CHECK THE GREEN MENUE".
- **The display flag is loaded and on.** Persistence key 0x51002
  (`CAMERA_DISPLAY_ONFF` → `mRVCStandaloneDisplayStatus`,
  `FUN_08398f50/f60`) must have been loaded. If it is 0, the MMI answers
  VIS_OFF. Whether this is a user setting or coding is [unknown].
- **A parking mode is set.** If OPS and RVC are both coded with APS default
  parking mode NONE, the request is refused ("RVC requests Focus: denied").
- **OPState is normal** (BAP fct 0x0F).

**BAP LSG 0x0B functions the MMI uses:**

| fct | Direction | Meaning |
|---|---|---|
| 0x10 | MMI → ECU | follow-up time |
| 0x12 | MMI → ECU | parking mode 0/1 |
| 0x13 | ECU → MMI | status bits, bit 2 = SpeedToHigh |
| 0x14 / 0x15 / 0x16 | MMI → ECU | colour / contrast / brightness, 0–100 % |
| 0x17 | both | DP_PRESENTATION: ECU request; MMI answer 1 VIS_ON, 2 QueuedUp, 0 VIS_OFF |

For comparison, OPS (parking sensors) is LSG 0x0A (DP fct 0x14) and side
view is LSG 0x1E (fct 0x17).

---

## 9. What this means for our apps

VCAP 0 is shared. The camera uses input 0, and our DSP video (hello6,
hello9, live streaming) uses input 1. These HMI facts matter
[confirmed unless marked]:

- **The HMI trusts its cache and never reads the hardware back.**
  - Path collision checks (`checkPathCollision` `FUN_08136044`,
    `checkVideoPathExclusion` `FUN_08133440`) only look at the HMI's own
    active-path table.
  - The watchdog is off and nothing re-asserts the path periodically.
  - If its cached path already matches, `setVideoPathInHW` sends nothing.
- **Seen on the car, 25.10.**
  1. At 00:06:33 the HMI switched the camera on (VP0 source 1, VP1
     source 0, enable).
  2. At 00:06:52 hello6 set VP1 to source 1 (DSP) and enabled it. The HMI
     sent nothing, so VCAP 0 was switched from the camera to our clip
     [inferred: slog].
  3. At 00:07:02 hello6's teardown sent source 0 + ENABLE 0 + UPDATE. Two
     `vcap disabled` lines follow at that second; the second is probably
     the HMI's camera stop [inferred]. Whether the driver saw the camera
     or our clip during those 10 s wasn't noted.
- **Our app can hide the camera from the driver.** Any app that sets VP1's
  source or enable while the camera is up does exactly that. The next
  camera session repairs it, because the HMI's stop path clears its cache
  and the next start sends SET_SOURCE again.
- **Rules for apps.**
  - Don't touch `/dev/videoctrl` VP0 at all: it is camera-only in
    practice.
  - Before switching VP1, make sure the camera is not active (see the
    first open question below).
  - On exit, **restore VP1 source 0** as well as ENABLE 0. Our teardown
    already does this, and it leaves the HMI's assumption (VCAP 0 on
    input 0) true.
- **Reusing the HMI's path from an app** (camera A to the screen):
  1. VP0 source 1, VP1 source 0, optional VP1 viewport / dimensions /
     deinterlacing, VP1 enable 1.
  2. `UPDATE` with payload **0**. The payload 1 makes the driver never
     reply, so the caller blocks forever.
  3. Show a video displayable with flags `0x101`.

  The UPDATE takes about 130 ms, because the driver runs a fixed ADI
  input-switch script with sleeps [confirmed: dev-videoctrl]. Poll
  `GET_STATUS {0, 1}`: `0x08000200` = no signal, bit 0x10 = locked.

---

## 10. Evidence from car runs

Every reversing event in the run slogs (12.10, 19.10 v2, 25.10, 28.10, 3.11
run1/run2) logs the same dev-videoctrl sequence within one second
[confirmed]:

```
VP0: hue changed to 0              ← standard + source + B/C/S/H, one UPDATE
VP0: standard changed to 5
VP0: source changed to 1           ← ADV7180 AIN2 = camera A
VP1: source changed to 0           ← VCAP 0 input 0 = ADV7180
VP1: input viewport changed to x1 6, y1 0, 708×480
VP1: input dimension changed to 720, 480
VP1: vcap enabled
VP1: mode changed to WEAVE
VP0: brightness -12, contrast 8, saturation 12, hue 0
VP0: status of register 0x10 of the ADV7180 0x07
VP0: status of register 0x13 of the ADV7180 0x69
VP0: status of the ADV7180 0x130
   … 8–90 s …
VP1: vcap disabled
```

The HMI's own `[Car][Parking]` traces don't reach slog; they go to the
trace channel. To see them, enable the `# Early RVC in OnOffPresCtrl` trace
scopes (`project_presctrl_onoff_COnOffPresCtrl_*`).

---

## 11. Open questions

- Is the camera active right now? An app needs a way to read this before
  touching VP1. Candidates: GET_STATUS on VP0 (only shows signal, not
  intent), the slog lines, or a DSI/BAP attribute.
- What the 0x100 bit in the ADV7180 status means (§7).
- Whether Java ever positions displayable 0x11 (`requestSetPosition`), for
  example in a split OPS/RVC view.
- Display power state 6 = `POWERSTATE_ON_REARVIEW`? Popup values 6, 8, 9
  and 10.
- Whether the camera B input (ADV7180 AIN3) is wired on this car.

---

## 12. Sources

Binaries (ifs-root rev 51): `usr/apps/MMI3GApplication`,
`usr/bin/dev-videoctrl`, `usr/bin/layermanagerV2`,
`etc/videoctrl-Audi3G.cfg`, `etc/mmi3g-srv-starter.cfg` (dev-videoctrl
`-c /etc/videoctrl-Audi3G.cfg -v`; MMI3GApplication depends on
`/dev/videoctrl` and `/dev/layermanager`). Java: efs-system `lsd.jxe`
(`ParkingProcessor(V2)`, `declareContexts`, StartupManager).

Decompiled with headless Ghidra 12 and `tools/DumpDecomp.java`. String and
call-pointer labels were then resolved against the ELF with
`tools/resolve_decomp.py`. The dumps live in
`third_party/decomp/P1001_rev51/` (gitignored; its README says how to
regenerate them).

Key addresses in MMI3GApplication:

| Address | What |
|---|---|
| `FUN_084260e8` | COnOffFSMHandler init: registers 0x11, contexts 0x61/0x62, pushes video values |
| `FUN_083b60e0`, `FUN_083b2cdc` | BAP DP_PRESENTATION handling and decoding |
| `FUN_083b217c` | sends VIS_ON / QueuedUp / VIS_OFF |
| `FUN_08413c64` / `FUN_08413f88` | start / stop parking popup (early path) |
| `FUN_08214030` | DispMgmt context switch: video part, signal check |
| `FUN_08136a34`, `FUN_08135c10` | switchVideoPath, setVideoPathInHW |
| `FUN_081368a8` | setCapActivity (`DEVCTL_VP_ENABLE`) |
| `FUN_08134404` | setFeatureAtVP |
| `FUN_08139fdc` | stopActiveSource |
| 0x08a4f5ac / 0x08a4f4fc / 0x08a4f4cc | in/out table, VP capability table, connection defaults |

Related: [phone-mirroring.md](phone-mirroring.md) §2.2 and §7.3 (VCAP,
dev-videoctrl devctls, video displayables), [native-apps.md](native-apps.md)
(videoctrl from apps).
