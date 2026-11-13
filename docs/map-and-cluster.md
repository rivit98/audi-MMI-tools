# Navigation map and instrument cluster

This doc covers two things:

- how the HMI draws the navigation map on the MMI screen;
- how the head unit puts content on the instrument cluster's centre display (A6 C7);
- which of these paths an app could use.

Sources:

- the Ghidra dumps of `MMI3GNavigation` and `MMI3GApplication` (P1001, rev 51);
- `lsd.jxe` strings.

Addresses are virtual addresses in those binaries.

---

## 1. Who draws the map

- **NavCore does not draw.** It links no graphics library: no EGL, GLES or
  `libgf`. It is the routing and map-database engine. srv-starter starts it
  with `--api-routing=shmem` [confirmed: `readelf -d`, `etc/mmi3g-srv-starter.cfg`].
- **MMI3GNavigation draws.** It is one of the HMI's native processes, next to
  MMI3GApplication, MMI3GMedia, MMI3GMisc and MMI3GTelephone, and links
  EGL/GLES 1 and `libgf`. The map viewer (`CGMapViewerApp`,
  `CGMapLayerManagerImpl`, `CGAGLQnx`) runs there [confirmed: strings, dump].
- Java decides what is on screen. It asks the map viewer for windows over
  DSI (`GMapWindowManager`: `requestNewWindow(handle, cid, transparency)`,
  `requestSetViewport`, `requestSetVisible`) [confirmed: strings].

## 2. Map windows are displayables

Each map view is its own layer-manager displayable, owned by MMI3GNavigation.
None of them is drawn into the HMI's surface (16).

The displayable ids are the HMI's `eDISP_*` enum index + 13:

| cid | name | cid | name |
|---|---|---|---|
| 16 | HMI | 23 | MAP_JUNCTION_VIEW |
| 17 | REAR_VIEW_CAM | 24 | MAP_LANDMARK_VIEW |
| 18 | BROWSER | 28 | OPS |
| **19** | **MAP_VIEWER** (main map) | **33** | **KOMBI_MAP_VIEW** (cluster) |
| 20 | MAP_ROUTE_GUIDANCE | 36 | HMI2 |
| 21 | MAP_INTERSECTION_VIEW | 39 | GOOGLE_EARTH |
| 22 | MAP_3D_INTERSECTION_VIEW | 40 | PUBLIC_SPLASHSCREEN |

The enum name table is the pointer list at MMI3GApplication 0x0887ff20
(index 0 = `eDISP_INVALID`). The +13 rule fits all five ids that
[rear-camera.md](rear-camera.md) §6 confirmed: 16, 17, 28, 36 and 40
[inferred for the rest].

19 = map also explains why showing `{cid, 16, 19}` on exit brings the map
back: that is the HMI surface over the main map.

**Registration** (`FUN_0852142c`, CGMapLayerManagerImpl.cpp) [confirmed]:

- Screen windows: `lmgrRegisterDisplayable(cid, w, h, 0x24)`, or `0x26`
  for a transparent window. Then `lmgrSetDestinationViewport(cid, x, y, w, h, 0)`.
- Kombi windows: flags `4`, or `6` if transparent.
- Flag bit 3 (blit the front into the new back, platform-notes.md §3) is never
  set. Every frame is drawn in full.
- Size changes use `lmgrResizeDisplayable` or `lmgrSetSourceViewport`.
- The show list goes out with `lmgrShowDisplayables(cids, n, 0)` (`FUN_08520530`).
- The lmgr client is linked into the binary, not imported:
  - register = `FUN_086022b8`
  - set destination viewport = `FUN_08602578`
  - getVfb = `FUN_086024b0`

## 3. How a frame is drawn

Per update (`doUpdate` `FUN_0852291c`, `CGAGLQnx` around `FUN_0848fb0c`) [confirmed]:

1. `lmgrGetVfb(cid)` returns the back surface's sid.
2. `gf_surface_attach_by_sid`, then `eglCreatePixmapSurface` on that gf
   surface, then `eglMakeCurrent`.
