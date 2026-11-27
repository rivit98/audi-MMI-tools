"""aoa_screen.py - the window of tools/aoa_host.py: an 800x480 stand-in for the MMI's screen.

Lays things out like src/audi_stream_receiver/ui.cpp:

    H.264              the decoded frame stretched to 800x480, as the VCAP does
    no picture         the status panel, full width, with what to do next

Messages come from aoa_host.py's USB process and are decoded on a thread that keeps
only the newest picture. Q or Esc closes the window.

Under the screen, the toolbar: the MMI's faults (tools/aoa_sim.py), by click or keys
1-9, sent to the USB process as JSON lines; switches are lit while on, and sent
again every second (the USB process restarts when the bus changes). Its status
line is the USB process's "sim" message.
"""

import copy
import threading
import time
from collections.abc import Callable, Iterable
from dataclasses import dataclass

import av
import pygame

SCREEN_W, SCREEN_H = 800, 480
REFRESH_HZ = 30
TOOLBAR_H, BUTTON_H, BUTTON_GAP = 60, 28, 4
TOGGLES_EVERY_S = 1.0
C_BUTTON, C_BUTTON_ON = (0x2A, 0x36, 0x44), (0xB0, 0x7A, 0x10)

# The toolbar: (name, label, is a switch); = aoa_sim.ACTIONS / TOGGLES
BUTTONS = [("refuse", "Refuse", False), ("sync", "Sync", False), ("config", "Config", False),
           ("hold_acks", "Hold acks", False), ("stall_usb", "Stall USB", False),
           ("drop_link", "Drop link", False), ("slow_acks", "Slow acks", True),
           ("iomedia", "io-media", True), ("catch_up", "Catch-up", True)]

STATUS_H, DETAIL_Y, BODY_TOP, FOOTER_H, MARGIN = 48, 54, 80, 24, 12  # = ui.cpp
C_BG, C_TEXT, C_DIM = (0x10, 0x18, 0x20), (0xE8, 0xE8, 0xE8), (0x8A, 0x92, 0x9E)
C_FOOTER_BG, C_RULE, C_VIDEO_BG = (0x1C, 0x28, 0x36), (0x2A, 0x36, 0x44), (0, 0, 0)

