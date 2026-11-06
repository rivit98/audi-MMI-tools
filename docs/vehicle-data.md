# Vehicle data: what the MMI 3G+ reads from the car's CAN buses

Can the head unit (P1001, board rev 51) read data from the car, and how
could an app of ours get at it? This covers the path from the car's buses
through the I/O controller to the SH4, the two streams that carry car data,
what our car actually sent, and the read paths open to an app.

Every claim is labelled:

- **[confirmed]**: read from the P1001 rev 51 binaries (address given) or
  seen in data captured on the car (capture given).
- **[inferred]**: follows from confirmed facts, not exercised.
- **[unverified]** / **[unknown]**: an assumption, or not found yet.

Addresses are virtual addresses in the binary named. The Ghidra dumps are
in `third_party/decomp/P1001_rev51/` (`ndr.c`, `MMI3GApplication.c`);
`vehicle_data.recovered.c` there is a hand-cleaned version of every
function this document relies on.

---

## 1. Summary

```
 car buses (Antrieb, Komfort, Dashboard, Infotainment, Fahrwerk, Extended)
      │  CAN (and BAP on top of it)
      ▼
 IOC: Renesas V850, V850app.bin v1341 ── terminates CAN, no raw pass-through
      │  FPGA link (PCI 10EE:9411, window 0xB0000)
      ▼
 dev-ipc ─► /dev/ipc/ch2 … ch11
      │
      ├─ ch5 ─► ndr ─► NavCore (dead reckoning)            ← vehicle signals
      │          └─► /dev/ndr/{name,id}/…, /dev/navi        (tagged records)
      │
      ├─ ch3 + ch4 ─► MMI3GApplication MOST subsystem      ← BAP, car menus
      │                (IPCD, CHBIpcProtocol)                 (frames, 0xFADE)
      ├─ ch2 ─► on/off, power (docs/power-onoff.md)
      └─ ch8 ─► knobs and buttons (platform-notes.md §8)
```

