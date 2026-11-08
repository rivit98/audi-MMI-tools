"""aoa_screen.py - the window of tools/aoa_host.py: an 800x480 stand-in for the MMI's screen.

Lays things out like src/audi_stream_receiver/ui.cpp:

    H.264              the decoded frame stretched to 800x480, as the VCAP does
    JPEG, landscape    centred on black; the status bar on top if the phone asks for it
    JPEG, portrait     on the left at full height, the status panel in the rest
    no picture         the status panel, full width, with what to do next

Over that goes a strip the MMI doesn't have, toggled with D: picture rate, time since
the last frame (red from FREEZE_S on) and why the last stream ended. So a stream that
froze keeps its picture and turns the strip red; one that was stopped goes to the panel.

Messages come from aoa_host.py's USB process and are decoded on a thread that keeps
only the newest picture. Q or Esc closes the window.
"""

import copy
import io
import threading
import time
from collections import deque
from collections.abc import Iterable
from dataclasses import dataclass, field

import av
import pygame

SCREEN_W, SCREEN_H = 800, 480
FREEZE_S = 1.0
FPS_WINDOW_S = 2.0
REFRESH_HZ = 30

STATUS_H, DETAIL_Y, BODY_TOP, FOOTER_H, MARGIN, TOPBAR_H = 48, 54, 80, 24, 12, 28  # = ui.cpp, video.h
STRIP_H = 22
C_BG, C_TEXT, C_DIM = (0x10, 0x18, 0x20), (0xE8, 0xE8, 0xE8), (0x8A, 0x92, 0x9E)
C_FOOTER_BG, C_RULE, C_VIDEO_BG = (0x1C, 0x28, 0x36), (0x2A, 0x36, 0x44), (0, 0, 0)
C_STRIP, C_STRIP_FROZEN = (0x20, 0x20, 0x20, 200), (0xC0, 0x20, 0x20, 230)

# Link state: colour, title, what to do next (= ui.cpp).
LINKS = {
    "waiting": ((0x3A, 0x44, 0x52), "Waiting for phone",
                "Plug the phone into the car's USB port."),
    "switching": ((0xB0, 0x7A, 0x10), "Phone found, switching...",
                  "The phone is switching to accessory mode. If it asks, let Audi streaming open."),
    "accessory": ((0x2F, 0x6F, 0xA8), "Phone connected, app not open",
                  "Open Audi streaming on the phone."),
    "connected": ((0x2E, 0x8E, 0x4A), "Connected",
                  'Tap "Stream screen" on the phone and allow the screen capture. '
                  "The phone's screen appears here."),
}


class NoDisplayError(Exception):
    """There is nothing to open a window on."""


class H264Decoder:
    """One H.264 stream -> RGB pictures of the whole screen."""

    def __init__(self, config: bytes) -> None:
        """A new stream; config is its SPS and PPS as Annex-B."""
        self.codec = av.CodecContext.create("h264", "r")
        self.codec.thread_count = 1  # threads would hold frames back
        self.codec.options = {"flags": "low_delay"}
        self.pending = config

    def decode(self, data: bytes) -> pygame.Surface | None:
        """One frame (Annex-B) -> its picture at 800x480, or None if it gave none."""
        packet = av.Packet(self.pending + data)
        self.pending = b""
        frames = self.codec.decode(packet)
        return to_surface(frames[-1]) if frames else None


def to_surface(frame) -> pygame.Surface:
    """A decoded frame stretched to the screen."""
    rgb = frame.reformat(SCREEN_W, SCREEN_H, "rgb24", interpolation="BILINEAR")
    plane, row = rgb.planes[0], SCREEN_W * 3
    data = bytes(plane)
    if plane.line_size != row:
        data = b"".join(data[y * plane.line_size:y * plane.line_size + row]
                        for y in range(SCREEN_H))
    return pygame.image.frombuffer(data, (SCREEN_W, SCREEN_H), "RGB")


@dataclass
class ScreenState:
    """What is on the screen; aoa_host.py's messages change it."""

    link: str = "waiting"
    detail: str = ""
    details_bar: bool = False
    mode: str | None = None  # "h264", "jpeg" or None for no picture
    picture: pygame.Surface | None = None
    size: tuple[int, int] = (0, 0)
    bytes: int = 0
    shown: int = 0
    last_frame: float | None = None
    ended: str = ""
    times: deque = field(default_factory=lambda: deque(maxlen=128))

    def fps(self, now: float) -> float:
        """Pictures a second lately."""
        return sum(now - t < FPS_WINDOW_S for t in self.times) / FPS_WINDOW_S


