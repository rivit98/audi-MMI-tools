"""aoa_sim.py - MMI faults that tools/aoa_host.py plays to the phone (the window's toolbar).

aoa_host.py decodes on a PC, so the MMI's own limits never show there. This module
puts them back, as the phone sees them:

    AckGate        acks held back for HOLD_S, or let out at SLOW_ACK_FPS a second
    IoMediaModel   io-media reading the MMI's CS-MP4: every frame padded to S, read at
                   READ_BYTES_S. Too slow for the frames, so the reader falls behind.
                   Past the ring: the io-media error (the MMI sends REFUSED). With the
                   catch-up on, the MMI drops frames until io-media is back, then asks
                   for an IDR (src/common/live_stream.h).

Pure: the caller passes the time; tests/test_aoa_sim.py.
"""

from collections import deque
from dataclasses import dataclass, field

HOLD_S = 3.0
SLOW_ACK_FPS = 5
STALL_S = 3.0

# = src/audi_stream_receiver/hwvideo.cpp, src/common/csmp4.h
FPS = 25
RING_BYTES = 8 << 20
CATCH_UP_FRAMES = 12
FILLER_MIN = 6
# What io-media read on the car: ~24 frames a second of 260 KB (266240 B)
READ_BYTES_S = 6_400_000

ACTIONS = ("refuse", "sync", "config", "hold_acks", "stall_usb", "drop_link")
TOGGLES = ("slow_acks", "iomedia", "catch_up")


@dataclass
class Controls:
    """The toolbar's switches, as the window last sent them."""

    slow_acks: bool = False
    iomedia: bool = False
    catch_up: bool = True

    def update(self, toggles: dict) -> None:
        """Take the window's switches; unknown names are ignored."""
        for name in TOGGLES:
            if name in toggles:
                setattr(self, name, bool(toggles[name]))


@dataclass
class AckGate:
    """Acks to the phone: at once, held back after hold(), or SLOW_ACK_FPS a second."""

    slow: bool = False
    hold_until: float = 0.0
    next_at: float = 0.0
    queue: deque = field(default_factory=deque)

    def hold(self, now: float, secs: float = HOLD_S) -> None:
        """Nothing goes out for secs."""
        self.hold_until = now + secs

    def add(self, ack: bytes, now: float) -> list[bytes]:
        """One more ack; what may go out now."""
        self.queue.append(ack)
        return self.due(now)

    def due(self, now: float) -> list[bytes]:
        """The acks that may go out now, in order."""
        if now < self.hold_until or not self.queue:
            return []
        if not self.slow:
            out = list(self.queue)
            self.queue.clear()
            return out
        if now < self.next_at:
            return []
        self.next_at = max(self.next_at + 1 / SLOW_ACK_FPS, now - 1 / SLOW_ACK_FPS)
        return [self.queue.popleft()]

    def held(self) -> int:
        """Acks waiting."""
        return len(self.queue)


def sample_size(frame_bytes: int) -> int:
    """The MMI's padded frame for a config's frame size (hwvideo.cpp setup)."""
    s = max(frame_bytes + FILLER_MIN, 8192)
    return (s + 4095) // 4096 * 4096


class IoMediaModel:
    """io-media reading the MMI's stream: S bytes per frame at read_bytes_s.

    The stream grows by the phone's frames, plus skip frames up to FPS a second
    (the MMI's gap fill), and both cost io-media S bytes. The reader can't pass
    the newest frame.
    """

    def __init__(self, frame_bytes: int, now: float, catch_up: bool = True,
                 read_bytes_s: int = READ_BYTES_S, started: bool = False) -> None:
        """A new stream; started: it is under way already (the switch turned on mid-stream)."""
        self.s = sample_size(frame_bytes)
        self.ring = min(128, max(16, RING_BYTES // self.s))
        self.catch_up_at = min(CATCH_UP_FRAMES, self.ring // 2)
        self.catch_up, self.read_bytes_s = catch_up, read_bytes_s
        self.frames, self.reader, self.last = 0, 0.0, now
        self.t0 = now if started else None
        self.catching = self.caught = False
        self.catch_ups = self.dropped = 0
        self.behind_max = 0

    def frame(self, idr: bool, now: float) -> str:
        """A frame the MMI took: "ok", "catch_up" (dropped) or "wait_idr" (dropped, SYNC)."""
        self.tick(now)
        if self.t0 is None:
            if not idr:
                return "wait_idr"
            self.t0 = now
        if self.catching and not self.caught:
            self.dropped += 1
            return "catch_up"
        if self.catching:
            if not idr:
                self.dropped += 1
                return "wait_idr"
            self.t0 = now - self.frames / FPS  # the clock goes on from this IDR
            self.catching = self.caught = False
        self.frames += 1
        return "ok"

    def behind(self) -> int:
        """Frames between io-media's read position and the newest one."""
        return self.frames - int(self.reader)

    def tick(self, now: float) -> str | None:
        """Time passes: "catch_up" (it started), "sync" (io-media is back: ask for an IDR),
        "eio" (io-media read past the ring: the error) or None."""
        dt, self.last = max(now - self.last, 0.0), now
        if self.t0 is not None and not self.catching:
            due = int((now - self.t0) * FPS) + 1
            self.frames = max(self.frames, due - 1)  # the gap fill's skips
        self.reader = min(self.reader + dt * self.read_bytes_s / self.s, float(self.frames))
        behind = self.behind()
        self.behind_max = max(self.behind_max, behind)
        if self.catch_up and not self.catching and behind > self.catch_up_at:
            self.catching, self.caught = True, False
            self.catch_ups += 1
            return "catch_up"
        if self.catching and not self.caught and behind <= 1:
            self.caught = True
            return "sync"
        if behind > self.ring:
            return "eio"
        return None

    def describe(self) -> str:
        """One line for the toolbar."""
        state = " catching up" if self.catching else ""
        return (f"io-media {self.s // 1024} KB frames, {self.behind()}/{self.ring} behind"
                f"{state}, {self.catch_ups} catch-ups")