- **Yes, the MMI reads car data, but never raw CAN frames.** There is no
  CAN driver or CAN socket on the QNX side [confirmed: no `dev-can*` or
  CAN library in ifs-root, none in the sysinfo dump's device listing, §2]. The IOC owns the buses and
  hands the SH4 pre-digested values on IPC channels.
- **Driving signals come on `/dev/ipc/ch5`** as small tagged records:
  wheel pulse counters, wheel direction, reverse gear, speedometer pulses,
  steering angle, gear position, temperatures, brake light, wipers, rain
  sensor, plus the unit's own gyro and accelerometer [confirmed, §3].
  `ndr` decodes them for the navigation.
- **A shell `cat` of ch5 works.** sysinfo_dump.sh read 1,364 bytes in 3 s
  on 2026-10-04; they parse with no bytes left over [confirmed, §3.3].
- **Car functions (BAP: parking aid, climate, door locking, cluster,
  lights) come through MMI3GApplication's MOST layer on ch3/ch4**
  [confirmed, §5]. That path is far harder to use (§6).
- **Arbitrary CAN messages are not reachable without changing the IOC
  firmware.** Which CAN ids reach ch5 is fixed in `V850app.bin`
  (repo research: `research/ARCHITECTURE.md`, `research/IOACTIVE_V850_REFERENCE.md`).

## 2. The hardware path

| Piece | What | Source |
|---|---|---|
| IOC | Renesas V850, firmware `MuIOC/Main/51/default/V850app.bin` (589,760 B, version 1341, build string `AUDI-3G-9411-C0-Version:-T01`) plus bootloader `V850bolo.bin` | `metainfo2.txt` `[MuIOC\Main\51\default\Application]` [confirmed] |
| Bus names in the IOC | `Antrieb`, `Komfort`, `Dashboard`, `Infotainment`, `Fahrwerk`, `Extended` | strings in `V850app.bin` [confirmed] |
| Link | `/usr/bin/dev-ipc -I38 -c10 -A0xB0000 -V10EE -D9411 -vvv`: the FPGA (PCI vendor 0x10EE, device 0x9411), 10 channels | running unit, `processes.txt` (2026-10-04) [confirmed] |
| Nodes | `/dev/ipc/ch2` … `ch11`, `debug`, `onoff`, `watchdog`; `/dev/ipc/ioc/ch5 → /dev/ipc/ch5` (made by `lnAll -f -Ps`) | `ls` in the sysinfo dump; `etc/mmi3g-srv-starter.cfg` [confirmed] |

MMI3GApplication knows the sleep state of each bus separately
(`[Car][Clock] Direkt mCANBusActivity.AntriebSleep = %d`, and the same for
the other five) [confirmed: strings]. Which of these buses are wired to the
unit and which it only hears about through the car's gateway is
[unknown]. The six names match the gateway's bus list, so the unit may sit
on one bus and learn about the others from the gateway [inferred].

### 2.1 Who uses which channel

From the firmware's strings and the open files of the running unit
(`pidin_fd.txt`, which stops before MMI3GApplication):

| Channel | Users | Content |
|---|---|---|
| ch2 | MMI3GApplication (`COnOffIPC`), MMI3GMisc | power state frames, docs/power-onoff.md §4 [confirmed] |
| ch3 | MMI3GApplication `IPCD::init` (0x08803f60) | MOST subsystem control channel [confirmed] |
| ch4 | MMI3GApplication `CHBIpcProtocol` (0x08806aa4) | MOST subsystem message channel: logical devices, BAP [confirmed, §5] |
| ch5 | `ndr` reads (fd 9, `r-`), NavCore holds it write-only (fd 12, `-w`) | vehicle signals [confirmed, §3] |
| ch6, ch7, ch10 | MMI3GMisc | [unknown] |
| ch8 | MMI3GApplication, MMI3GMedia, ejecthandler | knobs and buttons, platform-notes.md §8 [confirmed] |
| ch9 | MMI3GApplication | [unknown] |
| ch11 | dev-irc | [unknown] (infrared?) |

During the 3 s capture, ch2, ch4 and ch5 delivered data; the other channels
gave nothing (no button was pressed, so ch8 was quiet too).

## 3. ch5: the vehicle-signal stream

### 3.1 Record format

`ndr` opens `/dev/ipc/ioc/ch5` and hands each record to
`CProcessCh5::interpretBuffer` (`ndr` 0x08063208), a switch on the tag
[confirmed]:

```
 tag u8 | subtag u8 | len u16 LE | payload[len] | ts u16 LE
```

`ts` is the IOC's millisecond clock and wraps at 65536. ndr logs "RIZ:
JITTER IOC" when two wheel samples are not 60–150 ms apart [confirmed,
0x08061268].

### 3.2 Tags

