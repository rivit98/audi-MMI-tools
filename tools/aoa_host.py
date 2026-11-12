# /// script
# requires-python = ">=3.12"
# dependencies = ["pyusb==1.3.1", "av==19.0.2", "pygame==2.6.1"]
# ///
"""aoa_host.py - a desk stand-in for the MMI side of audi_stream_receiver.

Does what src/audi_stream_receiver/aoa.cpp does, with libusb on a PC: switches a USB
Android phone into accessory mode (AOA), then speaks the audi_stream_receiver
protocol (src/audi_stream_receiver/proto.h): HELLO / HELLO_REPLY, then the phone's
screen. Replug the phone at will; it goes back to waiting, like the MMI
receiver. Ctrl-C to stop.

Video: announces the MMI's boxes (VIDEO_CAPS, 800x480 screen, as
src/audi_stream_receiver/video.cpp), reassembles the phone's pictures, acks each as
shown and, with --save DIR, keeps the newest one as DIR/latest.jpg.

H.264 (the phone app's "H.264" format): announces the MMI's limits
(720x576, 25 fps, frames up to 256 KB), takes H264_CONFIG, checks every
frame the way the MMI does (an IDR first, no bigger than the config's
frame size), acks it at once and answers H264_STATUS like
src/audi_stream_receiver/hwvideo.cpp. With --save DIR the stream goes to
DIR/stream.h264 (a new config starts a new file): `ffplay DIR/stream.h264`,
or `python3 media/csmp4.py DIR/stream.h264 out.mp4 25` for the MMI's own
container. Every 25 frames: frame rate, sizes, IDRs.

Window (tools/aoa_screen.py; --no-display or no display at hand: none): what
the MMI's screen would show. It runs in this process; USB runs in a child
process, because that one starts over whenever the USB bus changes. Acks go
out before a frame reaches the window, so they say nothing about decoding: this
shows when and why the phone stops sending, not the MMI's timing.

A phone already in accessory mode that this run didn't switch may belong to
another app (Android Auto after `make aa-host`), and so may one whose first
data isn't this protocol: such an accessory is switched again with these
strings and the USB port reset, which hands it to Audi streaming.

Log lines carry the time of day, to line up with what you do on the phone. A pause
in the frames of GAP_S or more gets its own lines, and so does how a stream ended.

Run with `make aoa-host` (Docker, USB passthrough, saves into
build/aoa-host/) or `uv run tools/aoa_host.py [--save DIR]` (needs access to
/dev/bus/usb). Stop adb first if it holds the phone. Test with `make aoa-host-test`.
"""

import errno
import fcntl
import json
import os
import signal
import struct
import subprocess
import sys
import threading
import time
from collections.abc import Iterator
from pathlib import Path

import usb.core
import usb.util

MANUFACTURER, MODEL = "mmi3g", "audi_stream_receiver"  # = aoa.h, matched by accessory_filter.xml
STRINGS = [MANUFACTURER, MODEL, "desk aoa_host.py", "1", "https://github.com/dspl1236/MMI3G-Toolkit",
           "desk-0001"]
HELLO_TEXT = b"audi_stream_receiver-desk/1"

AOA_VID, AOA_PIDS = 0x18D1, range(0x2D00, 0x2D06)
AOA_GET_PROTOCOL, AOA_SEND_STRING, AOA_START = 51, 52, 53
HANDSHAKE_FIRST_S, HANDSHAKE_RETRY_S, HANDSHAKE_TRIES = 4, 5, 5
SYS_USB_DEVICES = "/sys/bus/usb/devices"
HELLO, HELLO_REPLY = 1, 4  # 2, 3: TEXT / ACK, retired
VIDEO_CAPS, VIDEO_START, VIDEO_DATA, VIDEO_STOP, VIDEO_DETAILS = 5, 6, 7, 9, 10  # 8: VIDEO_ACK, retired
H264_CONFIG, H264_STATUS, VIDEO_ACKS = 11, 12, 13
H264_PLAYING, H264_SYNC, H264_REFUSED, H264_CONFIG_WANTED = 0, 1, 2, 3
VIDEO_SHOWN, VIDEO_BAD, VIDEO_DROPPED = 0, 1, 2
HDR, MAX_PAYLOAD = 3, 2048
SCREEN_W, SCREEN_H, MAX_BYTES = 800, 480, 512 * 1024
FMT_JPEG, FMT_H264 = 1, 3
H264_MAX_W, H264_MAX_H, H264_FPS, H264_MAX_FRAME = 720, 576, 25, 256 * 1024  # = hwvideo.h
CAPS = struct.pack("<HHHHIBHHBI", SCREEN_W, SCREEN_H, SCREEN_W // 2, SCREEN_H, MAX_BYTES,
                   1 << (FMT_JPEG - 1) | 1 << (FMT_H264 - 1),
                   H264_MAX_W, H264_MAX_H, H264_FPS, H264_MAX_FRAME)
