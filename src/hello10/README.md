# hello10

Can the media port capture an **800-sample line**? io-media-nvidia
captures the DM642's video signal at a literal 720 wide: three 16-bit words
in its code ([docs/phone-mirroring.md](../../docs/phone-mirroring.md)
§7.14). An 800×480 picture pixel for pixel (Android Auto's smallest video,
the panel's size) needs that capture at 800, plus a patched DSP image and a
VCAP change. hello10 answers the first part without touching the DSP or
the HMI's own io-media. Written in C++
([docs/cpp-apps.md](../../docs/cpp-apps.md)).

Writing the words in the running io-media is refused (`ESRVRFAULT`,
phone-mirroring §7.14), so hello10 starts a **second io-media from RAM**,
the file patched, next to the HMI's own.

| Test | What | Question |
|---|---|---|
| `iomcopy` (default) | an unchanged copy plays c3 (720×480, 10 s); only if it played, a copy with the capture width at 800 plays it (20 s); then the original plays it (8 s) | Can a copy run beside the original? Does the port capture an 800-sample line? |
| `probe` (by name) | read only: io-media-nvidia's three capture-width words through `/proc/<pid>/as`; its `pidin arg` / `env`; its segment list (`DCMD_PROC_MAPINFO`, flags named by `mapinfo.cpp`); the width word's page mapped `MAP_SHARED` from `/mnt/efs-system/sbin/io-media-nvidia` in our own process and its `posix_mem_offset64` | Are the addresses right? How is io-media's code mapped? |

The default run is about 2 minutes. It may leave the radio or the screen
broken until the MMI restarts (MENU + main knob + top-right key); hello10
says so on its first screen.

## How the copy works

- **The copy:** `/mnt/efs-system/sbin/io-media-nvidia` copied to the run
  directory as `iom-copy` (or `iom-copy800` with the three width words
  `d0 02` → `20 03` at file offsets 0x14a190, 0x14a26e, 0x14ad20; each
  must read 720 first). It is started with the original's `/etc/iom.cfg`
  plus `mountpoint = "/dev/hello10-iom"` in `global-options`
  (`iomcopy_cfg.h`, host-tested, checked on the unit's files), `-B` and
  srv-starter's `NV_*` environment. Nothing is written to flash.
- **Why it can share the unit [inferred from the P1001 binaries]:** the
  config's `mountpoint` replaces `/dev/io-media` (option parsing at
  `0x080892b0`); `-B` keeps it in the foreground (without it,
  `procmgr_daemon` gives it a new pid); io-media is a client of
  `devg-NVTS` (`/dev/nvdsprm0`) and `devg-NVMiniRM` (`/dev/nvminirm0`),
  and the DSP image is loaded once at boot by NvElfLoader, so the copy
  doesn't reload the DSP.
- **Not known:** whether `devg-NVTS` serves a second client, and what the
  copy's start-up does to the DSP tasks the original runs (the audio
  mixer, the health monitor). Hence the steps below and the radio left on.
- **Steps:** start the copy, wait for its mountpoint (15 s at most), 5 s
  idle, then play c3 through it (our player pointed at the copy with
  `iom_set_root`), stop it (TERM, KILL). `copy800` runs only if `copy720`
  played. `orig720` plays c3 through the HMI's io-media again.
- **Logged:** the copy's start (pid, time to the mountpoint), what appeared
  in `/dev`, `/dev/name/*`, `/dev/shmem`, `/dev/tsshmem` while it started,
  whether the copy and the original are alive after the idle and after the
  play, how the copy ended, and a `RESULT copy720` / `copy800` / `orig720`
  line each (the fields above; `query` is empty for the copy:
  testapp-cmdline knows only `/dev/io-media`). Files in `var/hello10/`:
  `iom-copy*_out.txt` (the copy's output), `iom-copy*_ls_before.txt`,
  `_ls_after.txt`.

| Seen | Meaning |
|---|---|
| `copy720` plays (TIME events, picture), radio on, `orig720` plays | a copy works beside the original |
| `copy800`: right 80 columns black or frozen | the media port stops at the line's end: an 800-wide signal from a patched DSP image would be captured |
| `copy800`: garbled or slanted picture | the port counts pixels: 800 needs a matching signal, or is out |
| no mountpoint, or the copy exits | it can't run beside the original (its `_out.txt` and the slog say why) |
| radio silent or HMI media broken afterwards | the copy disturbed the DSP; restart the MMI |

## Running

- **Run:** `hello10 <sd_path> [test,test,...]`, e.g. `probe`. `make sd`
  puts the clip (c3) on the card.
- **Output** (in `var/` on the card):
  - `hello10.log`: every step, the `RESULT` lines and the summary.
  - `hello10/`: the copy's files (above), `query_orig720.log` (io-media's
    view of the graph: `GetTrackInfo`, `GetVideoInfo`), `slog.txt` (the
    system log after the tests: io-media's and the DSP's messages, an
    assertion if one fired).

The `RESULT` lines have hello9's fields (its README): `playing_ms`,
`time_events`, `pos_ms`, `errors`, `video` and `WxH` from the query,
`knob`, `lag`.

## Results

- **Writing the width in the running io-media is refused** [confirmed]:
  the three words read 720, the first write fails with `ESRVRFAULT`
  ("Server fault on msg pass") and nothing changes (phone-mirroring §7.14).
  c3 played before and after it: picture on screen, io-media reports
  720×480, lag median ~330 ms and ~540 ms.
- **`probe`** [confirmed]: the width page is RAM shared by every io-media
  process, not flash; its physical address is unknown (phone-mirroring
  §7.14).
- **A second io-media can't run beside the original** [confirmed]: the copy
  (`iom-copy`, own mountpoint) logs `nv_dsp: Failed to open
  m_pNvmZlibEncoder` / `error returned is 8000000B` and dies of SIGSEGV
  (ip `0x08157ff0`) within ~3 s; `/dev/hello10-iom` never appears. The
  DSP side serves one io-media only [inferred]. The original io-media
  is unharmed: c3 played after it (720×480, lag median ~320 ms).
- An 800×480 stream with its SPS cropped to 720 stays black
  ([docs/android-auto.md](../../docs/android-auto.md) §4.1). Software
  decoding on the SH4 gets the picture on screen at ~5 fps at best
  (phone-mirroring §4).

## State

`probe` and `iomcopy` have run on the car; both are answered (above).
`copy800` (does the media port capture an 800-sample line) can't run
through a second io-media; it would need the running one patched, which
the kernel refuses.

`probe` output: `maps:` lines (each width word's segment: flags, dev, ino,
offset, what a `/proc` write there does; the same for hello10's own code,
run from the card), a `phys:` line (`posix_mem_offset64` of our mapping of
the file: rc, offset = physical address when it is plain memory, fd), and
`var/hello10/iom_maps.txt`, `iom_arg.txt`, `iom_env.txt`, `asinfo.txt`
(`pidin syspage=asinfo`: which physical ranges are RAM and which flash).