| Tag | Handler (ndr) | Len | Payload | Where it comes from |
|---|---|---|---|---|
| 0x01 | GYRO 0x08061564 | 6 | gyro sample | the unit's own gyro (`sensor/Gyro/InternalGyro`) [inferred] |
| 0x02 | RIZ 0x08061268 | 1–8 | 4 × u16 wheel pulse counters. **subtag = bitmask of valid wheels** | car (wheel speed sensors) [inferred] |
| 0x03 | REV 0x08061f5c | 1–4 | 1 byte per wheel, same mask. **1 = forwards, 2 = backwards** (hfs handler 0x0805fa20 sets ReverseGear = byte0 == 2) | car [inferred] |
| 0x04 | STR_ANGLE 0x0806113c | 3 | steering angle | car |
| 0x05 | ExternalGYRO 0x08061828 | 8 | yaw rate from a car ECU | car |
| 0x06 | BrakeLightESP 0x08061e2c | 2 | brake light / ESP | car |
| 0x07 | TractionBrake 0x08061c98 | 1 | | car |
| 0x08 | OutsideTemp 0x08061064 | 1 | outside temperature | car |
| 0x09 | RainSens 0x0806244c | 1 | rain sensor | car |
| 0x0a | Wiper 0x080622e0 | 1 | wiper state | car |
| 0x0b | CURVETRONIC 0x08062210 | 1 | | car |
| 0x0c | RevGear 0x08061b6c | 1 | reverse gear state | car |
| 0x0d | SpeedoPulse 0x08060ee4 | 2 | u16 speedometer pulse counter, copied into all four wheel counters | car, for models without per-wheel ticks [inferred] |
| 0x0e | GearPosition 0x08061a28 | 10 | byte 9 = gear | car |
| 0x0f | TEMPERATURE 0x08062c80 | 16 | 8 × s16. Channel 1 in 0.1 °C, valid if subtag bit 1 (hfs handler 0x0805f7ac: °C = (ch1 + 5) / 10) | [unknown] which sensors |
| 0x10 | 3DAcceleration 0x08062648 | 14 | 3-axis sample | the unit's own accelerometer (`Internal3Daccelerometer`) [inferred] |
| 0x11 | 3DAccelerationHr 0x08062564 | 80 | high-rate block | the unit's own accelerometer |
| 0xf0 | TIMEDATE 0x08063044 | 7 | time and date | car (cluster clock?) |
| 0xff | PROTOCOL 0x08062f18 | 4 | ch5 protocol version | IOC |

Handler names, lengths and log tags (`IGyro`, `RizW4`, `RevW4`, `StWhe`,
`EGyro`, `BrESP`, `TrBrk`, `Tempr`, `RSens`, `WipSt`, `Curve`, `TimeD`,
`PrVer`) are [confirmed]. Field meanings beyond what the table says are
not decoded yet.

### 3.3 What our car sent (2026-10-04)

`tools/ch5dump.py` on `third_party/system-info-dump/var/sysinfo/epoch-20261004_101449/ipc/ch5.bin`:

| Tag | Records | Period | Payload | Reading |
|---|---|---|---|---|
| GYRO | 19 | 160 ms | changing | |
| 3DAcceleration | 30 | 100 ms | changing | |
| RIZ, subtag 1 | 30 | 100 ms | `04 00 00 00 00 00 00 00` | only wheel 0 valid, counter 4, **not moving** |
| REV, subtag 1 | 5 | 600 ms | `01 00 00 00` | wheel 0 forwards: **not in reverse** |
| TEMPERATURE, subtag 3 | 3 | 900 ms | `ea 01 78 00` + zeros | ch0 = 490, ch1 = 120 → **12 °C** |

All 1,364 bytes parse [confirmed]. The car was parked, matching the
stationary counter. That the subtag-1 car sends one wheel counter rather
than four is [inferred] from the mask. 12 °C fits an October morning,
so channel 1 is probably the outside temperature [inferred]; 490
(49.0 °C?) may be a temperature inside the unit [unverified]. Steering
angle, gear, brake and wiper records did not appear in 3 s with the car
parked. Whether they need the ignition on or a moving car is [unknown].

## 4. ndr and its file tree

`/usr/bin/ndr -hfs=/etc/hfs.cfg -e=lvm -e=disChina` (running unit
[confirmed]) decodes ch5 and the GPS, and runs two servers:

- **`/dev/ndr/cmd`, `/dev/ndr/msq`, `/dev/ndr/notify`**: NavCore's
  interface (`CLibResMgr::ndrOpen/ndrRead/ndrWrite`, all via `devctl`,
  with a version check) [confirmed: NavCore strings]. The devctl codes are
  not decoded.
