"""マネキンの足の接地判定(FU-E-34)の純関数と判定ロジックの単体テスト(合成画像のみ。エンジン不要)。"""
import json
import math

import numpy as np
import pytest

from parity import mannequin_feet as MF
from parity.imgio import write_pfm

H, W = 720, 1280


def test_row_to_gap_zero_at_center():
    # 最下行 r = H/2 - 1 の下端 = 中心行 = y=0
    assert MF.row_to_gap_m(H // 2 - 1, H, 6.0) == pytest.approx(0.0, abs=1e-9)


def test_row_to_gap_sign_and_scale():
    one_px = 6.0 * math.tan(math.radians(22.5)) / (H / 2)
    assert MF.row_to_gap_m(H // 2 - 2, H, 6.0) == pytest.approx(one_px)      # 1 行上 = 浮き
    assert MF.row_to_gap_m(H // 2 + 0, H, 6.0) == pytest.approx(-one_px)     # 1 行下 = めり込み


def test_figure_mask_and_bottom_row():
    ref = np.zeros((H, W, 3), np.float32)
    img = ref.copy()
    img[100:H // 2, 600:700] = 0.5
    m = MF.figure_mask(img, ref)
    assert MF.bottom_row(m) == H // 2 - 1
    assert MF.bottom_row(MF.figure_mask(ref, ref)) is None
    with pytest.raises(ValueError):
        MF.figure_mask(img[:10], ref)


def test_judge():
    assert MF.judge([0.0, 0.01, -0.01])["pass"]
    assert not MF.judge([0.0, 0.05])["pass"]
    assert not MF.judge([0.0, -0.05])["pass"]
    assert not MF.judge([0.0, None])["pass"]
    assert not MF.judge([])["pass"]


def _man(tmp_path, bottoms, move=True):
    ref = np.zeros((H, W, 3), np.float32)
    write_pfm(tmp_path / "ref.pfm", ref)
    frames = []
    for k, b in enumerate(bottoms):
        img = ref.copy()
        x0 = 600 + (20 * k if move else 0)
        img[100:b + 1, x0:x0 + 100] = 0.5
        write_pfm(tmp_path / f"f{k:02d}.pfm", img)
        frames.append(f"f{k:02d}.pfm")
    return {"frames": frames, "camDistM": 6.0, "clipTimes": []}


def test_evaluate_pass_and_float(tmp_path):
    man = _man(tmp_path, [H // 2 - 1] * 4)
    assert MF.evaluate(tmp_path, man)["pass"]
    # 15 行浮く = 約 6.0*0.414/360*15 = 10cm
    d2 = tmp_path / "b"
    d2.mkdir()
    r = MF.evaluate(d2, _man(d2, [H // 2 - 16] * 4))
    assert not r["pass"] and r["maxGapM"] > 0.05


def test_evaluate_static_animation_fails(tmp_path):
    r = MF.evaluate(tmp_path, _man(tmp_path, [H // 2 - 1] * 4, move=False))
    assert not r["pass"]