EXT = {FMT_JPEG: "jpg"}
SAVE_DIR = sys.argv[sys.argv.index("--save") + 1] if "--save" in sys.argv else None
GAP_S = 1.0
PHONE_TYPES = {HELLO, HELLO_REPLY, VIDEO_START, VIDEO_DATA, VIDEO_STOP, VIDEO_DETAILS, H264_CONFIG}
RESWITCH_TRIES = 3
SWITCHED_ENV = "AOA_HOST_SWITCHED"  # "1": the accessory is ours (survives the re-exec)
PIPE_BYTES = 1024 * 1024


def log(message: str) -> None:
    """One line with the time of day."""
    now = time.time()
    print(f"{time.strftime('%H:%M:%S', time.localtime(now))}.{int(now * 1000) % 1000:03} {message}",
          flush=True)


class ScreenLink:
    """The USB process's end of the pipe to the window: a JSON head, then a binary body."""

    def __init__(self, fd: int | None) -> None:
        """fd None: no window, everything sent is dropped."""
        self.fd = fd

    def send(self, kind: str, body: bytes = b"", **fields) -> None:
        """One message; with the window gone this process has no more reason to run."""
        if self.fd is None:
            return
        head = json.dumps({"kind": kind, **fields}).encode()
        data = memoryview(struct.pack("<II", len(head), len(body)) + head + body)
        try:
            while data:
                data = data[os.write(self.fd, data):]
        except OSError:
            log("the window is gone: stopping")
            sys.exit(0)


def read_messages(fd: int) -> Iterator[tuple[dict, bytes]]:
    """The window's end: (head, body) until the USB process is gone."""
    with open(fd, "rb") as pipe:
        while len(sizes := pipe.read(8)) == 8:
            head_len, body_len = struct.unpack("<II", sizes)
            yield json.loads(pipe.read(head_len)), pipe.read(body_len)


SCREEN = ScreenLink(int(sys.argv[sys.argv.index("--screen-fd") + 1])
                    if "--screen-fd" in sys.argv else None)


def link_state(state: str, detail: str) -> None:
    """The link state on the window's status panel (= src/audi_stream_receiver/model.h)."""
    SCREEN.send("link", state=state, detail=detail)


class FrameGaps:
    """Notices pauses in the phone's frames, and says how long before a stream's end."""

    def __init__(self, gap_s: float = GAP_S) -> None:
        """No frame seen yet."""
        self.gap_s, self.last, self.in_gap = gap_s, None, False

    def frame(self, now: float) -> None:
        """A frame arrived."""
        if self.last is not None and now - self.last >= self.gap_s:
            log(f"frames back after a pause of {now - self.last:.1f} s")
        self.last, self.in_gap = now, False

    def idle(self, now: float) -> None:
        """Called now and then; logs a pause once, while it lasts."""
        if self.last is not None and not self.in_gap and now - self.last >= self.gap_s:
            log(f"no frames for {now - self.last:.1f} s (no VIDEO_STOP, link up)")
            self.in_gap = True

    def ended(self, why: str, now: float) -> None:
        """The stream ended: log why and how long since its last frame."""
        ago = "no frames" if self.last is None else f"last frame {now - self.last:.1f} s before"
        log(f"stream ended: {why}, {ago}")
        self.last, self.in_gap = None, False


def frame(ftype: int, payload: bytes = b"") -> bytes:
    """Encode one protocol frame."""
    return struct.pack("<BH", ftype, len(payload)) + payload


def ack_frame(status: int) -> bytes:
    """VIDEO_ACKS for one picture: this host acks at once, the MMI batches H.264 frames."""
    counts = [0, 0, 0]
    counts[status] = 1  # VIDEO_SHOWN, VIDEO_BAD, VIDEO_DROPPED
    return frame(VIDEO_ACKS, struct.pack("<HHH", *counts))


