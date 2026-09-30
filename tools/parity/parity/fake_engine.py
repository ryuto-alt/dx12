"""偽エンジン(テストと `parity doctor --selftest` 用)。エンジンの MCP ブリッジ(行 JSON の TCP)を話すスタブ。

実エンジンと同じ流儀で、ping / open_scene / set_editor_camera / step_frames / screenshot_final / benchmark / perf_stats /
describe_mcp_manifest / render_reference(任意)に答え、画像は合成する。GPU も exe も要らない。

調整つまみ(テストで「エンジンが少しずれている」状況を作る):
  test_gain     エンジン画像の露出(リニアに掛ける倍率)
  ref_gain      パストレ基準の倍率
  ref_noise     パストレ基準に乗せるノイズの σ(seed ごとに違う)
  has_render_reference   False なら render_reference は「unknown method」
"""
from __future__ import annotations

import json
import socket
import threading
from pathlib import Path

import numpy as np

from . import color
from .imgio import save_png, write_pfm
from .tonemap import apply_tonemap


def synth_scene(w: int, h: int, cam_pos, cam_target) -> np.ndarray:
    """カメラで少し変わる、空・床・色つきの箱のある合成リニア画像(HxWx3、太陽 = 1 を超える)。"""
    yy, xx = np.mgrid[0:h, 0:w].astype(np.float32)
    u, v = xx / w, yy / h
    shift = float(cam_pos[0]) * 0.02 + float(cam_target[2]) * 0.01
    sky = np.stack([0.25 + 0.5 * (1 - v), 0.4 + 0.5 * (1 - v), 0.9 - 0.2 * v], -1) * 1.5
    floor = np.stack([0.18 + 0.1 * np.sin((u + shift) * 30) * np.sin(v * 30), 0.16 + 0.0 * u, 0.12 + 0.0 * u], -1)
    img = np.where((v > 0.55)[..., None], floor, sky)
    for cx, col in ((0.25, (0.8, 0.05, 0.05)), (0.5, (0.05, 0.6, 0.1)), (0.75, (0.1, 0.1, 0.9))):
        m = (np.abs(u - cx - shift * 0.3) < 0.08) & (v > 0.4) & (v < 0.75)
        img[m] = np.array(col, dtype=np.float32) * 0.9
    sun = ((u - 0.8) ** 2 + (v - 0.15) ** 2) < 0.0008
    img[sun] = 40.0
    return img.astype(np.float32)