- **The hfs tree**: each line of `/etc/hfs.cfg` (`0x00021006
  sensor/ReverseGear` …) becomes a node under four resource managers
  [confirmed, "hfs configure" in ndr.c]:
  - `/dev/ndr/name/<path>`, also linked as `/dev/navi`
  - `/dev/ndr/id/<id>`
  - `/dev/ndr/debug/name/<path>`, `/dev/ndr/debug/id/<id>`

  `CIDNode::read` (0x080963c8) returns the node's current value from the
  read offset. With a flag set in the open control block it renders the
  bytes as hex text instead ("0123456789ABCDEF", 3 characters per byte)
  [confirmed]. That the `debug/` trees are the hex ones is [inferred].

The vehicle nodes in `hfs.cfg`, and the ch5 tags that fill them
(`CProcessCh5_hfs::interpretBuffer`, 0x0806071c) [confirmed]:

| Node | Id | Filled from |
|---|---|---|
| `sensor/Gyro/InternalGyro` | 0x00021000 | GYRO |
| `sensor/WheelCounter/AllWheels` | 0x00021005 | RIZ |
| `sensor/ReverseGear` | 0x00021006 | REV, RevGear, GearPosition |
| `sensor/Temperature` | 0x00021008 | TEMPERATURE |
| `sensor/Accelerometer/Internal3Daccelerometer` | 0x00021009 | 3DAcceleration |
| `sensor/GPS/…` | 0x0002100a–0x00021023 | GPS receiver, not CAN |

The ReverseGear value is an 8-byte record, `time u32, status u8, flags u8,
value u8, pad`. `value` is 1 when reversing, whichever tag set it
[confirmed for REV, 0x0805fa20]. Steering angle, brake, wipers, outside
temperature and speed pulses are decoded by ndr but **have no hfs node**.
They only reach NavCore's devctl interface.

## 5. BAP and car functions: ch3 and ch4

MMI3GApplication's MOST subsystem talks to the IOC as if it were a MOST
network of logical devices:

- `IPCD::init` (0x08803f60) opens `/dev/ipc/ch3` as the control channel,
  waits up to 5 s for "external clients", then starts `CHBIpcProtocol`
  on channel 4 and sends a 9-byte start-up frame (`03 00`, timestamp u32,
  IPC version `01 03 00`) [confirmed].
- `CHBIpcProtocol::transmitTelegram` (0x08806908) prefixes each telegram
  with a running frame counter and a type byte. The receive thread checks
  the counter and a `0xFADE` end marker [confirmed: strings, code].
- The ch4 bytes captured on the car match this: `cnt u8 | 01 | len u16 |
  len−1 bytes | FA DE`, with counters 0xe6, 0xe7, 0xe8 … [confirmed:
  `ipc/ch4.bin`]. The telegrams inside were not decoded.
- BAP (the VAG protocol carried over CAN between control units) reaches
  the car code as a stream per control unit ("LSG"):
  `CBapCommDevCtrl::processStream` (0x081da084) reads `lsg u8`, then
  `{fid u8, len u16, payload}` repeatedly. fid 2 is the BAP config
  (major, minor) [confirmed]. Above it sit the `[Car]…` handlers: parking
  aid, rear camera (docs/rear-camera.md), climate (`CCarClimateExtCDEFHandler`),
  door locking, interior and exterior light, cluster settings
  (`BAP - sendSetLanguage`, `sendInspectionReset`, …). Each handler logs
  "LSG 0x%02x: CanBus is up/down!" [confirmed: strings].
- Which telegrams on ch4 carry BAP, and in what framing, is [unknown].

## 6. What an app of ours can do