# Link state: colour, title, what to do next (= ui.cpp).
LINKS = {
    "waiting": ((0x3A, 0x44, 0x52), "Waiting for phone",
                "Plug the phone into the car's USB port."),
    "switching": ((0xB0, 0x7A, 0x10), "Phone found, switching...",
                  "The phone is switching to accessory mode. If it asks, let Audi streaming open."),
    "accessory": ((0x2F, 0x6F, 0xA8), "Phone connected, app not open",
                  "Open Audi streaming on the phone."),
    "connected": ((0x2E, 0x8E, 0x4A), "Connected",
                  "Start Android Auto's head unit server on the phone. "
                  "Android Auto appears here."),
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
    mode: str | None = None  # "h264" or None for no picture
    picture: pygame.Surface | None = None
    shown: int = 0
    sim: str = ""  # the toolbar's status line


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

    def on_h264_config(self, _head: dict, body: bytes) -> None:
        """A new stream: black until its first frame after H.264, else the panel until then."""
        self.decoder = H264Decoder(body)
        with self.lock:
            self.state.mode = "h264" if self.state.mode == "h264" else None
            self.state.picture = None

    def on_h264(self, _head: dict, body: bytes) -> None:
        """One accepted frame."""
        if self.decoder is None:
            return
        picture = self.decoder.decode(body)
        if picture is not None:
            self.show(picture)

    def on_sim(self, head: dict, _body: bytes) -> None:
        """The faults' state, for the toolbar's status line."""
        with self.lock:
            self.state.sim = head["text"]

    def on_stop(self, _head: dict, _body: bytes) -> None:
        """The picture ends; back to the status panel."""
        self.decoder = None
        with self.lock:
            self.state.mode, self.state.picture = None, None

    def show(self, picture: pygame.Surface) -> None:
        """Put a new decoded frame on the screen."""
        with self.lock:
            self.state.mode, self.state.picture = "h264", picture
            self.state.shown += 1

    def snapshot(self) -> ScreenState:
        """A consistent copy for one redraw; surfaces are replaced, never drawn on."""
        with self.lock:
            return copy.copy(self.state)


class Toolbar:
    """The fault buttons under the screen: what a click or key sends to the USB process."""

    def __init__(self) -> None:
        """Switches as aoa_sim.Controls starts them."""
        self.toggles = {name: name == "catch_up" for name, _, switch in BUTTONS if switch}
        self.sent_at = 0.0

    @staticmethod
    def rects() -> list[pygame.Rect]:
        """Each button's place in the window."""
        width = (SCREEN_W - BUTTON_GAP) // len(BUTTONS)
        return [pygame.Rect(BUTTON_GAP + i * width, SCREEN_H + BUTTON_GAP, width - BUTTON_GAP,
                            BUTTON_H) for i in range(len(BUTTONS))]

    def press(self, index: int) -> dict:
        """Button index pressed: the command for the USB process."""
        name, _, switch = BUTTONS[index]
        if switch:
            self.toggles[name] = not self.toggles[name]
            return {"toggles": dict(self.toggles)}
        return {"action": name, "toggles": dict(self.toggles)}

    def click(self, pos: tuple[int, int]) -> dict | None:
        """A mouse click: the command, or None off the buttons."""
        for i, rect in enumerate(self.rects()):
            if rect.collidepoint(pos):
                return self.press(i)
        return None

    def key(self, key: int) -> dict | None:
        """Keys 1-9 press the buttons."""
        index = key - pygame.K_1
        return self.press(index) if 0 <= index < len(BUTTONS) else None

    def heartbeat(self, now: float) -> dict | None:
        """The switches again, every TOGGLES_EVERY_S."""
        if now - self.sent_at < TOGGLES_EVERY_S:
            return None
        self.sent_at = now
        return {"toggles": dict(self.toggles)}

    def paint(self, target: pygame.Surface, font: pygame.font.Font, status: str) -> None:
        """The buttons and the status line, below the screen."""
        target.fill(C_FOOTER_BG, (0, SCREEN_H, SCREEN_W, TOOLBAR_H))
        for i, (rect, (name, label, switch)) in enumerate(zip(self.rects(), BUTTONS, strict=True)):
            on = switch and self.toggles[name]
            target.fill(C_BUTTON_ON if on else C_BUTTON, rect)
            text = font.render(f"{i + 1} {label}", True, C_TEXT)
            target.blit(text, text.get_rect(center=rect.center))
        target.blit(font.render(status, True, C_DIM),
                    (MARGIN, SCREEN_H + BUTTON_GAP + BUTTON_H + 6))


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

    def paint(self, s: ScreenState, now: float) -> None:
        """The whole screen."""
        if s.mode == "h264":
            self.target.fill(C_VIDEO_BG)
            if s.picture is not None:
                self.target.blit(s.picture, (0, 0))
        else:
            self.target.fill(C_BG)
            self.panel(s, 0, SCREEN_W, now)

    def panel(self, s: ScreenState, x0: int, width: int, now: float) -> None:
        """The status panel: status bar, detail, what to do next, footer."""
        colour, title, hint = LINKS[s.link]
        self.target.fill(colour, (x0, 0, width, STATUS_H))
        self.text(self.big, (x0 + MARGIN, (STATUS_H - self.big.get_height()) // 2), title, C_TEXT)
        self.text(self.small, (x0 + MARGIN, DETAIL_Y), s.detail, C_DIM)
        self.target.fill(C_RULE, (x0, BODY_TOP - 4, width, 1))
        for i, line in enumerate(wrap(self.big, hint, width - 2 * MARGIN)):
            self.text(self.big, (x0 + MARGIN, BODY_TOP + 24 + i * self.big.get_height()),
                      line, C_TEXT)
        self.target.fill(C_FOOTER_BG, (x0, SCREEN_H - FOOTER_H, width, FOOTER_H))
        up = int(now - self.started)
        self.text(self.small, (x0 + MARGIN, SCREEN_H - FOOTER_H + 5),
                  f"up {up // 3600:02}:{up // 60 % 60:02}:{up % 60:02}  desk aoa_host.py", C_DIM)


class Screen:
    """The window."""

    def __init__(self) -> None:
        """Open it; NoDisplayError if there is nothing to open it on."""
        try:
            pygame.display.init()
            pygame.font.init()
            self.window = pygame.display.set_mode((SCREEN_W, SCREEN_H + TOOLBAR_H))
        except pygame.error as e:
            raise NoDisplayError(str(e)) from e
        if pygame.display.get_driver() in ("dummy", "offscreen"):
            raise NoDisplayError(f"SDL driver {pygame.display.get_driver()}")
        pygame.display.set_caption("MMI screen (aoa_host.py)   1-9: faults   Q: quit")

    def run(self, model: ScreenModel, worker_alive,
            control: Callable[[dict], None] = lambda _cmd: None) -> int:
        """Redraw until the window is closed or worker_alive() turns false; control() gets
        the toolbar's commands."""
        painter = Painter(self.window.subsurface((0, 0, SCREEN_W, SCREEN_H)))
        toolbar, clock = Toolbar(), pygame.time.Clock()
        while worker_alive():
            for event in pygame.event.get():
                if event.type == pygame.QUIT:
                    return 0
                cmd = None
                if event.type == pygame.KEYDOWN:
                    if event.key in (pygame.K_q, pygame.K_ESCAPE):
                        return 0
                    cmd = toolbar.key(event.key)
                elif event.type == pygame.MOUSEBUTTONDOWN and event.button == 1:
                    cmd = toolbar.click(event.pos)
                if cmd is not None:
                    control(cmd)
            now = time.monotonic()
            if (cmd := toolbar.heartbeat(now)) is not None:
                control(cmd)
            state = model.snapshot()
            painter.paint(state, now)
            toolbar.paint(self.window, painter.small, state.sim)
            pygame.display.flip()
            clock.tick(REFRESH_HZ)
        return 0


def pump(messages: Iterable[tuple[dict, bytes]], model: ScreenModel) -> None:
    """Apply messages until they end; runs on its own thread."""
    for head, body in messages:
        model.apply(head, body)