class Decoder:
    """Reassembles frames from arbitrary chunks; skips oversized ones."""

    def __init__(self) -> None:
        """Start with an empty buffer."""
        self.buf, self.skip, self.oversized = b"", 0, 0

    def feed(self, data: bytes) -> list[tuple[int, bytes]]:
        """Return the frames completed by data."""
        out = []
        self.buf += data
        while True:
            if self.skip:
                n = min(self.skip, len(self.buf))
                self.buf, self.skip = self.buf[n:], self.skip - n
                if self.skip:
                    return out
            if len(self.buf) < HDR:
                return out
            ftype, n = struct.unpack_from("<BH", self.buf)
            if n > MAX_PAYLOAD:
                log(f"  oversized frame ({n} bytes) skipped")
                self.oversized += 1
                self.buf, self.skip = self.buf[HDR:], n
                continue
            if len(self.buf) < HDR + n:
                return out
            out.append((ftype, self.buf[HDR:HDR + n]))
            self.buf = self.buf[HDR + n:]


class Peer:
    """Is the app on the other end Audi streaming? Its first data says."""

    def __init__(self) -> None:
        """Nothing heard yet."""
        self.known = False

    def foreign(self, oversized: int, frames: list[tuple[int, bytes]]) -> bool:
        """True if data before any frame of this protocol isn't this protocol."""
        if self.known:
            return False
        if oversized or any(ftype not in PHONE_TYPES for ftype, _ in frames):
            return True
        self.known = bool(frames)
        return False


def reswitch(acc) -> None:
    """Hand an accessory another app holds to Audi streaming: our strings, then a port reset."""
    why = try_switch(acc)
    if why is not None:
        log(f"  switching again failed ({why}); resetting the port anyway")
    os.environ[SWITCHED_ENV] = "1"
    try:
        acc.reset()
    except usb.core.USBError as e:
        if e.errno != errno.ENOENT:  # ENOENT: the phone is re-enumerating already
            log(f"  port reset: {e}")


def find_accessory():
    """The phone already in accessory mode, or None."""
    return usb.core.find(custom_match=lambda d: d.idVendor == AOA_VID and d.idProduct in AOA_PIDS)


class Handshakes:
    """When to try AOA on each device, like aoa.cpp: a phone just enumerated may not answer yet."""

    def __init__(self, first_s: float = HANDSHAKE_FIRST_S, retry_s: float = HANDSHAKE_RETRY_S,
                 tries: int = HANDSHAKE_TRIES) -> None:
        """Nothing seen yet."""
        self.first_s, self.retry_s, self.max_tries = first_s, retry_s, tries
        self.next_at: dict = {}
        self.count: dict = {}

    def due(self, key, now: float) -> bool:
        """True if the device at key should be tried now; the first call starts its wait."""
        next_at = self.next_at.setdefault(key, now + self.first_s)
        return now >= next_at and self.count.get(key, 0) < self.max_tries

    def tried(self, key, now: float) -> int:
        """Record a try; returns how many so far."""
        self.count[key] = self.count.get(key, 0) + 1
        self.next_at[key] = now + self.retry_s
        return self.count[key]


def try_switch(dev) -> str | None:
    """AOA handshake on one device; None if START was sent, else why not."""
    try:
        ver = dev.ctrl_transfer(0xC0, AOA_GET_PROTOCOL, 0, 0, 2, timeout=1000)
        version = ver[0] | ver[1] << 8 if len(ver) == 2 else 0
        if version < 1:
            return f"AOA version {version}"
        log(f"{dev.idVendor:04x}:{dev.idProduct:04x} speaks AOA v{version}, switching")
        for i, s in enumerate(STRINGS):
            dev.ctrl_transfer(0x40, AOA_SEND_STRING, 0, i, s.encode() + b"\0", timeout=1000)
        dev.ctrl_transfer(0x40, AOA_START, 0, 0, None, timeout=1000)
    except usb.core.USBError as e:
        return str(e)
    return None


