import os
import sys
from pathlib import Path

import numpy as np
import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from parity.fake_engine import FakeEngine, FakeLauncher  # noqa: E402
from parity.imgio import save_png  # noqa: E402


@pytest.fixture(autouse=True)
def parity_home(tmp_path, monkeypatch):
    home = tmp_path / "parity_home"
    monkeypatch.setenv("PARITY_HOME", str(home))
    monkeypatch.setenv("PARITY_GPU_KEY", "TestGPU")
    monkeypatch.setenv("PARITY_GPU_DRIVER", "1.0")
    monkeypatch.delenv("DX12_MCP_PORT", raising=False)
    monkeypatch.setenv("PARITY_TEST_PROJECT", str(tmp_path))
    return home


@pytest.fixture
def fake():
    e = FakeEngine()
    yield e
    e.close()


@pytest.fixture
def launcher():
    return FakeLauncher()


def gradient(h=90, w=160, seed=0):
    """表示参照(0..1)のなだらかな画像。色も輝度も幅がある。"""
    yy, xx = np.mgrid[0:h, 0:w].astype(np.float32)
    u, v = xx / (w - 1), yy / (h - 1)
    img = np.stack([0.15 + 0.7 * u, 0.2 + 0.6 * v, 0.6 - 0.4 * u * v + 0.1 * np.sin(u * 20)], -1)
    return np.clip(img, 0, 1).astype(np.float32)


@pytest.fixture
def make_png(tmp_path):
    def _mk(name, arr):
        return str(save_png(tmp_path / name, arr))
    return _mk


def spec_data(**over):
    d = {
        "specVersion": 1, "id": "t_scene", "title": "テスト", "status": "ready",
        "scene": {"source": "project", "project": "${PARITY_TEST_PROJECT}", "scene": "scenes/fake.json"},
        "cameras": [{"name": "a", "position": [0, 1, 0], "target": [0, 1, 5]},
                    {"name": "b", "position": [3, 1, 0], "target": [0, 1, 5]}],
        "engine": {"warmupFrames": 1, "settleFrames": 1},
        "reference": {"kind": "pt", "pt": {"spp": 8, "seeds": [1, 2], "api": "legacy"}},
        "alignment": {"tonemap": "engine_aces", "sizePolicy": "resize"},
        "gates": {"G1": {"flip_ldr_mean_max": 0.15, "ssim_min": 0.75, "lum_mean_ev_abs_max": 0.5},
                  "G2": {"flip_ldr_mean_max": 0.08, "ssim_min": 0.9, "lum_mean_ev_abs_max": 0.25}},
    }
    d.update(over)
    return d