3. GL ES 1 draws the map.
4. `lmgrUpdateVfb(cid)` flips the surfaces.

The EGL pixmap surfaces are cached per sid (`attachWindowSurface`,
`emptyWindowSurfaceCache`, config key `keepEglPixmapSurfaces`). This is the
same double-buffer rule our `screen.c` follows: the VFB's sid alternates on
every update.

**Renderers** (class names):

- `CG2DRenderer`, `CG25DRenderer` and `CG3DRenderer`, each with fixed- or
  float-point framebuffer back ends. 2.5D and 3D first draw into a pbuffer
  ("needs buffer surface").
- Tiles: `CGTexTileRenderer`, `CGGRTileRenderer`.
- Terrain: SOAR terrain and `CGSkyRenderer`.
- 3D landmarks: `CG3DGLModel`, VBOs and anti-aliased lines.
- Batched textured quads: `CGBatchRenderer` with `GL_QUADS_NV`.
- Traffic: `CGTmcRenderer`.
- nVidia register combiners: `glCombiner*NV`.

**Icons and text** don't use GL. `CGGRImageQnx` and `CIconRenderer` blit them
with `gf_draw_blit2`, from a colour surface plus a separate 8-bit alpha
surface (`GF_FORMAT_BYTE`).

**Compositing** is done in hardware by layermanagerV2 (rear-camera.md §6):
the HMI surface (16, ARGB with per-pixel alpha) lies over the map (19).

What this means for apps [inferred]:

- The map is a GL client on the same GPU as ours. That fits the GL apps'
  freezes over the map ([gpu-results.md](gpu-results.md) §4.4).
- To hide or cover the map, work with show lists and viewports.

## 4. The instrument cluster

The head unit has two independent ways to put content on the cluster's centre
display. Neither one is a display layer.

### 4.1 DDP2: text, arrows, compass, bars

The head unit sends no pixels. It sends draw commands, and the cluster renders
them with its own fonts and symbols.

```
Java CombiDriver / CombiCommandQueue (lsd.jxe, de/audi/tghu/combi/ddp2)
  │ DSI  org.dsi.ifc.datadisplayddp2.DSIDataDisplayDDP2
  ▼
MMI3GApplication  CSPHDataDisplayDDP2SA ("SPHDataDisplayDDP2" 3.1)
  ▼
CDataDisplayDDP2PresCtrl ─► CDataDisplayDDP2FSM ─► CDataDisplayQueue
  ▼
MOST logical device, FBlock 0xDD ─► MOST subsystem ─► IOC (/dev/ipc/ch4?)
  ▼
gateway/IOC ─TP2.0 on CAN─► cluster
```

- **FBlock 0xDD** [confirmed: `FUN_087c20be` returns `0xdd`]. DDP1 uses the
  same FBlock.
- **No TP2.0 code exists in the head unit.** TP2.0 appears only in an error
  text ("handleError: no TP2.0"). Something past the IOC converts the MOST
  calls [inferred]. Where the messages leave the process is
  [inferred]: through the MOST subsystem to `/dev/ipc/ch4`
  (vehicle-data.md §5).
- **Function IDs** [confirmed: function object constructors and the
  `[DD-MSC]` log names]:

| FktID | Function | FktID | Function |
|---|---|---|---|
| 0x300 | Power | 0x302 | Version |
| 0x330 | SetFrameStatus | 0x331 | FrameUpdate |
| 0x333 | DisplayStatus | 0x334 | SetCodePage |
| 0x340–0x343 | HMI / Navi / Telephone / Media state | 0x405 | TxBuffer (flow control) |
| 0x3a0 / 0x3c0 | maneuver: draw / update | 0x3b0 / 0x3d0 | text: draw / update |
| 0x3a1 / 0x3c1 | compass | 0x3b1 / 0x3d1 | cursor |
| 0x3a2 / 0x3c2 | distance bar | 0x3b2 / 0x3d2 | colour |
| 0x3a5 / 0x3c5 | traffic sign | 0x3b3 / 0x3d3 | text style |
| 0x3a6 / 0x3c6 | deviation bar | 0x3a3/0x3a4, 0x3c3/0x3c4 | lane guidance [inferred] |