class ScreenModel:
    """Applies aoa_host.py's messages to a ScreenState."""

    def __init__(self) -> None:
        """Nothing connected yet."""
        self.state = ScreenState()
        self.decoder: H264Decoder | None = None
        self.lock = threading.Lock()  # the window's thread reads what the reader's thread writes

    def apply(self, head: dict, body: bytes) -> None:
        """Handle one message; a picture that doesn't decode leaves the old one up."""
        try:
            getattr(self, f"on_{head['kind']}")(head, body)
        except (av.error.FFmpegError, pygame.error) as e:
            print(f"screen: {head['kind']} not decoded: {e}", flush=True)

    def on_link(self, head: dict, _body: bytes) -> None:
        """The link state changed."""
        with self.lock:
            self.state.link, self.state.detail = head["state"], head["detail"]

    def on_details(self, head: dict, _body: bytes) -> None:
        """VIDEO_DETAILS: the status bar over a landscape picture."""
        with self.lock:
            self.state.details_bar = head["on"]

    def on_h264_config(self, _head: dict, body: bytes) -> None:
        """A new stream: black until its first frame after H.264, else the panel until then."""
        self.decoder = H264Decoder(body)
        with self.lock:
            self.state.mode = "h264" if self.state.mode == "h264" else None
            self.state.picture, self.state.ended = None, ""

    def on_h264(self, _head: dict, body: bytes) -> None:
        """One accepted frame."""
        if self.decoder is None:
            return
        picture = self.decoder.decode(body)
        if picture is not None:
            self.show("h264", picture, picture.get_size(), len(body))

    def on_jpeg(self, _head: dict, body: bytes) -> None:
        """One accepted picture, drawn 1:1."""
        picture = pygame.image.load(io.BytesIO(body), "picture.jpg")
        self.show("jpeg", picture, picture.get_size(), len(body))

    def on_stop(self, head: dict, _body: bytes) -> None:
        """The picture ends; back to the status panel."""
        self.decoder = None
        with self.lock:
            self.state.mode, self.state.picture, self.state.ended = None, None, head["why"]
            self.state.times.clear()

    def show(self, mode: str, picture: pygame.Surface, size: tuple[int, int], nbytes: int) -> None:
        """Put a new picture on the screen."""
        now = time.monotonic()
        with self.lock:
            state = self.state
            state.mode, state.picture, state.size, state.bytes = mode, picture, size, nbytes
            state.shown, state.last_frame, state.ended = state.shown + 1, now, ""
            state.times.append(now)

    def snapshot(self) -> ScreenState:
        """A consistent copy for one redraw; surfaces are replaced, never drawn on."""
        with self.lock:
            state = copy.copy(self.state)
            state.times = self.state.times.copy()
        return state


def wrap(font: pygame.font.Font, text: str, width: int) -> list[str]:
    """Word-wrap text to width pixels."""
    lines, line = [], ""
    for word in text.split():
        candidate = f"{line} {word}".strip()
        if line and font.size(candidate)[0] > width:
            lines.append(line)
            line = word
        else:
            line = candidate
    return [*lines, line] if line else lines