def bus_devices(root=SYS_USB_DEVICES) -> set[tuple[str, str]]:
    """The (bus, address) of every USB device, from sysfs."""
    found = set()
    for d in Path(root).glob("*"):
        try:
            found.add(((d / "busnum").read_text().strip(), (d / "devnum").read_text().strip()))
        except OSError:  # an interface, or a device just gone
            pass
    return found


# Read before libusb starts. In a container libusb keeps the device list it
# found at startup, so when the bus differs from this the process starts over.
BUS_AT_START = bus_devices()


def restart_if_bus_changed() -> None:
    """Replace this process with a new one if USB devices came or went since it started."""
    if bus_devices() != BUS_AT_START:
        log("USB devices changed: restarting to see them")
        os.execv(sys.executable, [sys.executable, *sys.argv])


def wait_for_accessory():
    """Switch any Android device to accessory mode and return it."""
    handshakes = Handshakes()
    link_state("waiting", "plug the phone into the USB port")
    while True:
        restart_if_bus_changed()
        acc = find_accessory()
        if acc is not None and os.environ.get(SWITCHED_ENV) != "1":
            log("the phone was in accessory mode already, maybe for another app (make aa-host?): "
                "switching it to Audi streaming")
            reswitch(acc)
            time.sleep(2)
            continue
        if acc is not None:
            return acc
        for dev in usb.core.find(find_all=True):
            key, now = (dev.bus, dev.address), time.monotonic()
            if dev.bDeviceClass == 9 or not handshakes.due(key, now):
                continue
            attempt = handshakes.tried(key, now)
            why = try_switch(dev)
            if why is None:
                os.environ[SWITCHED_ENV] = "1"
                link_state("switching", "phone is switching to accessory mode")
                break
            if attempt in (1, HANDSHAKE_TRIES):  # first and last: other USB devices refuse too
                log(f"{dev.idVendor:04x}:{dev.idProduct:04x}: no AOA ({why}), "
                      f"try {attempt} of {HANDSHAKE_TRIES}")
        time.sleep(1)


def open_endpoints(acc):
    """Claim the ff/ff accessory interface; return (iface, ep_in, ep_out)."""
    cfg = acc.get_active_configuration()
    for intf in cfg:
        if (intf.bInterfaceClass, intf.bInterfaceSubClass) != (0xFF, 0xFF):
            continue
        if acc.is_kernel_driver_active(intf.bInterfaceNumber):
            acc.detach_kernel_driver(intf.bInterfaceNumber)
        usb.util.claim_interface(acc, intf.bInterfaceNumber)
        eps = [e for e in intf if usb.util.endpoint_type(e.bmAttributes) == usb.util.ENDPOINT_TYPE_BULK]
        ep_in = next(e for e in eps if usb.util.endpoint_direction(e.bEndpointAddress) == usb.util.ENDPOINT_IN)
        ep_out = next(e for e in eps if usb.util.endpoint_direction(e.bEndpointAddress) == usb.util.ENDPOINT_OUT)
        return intf.bInterfaceNumber, ep_in, ep_out
    raise RuntimeError("no ff/ff accessory interface")


def nal_units(data: bytes) -> list[bytes]:
    """The NAL units of an Annex-B buffer (start codes and trailing zeros left out)."""
    starts, i = [], data.find(b"\0\0\1")
    while i >= 0:
        starts.append(i + 3)
        i = data.find(b"\0\0\1", i + 3)
    out = []
    for k, s in enumerate(starts):
        e = starts[k + 1] - 3 if k + 1 < len(starts) else len(data)
        nal = data[s:e].rstrip(b"\0")
        if nal:
            out.append(nal)
    return out


def sps_poc_type(sps: bytes) -> int | None:
    """pic_order_cnt_type of a Baseline / Main SPS (NAL header included), or None."""
    rbsp = sps.replace(b"\0\0\3", b"\0\0")
    bits = "".join(f"{b:08b}" for b in rbsp[4:])  # after header, profile, flags, level
    pos = 0

    def ue() -> int:
        nonlocal pos
        zeros = 0
        while pos < len(bits) and bits[pos] == "0":
            zeros, pos = zeros + 1, pos + 1
        value = int(bits[pos:pos + zeros + 1], 2) - 1
        pos += zeros + 1
        return value

    if len(sps) < 5 or sps[1] not in (66, 77, 88):  # chroma fields come first in High
        return None
    try:
        ue()  # seq_parameter_set_id
        ue()  # log2_max_frame_num_minus4
        return ue()
    except ValueError:
        return None