| Route | Gets | Effort | Risk |
|---|---|---|---|
| **A. `read()` `/dev/ndr/debug/name/sensor/…`** (or `/dev/navi/…`) | reverse gear, wheel counters, temperature, internal gyro/accel | a shell `cat`, or `open`/`read` in C | low; ndr serves several clients [inferred] |
| **B. `read()` `/dev/ipc/ch5`**, parsed as §3.1 | everything in §3.2 the car sends | small parser (`tools/ch5dump.py` shows it) | **[unknown] whether a second reader takes records away from ndr.** On ch8 every reader gets every message (platform-notes.md §8), and sysinfo's 3 s read on ch5 did no visible harm, so a copy per reader is likely [inferred]. Losing records would hurt the navigation's dead reckoning. |
| **C. NavCore's `/dev/ndr/cmd`/`notify` devctl** | everything ndr decodes, with change notifications | decode the devctl protocol from NavCore's `CLibResMgr` | low once decoded |
| **D. Java: DSI persistence** (`per 3` namespace) | values the IOC maps into per 3, e.g. battery voltage (`research/ARCHITECTURE.md`) | OSGi bundle, see `modules/per3-reader` | low |
| **E. ch4 / BAP** | car function states (doors, climate, parking aid) | decode the MOST telegrams first | high: MMI3GApplication owns the protocol (frame counters, acks), so a writer would break it. Reading is [unknown] like ch5 |
| **F. Other CAN messages** (RPM, coolant, …) | anything on the buses | patch and reflash `V850app.bin` | very high: a bad IOC image can leave the unit without power control |

For a first app, **A then B** is the practical order: A needs no
protocol work and B adds speed, steering and gear.

## 7. Open questions

- Does reading `/dev/ipc/ch5` alongside ndr give each reader every record?
  Test: read ch5 for a minute while driving, with the map in dead-reckoning
  view (or a tunnel), and compare with `ndr`'s own counters.
- The `/dev/ndr/name/…` tree: is it really there, are the nodes readable,
  and what does `debug/` print?
- Which tags appear with the ignition on, the engine running, while
  driving, reversing and steering. Units and scaling of STR_ANGLE,
  SpeedoPulse, GearPosition, OutsideTemp and TIMEDATE.
- Which telegrams on ch4 carry BAP, and whether reading ch4 disturbs
  MMI3GApplication.
- Which buses the unit is wired to (§2).

## 8. Next car run

A shell script on the SD card, no app needed:

1. `ls -lR /dev/ndr /dev/navi` and `cat` each `debug/name/sensor/*` node
   once → answers §7's tree questions.
2. 10 s captures of `/dev/ipc/ch5` (as sysinfo_dump.sh does: `cat … &`,
   sleep, kill) in these states, one file each: ignition on, engine running,
   steering lock to lock, in reverse, rolling forwards, wipers on.
   `python3 -I tools/ch5dump.py <file>` on the desk, then fill in the
   §3.2 payload columns.
3. Note whether the navigation's position stays right during the captures
   (route B's risk).

## 9. Sources

| What | Where |
|---|---|
| ndr dump, strings resolved | `third_party/decomp/P1001_rev51/ndr.c` (`ndr.raw.c` without the resolver pass) |
| Hand-cleaned reconstruction of the functions above | `third_party/decomp/P1001_rev51/vehicle_data.recovered.c` |
| ndr Ghidra project, log, binary copy | `third_party/decomp/P1001_rev51/work/ndr/` |
| MMI3GApplication: IPCD::init 0x08803f60, CHBIpcProtocol::init 0x08806aa4, transmitTelegram 0x08806908, "/dev/ipc/ch" 0x087e0fb4, BapCommDevCtrl::processStream 0x081da084 | `third_party/decomp/P1001_rev51/MMI3GApplication.c` |
| Captures: ch2, ch4, ch5 (3 s each), processes, open files | `third_party/system-info-dump/var/sysinfo/epoch-20261004_101449/` |
| ch5 decoder | `tools/ch5dump.py` |
| IOC firmware | `third_party/HN+R_EU_AU_P1001/MuIOC/Main/51/default/V850app.bin` |
| Earlier V850 / per 3 research (K0942) | repo root `research/ARCHITECTURE.md`, `research/IOACTIVE_V850_REFERENCE.md`, `research/PER3_READER.md` |