class Painter:
    """Draws a ScreenState onto a surface of the screen's size."""

    def __init__(self, target: pygame.Surface) -> None:
        """Fonts need pygame.font.init()."""
        self.target = target
        self.big, self.small = pygame.font.Font(None, 32), pygame.font.Font(None, 20)
        self.started = time.monotonic()

    def text(self, font: pygame.font.Font, pos: tuple[int, int], text: str, colour) -> None:
        """One line of text."""
        self.target.blit(font.render(text, True, colour), pos)

    def paint(self, s: ScreenState, now: float, strip: bool = True) -> None:
        """The whole screen."""
        if s.mode == "h264":
            self.target.fill(C_VIDEO_BG)
            if s.picture is not None:
                self.target.blit(s.picture, (0, 0))
        elif s.mode == "jpeg" and s.size[0] < s.size[1]:
            self.portrait(s, now)
        elif s.mode == "jpeg":
            self.landscape(s, now)
        else:
            self.target.fill(C_BG)
            self.panel(s, 0, SCREEN_W, now)
        if strip:
            self.strip(s, now)

    def landscape(self, s: ScreenState, now: float) -> None:
        """A landscape picture centred, the status bar on top if asked for."""
        self.target.fill(C_VIDEO_BG)
        self.target.blit(s.picture, ((SCREEN_W - s.size[0]) // 2, (SCREEN_H - s.size[1]) // 2))
        if s.details_bar:
            colour, title, _ = LINKS[s.link]
            self.target.fill(colour, (0, 0, SCREEN_W, TOPBAR_H))
            self.text(self.small, (MARGIN, 7), f"{title}  {s.detail}", C_TEXT)
            stats = f"video {s.fps(now):.1f} fps  {s.size[0]}x{s.size[1]}  {s.bytes // 1024} KB"
            x = SCREEN_W - MARGIN - self.small.size(stats)[0]
            self.target.fill(colour, (x - MARGIN, 0, SCREEN_W - x + MARGIN, TOPBAR_H))
            self.text(self.small, (x, 7), stats, C_TEXT)

    def portrait(self, s: ScreenState, now: float) -> None:
        """A portrait picture on the left, the status panel in the rest."""
        col = s.size[0]
        self.target.fill(C_BG)
        self.target.fill(C_VIDEO_BG, (0, 0, col, SCREEN_H))
        self.target.blit(s.picture, (0, (SCREEN_H - s.size[1]) // 2))
        self.target.fill(C_RULE, (col, 0, 1, SCREEN_H))
        self.panel(s, col + 1, SCREEN_W - col - 1, now)

    def panel(self, s: ScreenState, x0: int, width: int, now: float) -> None:
        """The status panel: status bar, detail, video stats or what to do next, footer."""
        colour, title, hint = LINKS[s.link]
        self.target.fill(colour, (x0, 0, width, STATUS_H))
        self.text(self.big, (x0 + MARGIN, (STATUS_H - self.big.get_height()) // 2), title, C_TEXT)
        self.text(self.small, (x0 + MARGIN, DETAIL_Y), s.detail, C_DIM)
        self.target.fill(C_RULE, (x0, BODY_TOP - 4, width, 1))
        if s.mode:
            lines = [f"picture {s.size[0]}x{s.size[1]}, {s.bytes // 1024} KB {s.mode.upper()}",
                     f"{s.fps(now):.1f} pictures a second", f"shown {s.shown}"]
            for i, line in enumerate(lines):
                self.text(self.small, (x0 + MARGIN, BODY_TOP + 8 + i * 22), line, C_TEXT)
        else:
            for i, line in enumerate(wrap(self.big, hint, width - 2 * MARGIN)):
                self.text(self.big, (x0 + MARGIN, BODY_TOP + 24 + i * self.big.get_height()),
                          line, C_TEXT)
        self.target.fill(C_FOOTER_BG, (x0, SCREEN_H - FOOTER_H, width, FOOTER_H))
        up = int(now - self.started)
        self.text(self.small, (x0 + MARGIN, SCREEN_H - FOOTER_H + 5),
                  f"up {up // 3600:02}:{up // 60 % 60:02}:{up % 60:02}  desk aoa_host.py", C_DIM)

    def strip(self, s: ScreenState, now: float) -> None:
        """The desk's own line, above where the footer goes."""
        age = None if s.last_frame is None else now - s.last_frame
        frozen = s.mode is not None and age is not None and age >= FREEZE_S
        ago = "none yet" if age is None else f"{age:.1f} s ago"
        parts = [f"{s.mode.upper()} {s.fps(now):.1f} fps" if s.mode else "no picture",
                 f"last frame {ago}"]
        if frozen:
            parts.insert(0, "FROZEN")
        if s.ended:
            parts.append(s.ended)
        band = pygame.Surface((SCREEN_W, STRIP_H), pygame.SRCALPHA)
        band.fill(C_STRIP_FROZEN if frozen else C_STRIP)
        self.target.blit(band, (0, SCREEN_H - FOOTER_H - STRIP_H))
        self.text(self.small, (MARGIN, SCREEN_H - FOOTER_H - STRIP_H + 4), "  |  ".join(parts),
                  C_TEXT)


class Screen:
    """The window."""

    def __init__(self) -> None:
        """Open it; NoDisplayError if there is nothing to open it on."""
        try:
            pygame.display.init()
            pygame.font.init()
            self.window = pygame.display.set_mode((SCREEN_W, SCREEN_H))
        except pygame.error as e:
            raise NoDisplayError(str(e)) from e
        if pygame.display.get_driver() in ("dummy", "offscreen"):
            raise NoDisplayError(f"SDL driver {pygame.display.get_driver()}")
        pygame.display.set_caption("MMI screen (aoa_host.py)   D: desk strip   Q: quit")

    def run(self, model: ScreenModel, worker_alive) -> int:
        """Redraw until the window is closed or worker_alive() turns false."""
        painter, clock, strip = Painter(self.window), pygame.time.Clock(), True
        while worker_alive():
            for event in pygame.event.get():
                if event.type == pygame.QUIT:
                    return 0
                if event.type == pygame.KEYDOWN and event.key in (pygame.K_q, pygame.K_ESCAPE):
                    return 0
                if event.type == pygame.KEYDOWN and event.key == pygame.K_d:
                    strip = not strip
            painter.paint(model.snapshot(), time.monotonic(), strip)
            pygame.display.flip()
            clock.tick(REFRESH_HZ)
        return 0


def pump(messages: Iterable[tuple[dict, bytes]], model: ScreenModel) -> None:
    """Apply messages until they end; runs on its own thread."""
    for head, body in messages:
        model.apply(head, body)