class H264:
    """The phone's hardware stream: H264_CONFIG, then frames (format 3)."""

    def __init__(self) -> None:
        """No config yet."""
        self.config, self.frame_bytes, self.out = None, 0, None
        self.started, self.frames, self.bytes, self.biggest, self.idrs = False, 0, 0, 0, 0
        self.refused, self.files, self.t0 = 0, 0, time.monotonic()

    def status(self, code: int, ep_out) -> None:
        """Send H264_STATUS."""
        ep_out.write(frame(H264_STATUS, bytes([code])), timeout=2000)

    def on_config(self, payload: bytes, ep_out) -> None:
        """A new stream (or the same config again: nothing to do)."""
        try:
            fps, frame_bytes, sps_len = struct.unpack_from("<BIH", payload)
            sps = payload[7:7 + sps_len]
            (pps_len,) = struct.unpack_from("<H", payload, 7 + sps_len)
            pps = payload[9 + sps_len:9 + sps_len + pps_len]
        except struct.error:
            log("H264_CONFIG malformed: refused")
            self.status(H264_REFUSED, ep_out)
            return
        if payload == self.config:
            log("H264_CONFIG again, the same: nothing to do")
            self.status(H264_PLAYING, ep_out)
            return
        if not sps or not pps or not 0 < fps <= 30 or frame_bytes > H264_MAX_FRAME:
            log(f"H264_CONFIG refused: {fps} fps, frames {frame_bytes} B, SPS {len(sps)} / PPS {len(pps)} B")
            self.status(H264_REFUSED, ep_out)
            return
        profile, level = sps[1], sps[3]
        log(f"H264_CONFIG: stream at {fps} fps, frames up to {frame_bytes} B, SPS {len(sps)} B "
              f"(profile {profile}, level {level / 10:.1f}, POC type {sps_poc_type(sps)}), "
              f"PPS {len(pps)} B")
        if profile != 66 or level > 30:
            log("  (the MMI refuses this: Baseline 3.0 at most)")
        self.config, self.frame_bytes, self.started = payload, frame_bytes, False
        self.frames = self.bytes = self.biggest = self.idrs = self.refused = 0
        self.t0 = time.monotonic()
        if self.out:
            self.out.close()
            self.out = None
        if SAVE_DIR:
            self.files += 1
            name = os.path.join(SAVE_DIR, "stream.h264" if self.files == 1 else f"stream{self.files}.h264")
            self.out = open(name, "wb")
            # SPS and PPS first: the file plays on its own (ffplay, csmp4.py)
            self.out.write(b"".join(b"\0\0\0\1" + nal for nal in (sps, pps)))
            log(f"  saving to {name}")
        SCREEN.send("h264_config", b"".join(b"\0\0\0\1" + nal for nal in (sps, pps)))
        self.status(H264_PLAYING, ep_out)
        self.status(H264_SYNC, ep_out)

    def on_frame(self, data: bytes) -> int:
        """One frame (Annex-B); how it went on the MMI, for VIDEO_ACKS."""
        if self.config is None:
            return VIDEO_BAD
        nals = nal_units(data)
        idr = any(n[0] & 0x1F == 5 for n in nals)
        need = sum(4 + len(n) for n in nals if n[0] & 0x1F != 9)
        if not self.started and not idr:
            self.refused += 1
            return VIDEO_DROPPED
        if need > self.frame_bytes:
            log(f"frame of {need} B > {self.frame_bytes} B: refused (the MMI would ask for an IDR)")
            self.refused += 1
            self.started = False
            return VIDEO_BAD
        self.started = True
        self.frames += 1
        self.bytes += len(data)
        self.biggest = max(self.biggest, need)
        self.idrs += idr
        if self.out:
            self.out.write(data)
        if self.frames % 25 == 0 or self.frames <= 2:
            secs = max(time.monotonic() - self.t0, 0.001)
            log(f"H.264 frame #{self.frames}: {self.frames / secs:.1f} fps, "
                  f"{self.bytes * 8 / secs / 1000:.0f} kbit/s, avg {self.bytes // self.frames} B, "
                  f"max {self.biggest} B of {self.frame_bytes}, IDR {self.idrs}, refused {self.refused}")
        return VIDEO_SHOWN


