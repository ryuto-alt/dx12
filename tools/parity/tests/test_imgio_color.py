import numpy as np
import pytest
from PIL import Image

from parity import color
from parity.imgio import (ImageError, Img, SizeMismatch, ensure_same_size, load_image, save_png, write_pfm)
from conftest import gradient


def test_png_roundtrip_8bit(tmp_path):
    a = gradient()
    p = save_png(tmp_path / "a.png", a)
    im = load_image(p)
    assert im.kind == "display" and im.eotf == "srgb" and im.size == (160, 90)
    assert np.abs(im.rgb - a).max() <= 0.5 / 255 + 1e-6


def test_png_gamma22_flag(tmp_path):
    p = save_png(tmp_path / "a.png", gradient())
    assert load_image(p, "gamma22").eotf == "gamma22"


def test_png_16bit(tmp_path):
    cv2 = pytest.importorskip("cv2")
    a = (gradient() * 65535).astype(np.uint16)
    ok, buf = cv2.imencode(".png", a[:, :, ::-1])
    assert ok
    (tmp_path / "s16.png").write_bytes(buf.tobytes())
    im = load_image(tmp_path / "s16.png")
    assert im.meta["bits"] == 16
    assert np.abs(im.rgb - a / 65535.0).max() < 1e-4


def test_pfm_roundtrip_is_linear_and_keeps_hdr(tmp_path):
    lin = (gradient() * 8.0).astype(np.float32)
    write_pfm(tmp_path / "a.pfm", lin)
    im = load_image(tmp_path / "a.pfm")
    assert im.kind == "linear"
    assert np.array_equal(im.rgb, lin)          # 上下反転も含めて完全に往復する
    assert im.rgb.max() > 1.0


def test_exr_roundtrip(tmp_path):
    pytest.importorskip("OpenEXR")
    from parity.imgio import write_exr
    lin = (gradient() * 4.0).astype(np.float32)
    write_exr(tmp_path / "a.exr", lin)
    im = load_image(tmp_path / "a.exr")
    assert im.kind == "linear" and np.array_equal(im.rgb, lin)


def test_hdr_cannot_be_read_as_srgb(tmp_path):
    write_pfm(tmp_path / "a.pfm", gradient())
    with pytest.raises(ImageError):
        load_image(tmp_path / "a.pfm", "srgb")


def test_png_as_linear_is_explicit(tmp_path):
    p = save_png(tmp_path / "a.png", gradient())
    assert load_image(p, "linear").kind == "linear"


def test_size_policies():
    a = Img(np.zeros((90, 160, 3), np.float32))
    b = Img(np.zeros((45, 80, 3), np.float32))
    with pytest.raises(SizeMismatch):
        ensure_same_size(a, b, "error")
    a2, b2, info = ensure_same_size(a, b, "resize")
    assert b2.size == (160, 90) and info["sizePolicy"] == "resize"
    c = Img(np.zeros((90, 100, 3), np.float32))
    with pytest.raises(SizeMismatch):
        ensure_same_size(a, c, "resize")            # 縦横比が違えば resize では合わせない
    a3, c3, info = ensure_same_size(a, c, "crop")
    assert a3.size == c3.size == (100, 90)


def test_srgb_roundtrip_and_known_values():
    x = np.linspace(0, 1, 101, dtype=np.float32)
    assert np.abs(color.linear_to_srgb(color.srgb_to_linear(x)) - x).max() < 1e-5
    assert abs(float(color.srgb_to_linear(np.float32(0.5))) - 0.21404) < 1e-4
    assert abs(float(color.gamma22_to_linear(np.float32(0.5))) - 0.5 ** 2.2) < 1e-6


def test_lab_known_colors():
    white = color.linear_to_lab(np.array([1.0, 1.0, 1.0]))
    assert abs(white[0] - 100) < 0.05 and abs(white[1]) < 0.05 and abs(white[2]) < 0.05
    red = color.linear_to_lab(np.array([1.0, 0.0, 0.0]))      # sRGB 赤 = Lab(53.24, 80.09, 67.20)
    assert np.allclose(red, [53.24, 80.09, 67.20], atol=0.05)


def test_delta_e_2000_published_pairs():
    # Sharma, Wu, Dalal (2005) の検証データから
    pairs = [((50.0, 2.6772, -79.7751), (50.0, 0.0, -82.7485), 2.0425),
             ((50.0, 3.1571, -77.2803), (50.0, 0.0, -82.7485), 2.8615),
             ((50.0, 2.5, 0.0), (73.0, 25.0, -18.0), 27.1492)]
    for a, b, expect in pairs:
        d = color.delta_e_2000(np.array([[a]]), np.array([[b]]))
        assert abs(float(d[0, 0]) - expect) < 1e-3


def test_hue_circular_difference():
    assert color.circ_diff_deg(np.array([10.0]), np.array([350.0]))[0] == pytest.approx(20.0)
    assert color.circ_diff_deg(np.array([350.0]), np.array([10.0]))[0] == pytest.approx(-20.0)
