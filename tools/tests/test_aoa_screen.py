import io
from fractions import Fraction

import aoa_screen
import av
import pygame
import pytest
from aoa_screen import Painter, ScreenModel, ScreenState

RED, RED_YUV = (220, 30, 30), (81, 90, 240)


@pytest.fixture
def target():
    pygame.display.init()
    pygame.font.init()
    return pygame.Surface((aoa_screen.SCREEN_W, aoa_screen.SCREEN_H))


def jpeg(w, h, colour=RED):
    surface = pygame.Surface((w, h))
    surface.fill(colour)
    out = io.BytesIO()
    pygame.image.save(surface, out, "x.jpg")
    return out.getvalue()


def h264_stream(w=720, h=432, count=5):
    """Baseline frames of solid red, Annex-B; the first one holds SPS and PPS."""
    codec = av.CodecContext.create("libx264", "w")
    codec.width, codec.height, codec.pix_fmt = w, h, "yuv420p"
    codec.time_base = Fraction(1, 25)
    codec.options = {"profile": "baseline", "tune": "zerolatency", "preset": "ultrafast"}
    packets = []
    for pts in range(count):
        frame = av.VideoFrame(w, h, "yuv420p")
        for plane, value in zip(frame.planes, RED_YUV, strict=True):
            plane.update(bytes([value]) * plane.buffer_size)
        frame.pts = pts
        packets += codec.encode(frame)
    packets += codec.encode(None)
    return [bytes(p) for p in packets]


def near(a, b, tolerance=40):
    return all(abs(x - y) <= tolerance for x, y in zip(a, b, strict=False))


def test_h264_fills_the_whole_screen(target):
    model = ScreenModel()
    model.apply({"kind": "h264_config"}, b"")
    for data in h264_stream():
        model.apply({"kind": "h264"}, data)
    state = model.snapshot()
    assert state.mode == "h264"
    assert state.shown >= 1
    Painter(target).paint(state, state.last_frame, strip=False)
    for corner in ((0, 0), (799, 0), (0, 479), (799, 479)):
        assert near(target.get_at(corner), RED)


def test_a_new_h264_stream_is_black_until_its_first_frame(target):
    model = ScreenModel()
    model.apply({"kind": "h264_config"}, b"")
    for data in h264_stream():
        model.apply({"kind": "h264"}, data)
    model.apply({"kind": "h264_config"}, b"")
    Painter(target).paint(model.snapshot(), 0, strip=False)
    assert target.get_at((400, 240))[:3] == aoa_screen.C_VIDEO_BG


@pytest.mark.parametrize("size", [(220, 480), (800, 368)])
def test_a_stream_after_a_jpeg_shows_the_panel_until_its_first_frame(target, size):
    model = ScreenModel()
    model.apply({"kind": "jpeg"}, jpeg(*size))
    model.apply({"kind": "h264_config"}, b"")
    Painter(target).paint(model.snapshot(), 0, strip=False)
    assert target.get_at((790, 5))[:3] == aoa_screen.LINKS["waiting"][0]


def test_landscape_jpeg_is_centred_on_black(target):
    model = ScreenModel()
    model.apply({"kind": "jpeg"}, jpeg(800, 368))
    Painter(target).paint(model.snapshot(), 0, strip=False)
    assert target.get_at((400, 20))[:3] == aoa_screen.C_VIDEO_BG
    assert near(target.get_at((400, 240)), RED)


def test_portrait_jpeg_goes_left_with_the_panel_beside_it(target):
    model = ScreenModel()
    model.apply({"kind": "jpeg"}, jpeg(220, 480))
    state = model.snapshot()
    Painter(target).paint(state, 0, strip=False)
    assert near(target.get_at((100, 240)), RED)
    assert target.get_at((220, 240))[:3] == aoa_screen.C_RULE
    assert target.get_at((790, 5))[:3] == aoa_screen.LINKS["waiting"][0]


def test_a_stop_goes_back_to_the_panel_and_says_why(target):
    model = ScreenModel()
    model.apply({"kind": "jpeg"}, jpeg(800, 480))
    model.apply({"kind": "stop", "why": "ended by VIDEO_STOP"}, b"")
    state = model.snapshot()
    assert state.mode is None
    assert state.ended == "ended by VIDEO_STOP"
    Painter(target).paint(state, 0, strip=False)
    assert target.get_at((790, 5))[:3] == aoa_screen.LINKS["waiting"][0]


def test_a_picture_that_does_not_decode_leaves_the_old_one_up():
    model = ScreenModel()
    model.apply({"kind": "jpeg"}, jpeg(800, 480))
    before = model.snapshot().picture
    model.apply({"kind": "jpeg"}, b"not a jpeg")
    assert model.snapshot().picture is before


def strip_colour(target, state, now):
    Painter(target).paint(state, now)
    return target.get_at((790, aoa_screen.SCREEN_H - aoa_screen.FOOTER_H - 3))


def test_the_strip_turns_red_only_while_a_picture_is_frozen(target):
    frozen = pygame.Surface((800, 480))
    state = ScreenState(mode="h264", picture=frozen, last_frame=100.0)
    assert strip_colour(target, state, 100.5)[0] < 100
    assert strip_colour(target, state, 101.5)[0] > 100
    stopped = ScreenState(mode=None, last_frame=100.0)
    assert strip_colour(target, stopped, 200)[0] < 100


def test_wrap_keeps_lines_within_the_width(target):
    font = pygame.font.Font(None, 32)
    lines = aoa_screen.wrap(font, aoa_screen.LINKS["connected"][2], 300)
    assert len(lines) > 1
    assert all(font.size(line)[0] <= 300 for line in lines)