class Pictures:
    """The phone's pictures: VIDEO_START + VIDEO_DATA... -> one JPEG / H.264 frame."""

    def __init__(self) -> None:
        """Nothing received yet."""
        self.cur, self.data, self.shown, self.t0 = None, b"", 0, time.monotonic()
        self.h264, self.gaps = H264(), FrameGaps()

    def start(self, payload: bytes, ep_out) -> None:
        """A new picture; refused like vasm.h does."""
        pid, w, h, fmt, size = struct.unpack_from("<HHHBI", payload)
        phone_ms = struct.unpack_from("<H", payload, 11)[0] if len(payload) >= 13 else 0
        box = (SCREEN_W // 2, SCREEN_H) if w < h else (SCREEN_W, SCREEN_H)
        if fmt == FMT_H264:
            box = (H264_MAX_W, H264_MAX_H)
        if (fmt not in EXT and fmt != FMT_H264) or not 0 < size <= MAX_BYTES or \
                not (0 < w <= box[0] and 0 < h <= box[1]):
            log(f"picture {pid} refused: {w}x{h} format {fmt} {size} bytes (box {box[0]}x{box[1]})")
            ep_out.write(ack_frame(VIDEO_BAD), timeout=2000)
            self.cur = None
            return
        self.cur, self.data = (pid, w, h, fmt, size, phone_ms), b""

    def data_chunk(self, payload: bytes, ep_out) -> None:
        """More of the current picture; ack it when complete."""
        if self.cur is None or struct.unpack_from("<H", payload)[0] != self.cur[0]:
            return
        self.data += payload[2:]
        pid, w, h, fmt, size, phone_ms = self.cur
        if len(self.data) < size:
            return
        self.cur = None
        if fmt == FMT_H264:
            ack = self.h264.on_frame(self.data)
            ep_out.write(ack_frame(ack), timeout=2000)
            if ack == VIDEO_SHOWN:
                self.gaps.frame(time.monotonic())
                SCREEN.send("h264", self.data)
            if ack == VIDEO_DROPPED or (ack == VIDEO_BAD and self.h264.config):
                self.h264.status(H264_SYNC, ep_out)
            elif ack == VIDEO_BAD:
                self.h264.status(H264_CONFIG_WANTED, ep_out)
            return
        self.shown += 1
        if SAVE_DIR:
            name = os.path.join(SAVE_DIR, f"latest.{EXT[fmt]}")
            with open(name + ".tmp", "wb") as f:
                f.write(self.data)
            os.replace(name + ".tmp", name)
        ep_out.write(ack_frame(VIDEO_SHOWN), timeout=2000)
        self.gaps.frame(time.monotonic())
        SCREEN.send("jpeg", self.data)
        if self.shown <= 3 or self.shown % 20 == 0:
            fps = self.shown / max(time.monotonic() - self.t0, 0.001)
            log(f"picture #{self.shown} id {pid}: {w}x{h} {'portrait' if w < h else 'landscape'}, "
                  f"{EXT[fmt]} {size} bytes (phone {phone_ms} ms), {fps:.1f} fps on average")


def handle(ftype: int, payload: bytes, ep_out, pics: Pictures) -> None:
    """React to one frame from the phone."""
    if ftype == HELLO:
        log(f"phone HELLO: {payload.decode(errors='replace')}")
        link_state("connected", payload.decode(errors="replace"))
        ep_out.write(frame(HELLO_REPLY, HELLO_TEXT), timeout=2000)
        ep_out.write(frame(VIDEO_CAPS, CAPS), timeout=2000)
    elif ftype == HELLO_REPLY:
        log(f"phone HELLO_REPLY: {payload.decode(errors='replace')}")
        link_state("connected", payload.decode(errors="replace"))
    elif ftype == VIDEO_START and len(payload) >= 11:
        pics.start(payload, ep_out)
    elif ftype == VIDEO_DATA and len(payload) >= 2:
        pics.data_chunk(payload, ep_out)
    elif ftype == VIDEO_STOP:
        log(f"VIDEO_STOP after {pics.shown} pictures, {pics.h264.frames} H.264 frames")
        pics.h264.config = None
        pics.gaps.ended("VIDEO_STOP from the phone", time.monotonic())
        SCREEN.send("stop", why="ended by VIDEO_STOP")
    elif ftype == H264_CONFIG:
        pics.h264.on_config(payload, ep_out)
    elif ftype == VIDEO_DETAILS and len(payload) >= 1:
        log(f"VIDEO_DETAILS: status bar over landscape {'on' if payload[0] else 'off'}")
        SCREEN.send("details", on=payload[0] != 0)
    else:
        log(f"frame type {ftype}, {len(payload)} bytes ignored")


def session(acc, may_reswitch: bool = True) -> bool:
    """Talk to the accessory until it goes away; True if it belonged to another app and was switched."""
    iface, ep_in, ep_out = open_endpoints(acc)
    log(f"accessory {acc.idVendor:04x}:{acc.idProduct:04x} open, IN 0x{ep_in.bEndpointAddress:02x} "
          f"OUT 0x{ep_out.bEndpointAddress:02x}; open Audi streaming on the phone")
    dec, pics, peer = Decoder(), Pictures(), Peer()
    link_state("accessory", "open Audi streaming on the phone")
    try:
        ep_out.write(frame(HELLO, HELLO_TEXT), timeout=2000)
        ep_out.write(frame(VIDEO_CAPS, CAPS), timeout=2000)
    except usb.core.USBTimeoutError:
        log("(HELLO not taken yet - the phone app is not open)")
    try:
        while True:
            try:
                data = bytes(ep_in.read(16384, timeout=1000))
            except usb.core.USBTimeoutError:
                pics.gaps.idle(time.monotonic())
                continue
            pics.gaps.idle(time.monotonic())
            oversized = dec.oversized
            frames = dec.feed(data)
            if may_reswitch and peer.foreign(dec.oversized - oversized, frames):
                log("not Audi streaming on the other end (Android Auto, left by make aa-host?): "
                    "switching the accessory to Audi streaming")
                reswitch(acc)
                return True
            for ftype, payload in frames:
                handle(ftype, payload, ep_out, pics)
    finally:
        if pics.gaps.last is not None:
            pics.gaps.ended("USB link down", time.monotonic())
        SCREEN.send("stop", why="ended by USB link down")
        try:
            usb.util.release_interface(acc, iface)
        except usb.core.USBError:
            pass  # gone, or reset
        usb.util.dispose_resources(acc)


def run_with_window() -> int | None:
    """The window here, USB in a child process; None if no window can open."""
    import aoa_screen  # noqa: PLC0415 - the USB process doesn't need av and pygame loaded

    try:
        screen = aoa_screen.Screen()
    except aoa_screen.NoDisplayError as e:
        log(f"no window ({e})")
        return None
    signal.signal(signal.SIGTERM, lambda *_: sys.exit(0))  # so that the worker is stopped too
    read_end, write_end = os.pipe()
    fcntl.fcntl(write_end, fcntl.F_SETPIPE_SZ, PIPE_BYTES)
    worker = subprocess.Popen([sys.executable, *sys.argv, "--screen-fd", str(write_end)],
                              pass_fds=(write_end,))
    os.close(write_end)
    model = aoa_screen.ScreenModel()
    threading.Thread(target=aoa_screen.pump, args=(read_messages(read_end), model),
                     daemon=True).start()
    try:
        return screen.run(model, lambda: worker.poll() is None)
    finally:
        worker.terminate()
        worker.wait()


def main() -> int:
    """Loop: wait for a phone, talk to it, start over when it drops."""
    if "--check" in sys.argv:          # image build: dependencies resolve, libusb loads
        import aoa_screen  # noqa: F401, PLC0415 - av and pygame load too

        log(f"ok, libusb backend: {usb.core.find(find_all=True) is not None}")
        return 0
    if SCREEN.fd is None and "--no-display" not in sys.argv:
        code = run_with_window()
        if code is not None:
            return code
    log("waiting for an Android phone on USB...")
    reswitches = 0
    while True:
        acc = wait_for_accessory()
        try:
            if session(acc, may_reswitch=reswitches < RESWITCH_TRIES):
                reswitches += 1
                if reswitches == RESWITCH_TRIES:
                    log("the accessory stays with another app: close it on the phone, or replug")
                time.sleep(2)
            else:
                reswitches = 0
        except usb.core.USBError as e:
            log(f"link down: {e}; waiting again")
            time.sleep(1)


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        sys.exit(0)
