# /// script
# requires-python = ">=3.12"
# dependencies = ["pyusb==1.3.1"]
# ///
"""aoa_host.py - a desk stand-in for the MMI side of audi_stream_receiver.

Does what src/audi_stream_receiver/aoa.c does, with libusb on a PC: switches a USB
Android phone into accessory mode (AOA), then speaks the audi_stream_receiver
protocol (src/audi_stream_receiver/proto.h): HELLO / HELLO_REPLY, then the phone's
screen. Replug the phone at will; it goes back to waiting, like the MMI
receiver. Ctrl-C to stop.

Video: announces the MMI's boxes (VIDEO_CAPS, 800x480 screen, as
src/audi_stream_receiver/video.c), reassembles the phone's pictures, acks each as
shown and, with --save DIR, keeps the newest one as DIR/latest.jpg or
DIR/latest.png (the phone app's format setting).

Run with `make aoa-host` (Docker, USB passthrough, saves into
build/aoa-host/) or `uv run tools/aoa_host.py [--save DIR]` (needs access to
/dev/bus/usb). Stop adb first if it holds the phone.
"""

import os
import struct
import sys
import time

import usb.core
import usb.util

MANUFACTURER, MODEL = "mmi3g", "audi_stream_receiver"  # = aoa.h, matched by accessory_filter.xml
STRINGS = [MANUFACTURER, MODEL, "desk aoa_host.py", "1", "https://github.com/dspl1236/MMI3G-Toolkit",
           "desk-0001"]
HELLO_TEXT = b"audi_stream_receiver-desk/1"

AOA_VID, AOA_PIDS = 0x18D1, range(0x2D00, 0x2D06)
HELLO, HELLO_REPLY = 1, 4  # 2, 3: TEXT / ACK, retired 2026-10-25
VIDEO_CAPS, VIDEO_START, VIDEO_DATA, VIDEO_ACK, VIDEO_STOP, VIDEO_DETAILS = 5, 6, 7, 8, 9, 10
VIDEO_SHOWN, VIDEO_BAD = 0, 1
HDR, MAX_PAYLOAD = 3, 2048
SCREEN_W, SCREEN_H, MAX_BYTES = 800, 480, 512 * 1024
FMT_JPEG, FMT_PNG = 1, 2
CAPS = struct.pack("<HHHHIB", SCREEN_W, SCREEN_H, SCREEN_W // 2, SCREEN_H, MAX_BYTES,
                   1 << (FMT_JPEG - 1) | 1 << (FMT_PNG - 1))
EXT = {FMT_JPEG: "jpg", FMT_PNG: "png"}
SAVE_DIR = sys.argv[sys.argv.index("--save") + 1] if "--save" in sys.argv else None


def frame(ftype: int, payload: bytes = b"") -> bytes:
    """Encode one protocol frame."""
    return struct.pack("<BH", ftype, len(payload)) + payload


class Decoder:
    """Reassembles frames from arbitrary chunks; skips oversized ones."""

    def __init__(self) -> None:
        """Start with an empty buffer."""
        self.buf, self.skip = b"", 0

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
                print(f"  oversized frame ({n} bytes) skipped")
                self.buf, self.skip = self.buf[HDR:], n
                continue
            if len(self.buf) < HDR + n:
                return out
            out.append((ftype, self.buf[HDR:HDR + n]))
            self.buf = self.buf[HDR + n:]


def find_accessory():
    """The phone already in accessory mode, or None."""
    return usb.core.find(custom_match=lambda d: d.idVendor == AOA_VID and d.idProduct in AOA_PIDS)


def try_switch(dev) -> bool:
    """AOA handshake on one device; True if START was sent."""
    try:
        ver = dev.ctrl_transfer(0xC0, 51, 0, 0, 2, timeout=1000)
    except usb.core.USBError:
        return False
    version = ver[0] | ver[1] << 8 if len(ver) == 2 else 0
    if version < 1:
        return False
    print(f"{dev.idVendor:04x}:{dev.idProduct:04x} speaks AOA v{version}, switching")
    for i, s in enumerate(STRINGS):
        dev.ctrl_transfer(0x40, 52, 0, i, s.encode() + b"\0", timeout=1000)
    dev.ctrl_transfer(0x40, 53, 0, 0, None, timeout=1000)
    return True


def wait_for_accessory():
    """Switch any Android device to accessory mode and return it."""
    tried = set()
    while True:
        acc = find_accessory()
        if acc is not None:
            return acc
        for dev in usb.core.find(find_all=True):
            key = (dev.bus, dev.address)
            if dev.bDeviceClass == 9 or key in tried:
                continue
            tried.add(key)
            if try_switch(dev):
                break
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