class FakeEngine:
    def __init__(self, width: int = 320, height: int = 180):
        self.w, self.h = width, height
        self.test_gain, self.ref_gain, self.ref_noise = 1.0, 1.0, 0.0
        self.has_render_reference = True
        self.busy_once = True            # 最初の open_scene を「already in progress」で 1 回弾く(実エンジンの罠を再現)
        self.current_scene = "scenes/default.json"
        self.cam = ([0, 1, 0], [0, 1, 1])
        self.calls: list[tuple[str, dict]] = []
        self.gpu_ms, self.frame_ms, self.vram = 6.0, 9.0, 2048.0
        self.mutate = None               # callable(linear)->linear: テスト側で画像を壊す
        self._srv = socket.socket()
        self._srv.bind(("127.0.0.1", 0))
        self._srv.listen(1)
        self.port = self._srv.getsockname()[1]
        self._stop = False
        self._th = threading.Thread(target=self._serve, daemon=True)
        self._th.start()

    # ── サーバ ──
    def close(self) -> None:
        self._stop = True
        try:
            socket.create_connection(("127.0.0.1", self.port), timeout=1).close()
        except OSError:
            pass
        self._srv.close()

    def _serve(self) -> None:
        while not self._stop:
            try:
                conn, _ = self._srv.accept()
            except OSError:
                return
            if self._stop:
                return
            threading.Thread(target=self._client, args=(conn,), daemon=True).start()

    def _client(self, conn: socket.socket) -> None:
        buf = b""
        with conn:
            while True:
                try:
                    d = conn.recv(65536)
                except OSError:
                    return
                if not d:
                    return
                buf += d
                while b"\n" in buf:
                    line, buf = buf.split(b"\n", 1)
                    if not line.strip():
                        continue
                    req = json.loads(line)
                    resp = {"id": req["id"]}
                    try:
                        resp["ok"] = True
                        resp["result"] = self.handle(req["method"], req.get("params") or {})
                    except FakeError as e:
                        resp = {"id": req["id"], "ok": False, "error": e.msg, "error_code": e.code}
                    conn.sendall((json.dumps(resp) + "\n").encode())

    # ── メソッド ──
    def handle(self, method: str, p: dict) -> dict:
        self.calls.append((method, p))
        if method == "ping":
            return {"pong": True, "currentScene": self.current_scene, "engineVersion": "fake-1.0", "vramUsedMB": self.vram,
                    "vramBudgetMB": 7000.0, "pid": 1, "protocolVersion": 4}
        if method == "describe_mcp_manifest":
            name = p.get("method")
            known = {"ping", "open_scene", "set_editor_camera", "step_frames", "screenshot_final", "benchmark", "perf_stats",
                     "describe_mcp_manifest"} | ({"render_reference"} if self.has_render_reference else set())
            if name and name not in known:
                raise FakeError(f"unknown method: {name}", 8)
            return {"count": 1}
        if method == "open_scene":
            if self.busy_once:
                self.busy_once = False
                raise FakeError("a scene load is already in progress", 2)
            self.current_scene = p["path"]
            return {"ok": True}
        if method == "set_editor_camera":
            self.cam = (p.get("position", self.cam[0]), p.get("target", self.cam[1]))
            return {"position": self.cam[0]}
        if method == "step_frames":
            return {"frames": p.get("frames", 1)}
        if method == "screenshot_final":
            lin = synth_scene(self.w, self.h, *self.cam) * self.test_gain
            if self.mutate:
                lin = self.mutate(lin)
            d, _ = apply_tonemap("engine_aces", lin)
            path = p.get("path") or "shot.png"
            save_png(path, d)
            return {"path": path, "width": self.w, "height": self.h}
        if method == "render_reference":
            if not self.has_render_reference:
                raise FakeError("unknown method: render_reference", 8)
            lin = synth_scene(self.w, self.h, *self.cam) * self.ref_gain
            if self.ref_noise > 0:
                rng = np.random.default_rng(int(p.get("seed", 0)) + 1234)
                lin = np.clip(lin + rng.normal(0, self.ref_noise, lin.shape).astype(np.float32) * np.maximum(lin, 0.02) ** 0.5, 0, None)
            path = p["path"]
            write_pfm(path, lin)
            return {"path": path, "width": self.w, "height": self.h, "spp": p.get("spp")}
        if method in ("benchmark", "perf_stats"):
            return {"fps": 1000.0 / self.frame_ms, "frameMs": {"avg": self.frame_ms, "p95": self.frame_ms * 1.1},
                    "cpu": {"workMs": 4.0, "fenceWaitMs": 1.0, "presentMs": 0.5}, "gpuPassMs": {"total": self.gpu_ms, "mainScene": 3.0},
                    "drawCalls": 100, "triangles": 100000, "renderResolution": {"width": self.w, "height": self.h}}
        raise FakeError(f"unknown method: {method}", 8)


class FakeError(Exception):
    def __init__(self, msg: str, code: int = 2):
        super().__init__(msg)
        self.msg, self.code = msg, code


class FakeLauncher:
    """engine_session に渡す。起動済みの偽エンジンがあるので何もしない。"""

    def __init__(self):
        self.started = 0
        self.stopped = 0

    def start(self, project, extra_args=None):
        self.started += 1
        return {"fake": True}

    def stop(self):
        self.stopped += 1