- **Commands** (DSI requests): `SetText(frame, cell, text, style, update)`,
  `SetTextStyle`, `SetColor`, `SetCursor`, `SetManeuver`, `SetCompass`,
  `SetDistBar`, `SetDevBar`, `SetTrafficSign(frame, cell, symbol, restriction)`,
  `FrameUpdate`, `SetFrameStatus`, `SetCodePage`.
- **Text is cut to 97 bytes** including the terminator
  (`FUN_083f7b38`, "[DD] limit string from %d to %d") [confirmed].
- **Flow control:**
  - Start-up order: Power on, then Version, then TxBuffer.
  - A TxBuffer Get with a rolling index 1–255 and a 500 ms timer switches
    the FSM between normal and queueing mode. In queueing mode requests go
    into `CDataDisplayQueue`.
  - Source: subagent disassembly around 0x083f24f0 and 0x083f5d5c [inferred].
- **Which frames the cluster supports** is reported at run time (frame,
  bitmap and colour-depth versions, DisplayStatus) [unknown statically].

### 4.2 KOMO: pictures

This is the cluster map and intersection zoom. Java switches it on with
`DSIKOMOView.enableKomoView`. Route data goes through `DSIKOMONavInfo`.

1. MMI3GNavigation renders a map view with GL into a Kombi window
   (cid 33, flags 4/6). For Kombi windows `doUpdate` draws into a surface
   taken from the compressor, not into a layer-manager VFB
   ("Request a new surface from the Compressor") [confirmed: `FUN_0852291c`].
2. `requestCommitKombiUpdate` builds a key frame (a full picture), overlays
   and a bar graph with `gf_draw_blit2` [confirmed: strings].
3. `CAKombiCompressor` compresses the picture. The DSP job uses `nv_dsp`.
4. It sends the result as MOST **FBlock 0xD7, FID 0x300 (picture)** and
   **0x301**. Fading goes over MHP or the control channel [confirmed: strings,
   `FUN_082a8a20`, `FUN_082a8ff4`].

Picture message fields (`FUN_082a8a20`) [confirmed]:

| Field | Values |
|---|---|
| pixel format | ABGR |
| compression | NONE, RLE, ZLIB(1) |
| frame | full, base, overlay |
| grid | 2D, 2.5D, 2.5D_upper_mask |
| picture type | none, Exitview, KDK, predefined, Follow-Me |

Predefined pictures come from files (`CAKombiFileNameBroker`, index file).
The code checks whether the car codes a MOST cluster ("MOSTKombiCoded") and
a HUD [confirmed: strings]. Whether this car's cluster takes pictures is
[unknown]: no KOMO traffic appears in our car logs.

## 5. What an app can do

| Route | Verdict |
|---|---|
| Java OSGi bundle in the HMI's JVM that calls `DSIDataDisplayDDP2` (`setText`, `frameUpdate`, …) as a second client, the same pattern as the toolkit's `modules/per3-reader` | **Most practical.** The native side takes every request regardless of the sender: it queues when the TxBuffer is busy, and only logs a wrong power state. [unknown] whether DSI allows a second client. The HMI's `CombiDriver` repaints its frames on a timer (`CombiDDP2UpdateTimer`), so the bundle needs a frame the HMI does not use [inferred]. |
| A native app writing MOST telegrams to `/dev/ipc/ch4` | **No.** MMI3GApplication owns the IPC frame counters, the acks, the TxBuffer handshake and the power state, so a second writer breaks them (vehicle-data.md §8). |
| A native DSI client | Possible in principle, but the DSI wire protocol to the service broker is not decoded [unknown]. |
| Pictures through KOMO | Only on a cluster coded for pictures. Drawing into the Kombi surface would race MMI3GNavigation. |
| Patching the HMI's `CombiDriver` | Would work, but it changes system files. Apps don't do that. |

**First test (not yet run on the car):** a bundle that only logs what the
cluster reports: DisplayStatus, the version info and the supported frames.
Then a single `setText` to an idle frame.