class Pictures:
    """The phone's pictures: VIDEO_START + VIDEO_DATA... -> one JPEG / PNG."""

    def __init__(self) -> None:
        """Nothing received yet."""
        self.cur, self.data, self.shown, self.t0 = None, b"", 0, time.monotonic()

    def start(self, payload: bytes, ep_out) -> None:
        """A new picture; refused like vasm.c does."""
        pid, w, h, fmt, size = struct.unpack_from("<HHHBI", payload)
        phone_ms = struct.unpack_from("<H", payload, 11)[0] if len(payload) >= 13 else 0
        box = (SCREEN_W // 2, SCREEN_H) if w < h else (SCREEN_W, SCREEN_H)
        if fmt not in EXT or not 0 < size <= MAX_BYTES or not (0 < w <= box[0] and 0 < h <= box[1]):
            print(f"picture {pid} refused: {w}x{h} format {fmt} {size} bytes (box {box[0]}x{box[1]})")
            ep_out.write(frame(VIDEO_ACK, struct.pack("<HBH", pid, VIDEO_BAD, 0)), timeout=2000)
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
        self.shown += 1
        if SAVE_DIR:
            name = os.path.join(SAVE_DIR, f"latest.{EXT[fmt]}")
            with open(name + ".tmp", "wb") as f:
                f.write(self.data)
            os.replace(name + ".tmp", name)
        ep_out.write(frame(VIDEO_ACK, struct.pack("<HBH", pid, VIDEO_SHOWN, 0)), timeout=2000)
        if self.shown <= 3 or self.shown % 20 == 0:
            fps = self.shown / max(time.monotonic() - self.t0, 0.001)
            print(f"picture #{self.shown} id {pid}: {w}x{h} {'portrait' if w < h else 'landscape'}, "
                  f"{EXT[fmt]} {size} bytes (phone {phone_ms} ms), {fps:.1f} fps on average")


def handle(ftype: int, payload: bytes, ep_out, pics: Pictures) -> None:
    """React to one frame from the phone."""
    if ftype == HELLO:
        print(f"phone HELLO: {payload.decode(errors='replace')}")
        ep_out.write(frame(HELLO_REPLY, HELLO_TEXT), timeout=2000)
        ep_out.write(frame(VIDEO_CAPS, CAPS), timeout=2000)
    elif ftype == HELLO_REPLY:
        print(f"phone HELLO_REPLY: {payload.decode(errors='replace')}")
    elif ftype == VIDEO_START and len(payload) >= 11:
        pics.start(payload, ep_out)
    elif ftype == VIDEO_DATA and len(payload) >= 2:
        pics.data_chunk(payload, ep_out)
    elif ftype == VIDEO_STOP:
        print(f"VIDEO_STOP after {pics.shown} pictures")
    elif ftype == VIDEO_DETAILS and len(payload) >= 1:
        print(f"VIDEO_DETAILS: status bar over landscape {'on' if payload[0] else 'off'}")
    else:
        print(f"frame type {ftype}, {len(payload)} bytes ignored")


def session(acc) -> None:
    """Talk to the accessory until it goes away."""
    iface, ep_in, ep_out = open_endpoints(acc)
    print(f"accessory {acc.idVendor:04x}:{acc.idProduct:04x} open, IN 0x{ep_in.bEndpointAddress:02x} "
          f"OUT 0x{ep_out.bEndpointAddress:02x}; open Audi streaming on the phone")
    dec, pics = Decoder(), Pictures()
    try:
        ep_out.write(frame(HELLO, HELLO_TEXT), timeout=2000)
        ep_out.write(frame(VIDEO_CAPS, CAPS), timeout=2000)
    except usb.core.USBTimeoutError:
        print("(HELLO not taken yet - the phone app is not open)")
    try:
        while True:
            try:
                data = bytes(ep_in.read(16384, timeout=1000))
            except usb.core.USBTimeoutError:
                continue
            for ftype, payload in dec.feed(data):
                handle(ftype, payload, ep_out, pics)
    finally:
        usb.util.release_interface(acc, iface)
        usb.util.dispose_resources(acc)


def main() -> int:
    """Loop: wait for a phone, talk to it, start over when it drops."""
    if "--check" in sys.argv:          # image build: dependencies resolve, libusb loads
        print(f"ok, libusb backend: {usb.core.find(find_all=True) is not None}")
        return 0
    print("waiting for an Android phone on USB...")
    while True:
        acc = wait_for_accessory()
        try:
            session(acc)
        except usb.core.USBError as e:
            print(f"link down: {e}; waiting again")
            time.sleep(1)


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        sys.exit(0)
