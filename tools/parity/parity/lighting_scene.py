"""ライティング評価(LS-00b)の検証シーン LS-A〜F の読み込みと、影の領域マスクの自動生成。

シーンの形は GI 評価(gi_scene.py)と同じ(spec / sun / ddgi / cameras / regions / criteria)に、次の欄を足したもの:
  models[]       … スキンドなどの glb(エンジンの MCP `import_asset` → `spawn_model` で後から置く)。{name, path(リポジトリ相対), at, proxy:{size}}。
                   proxy は解析的な影マスク用の箱(足元 = at、高さ方向に size[1])。描画には使わない。
  variants[]     … 条件の違い(例: 昼 / 夕方 高度 10°)。{id, sun}。無ければ既定 1 つ(sun)。
  shadowRegions  … 解析的な影の領域 {名前: {receivers:[…], occluders:[…], state:"roi"|"lit", ...}}。
                     roi = 影と縁を含む領域(影の外周に roiPx 画素の余白)。縁の幅・coverage に使う。
                     lit = 影から十分離れた日向。アクネ(日向なのに影になる画素)の分母。
  texelBands     … テクセル密度を見る距離(m)の一覧。
  temporal       … {frames, settle}。静止 N フレームの安定を見るシーン。
  conditions     … このシーンを撮る既定の条件 ID(lighting_eval.CONDITIONS)。

マスクは「カメラから光線を投げて最初に当たるエンティティ」と「ヒット位置から太陽へ投げた光線が遮蔽物に当たるか」で作る(手描き無し・エンジンの描画に依存しない)。
太陽の向きは LightMath.h の SunAnglesToDirection と同じ(+Z = 方位 0°、+X = 90°。太陽が見える方向 = (cos(el) sin(az), sin(el), cos(el) cos(az)))。
"""
from __future__ import annotations

import math
from pathlib import Path

import numpy as np

from . import gi_scene as GS
from . import paths

SCENES_DIR = paths.TOOL_DIR / "scenes" / "lighting"
DEFAULT_VARIANT = "default"


def load_scenes(dir_: Path | None = None) -> list[GS.GiScene]:
    scenes = GS.discover_scenes(dir_ or SCENES_DIR)
    for s in scenes:
        validate(s)
    return scenes


def validate(s: GS.GiScene) -> None:
    names = {e["name"] for e in s.entities}
    for m in s.data.get("models", []):
        for k in ("name", "path", "at", "proxy"):
            if k not in m:
                raise ValueError(f"{s.id}: models の要素に '{k}' が無い")
        if m["name"] in names:
            raise ValueError(f"{s.id}: モデル名がエンティティ名と重複: {m['name']}")
    known = names | {m["name"] for m in s.data.get("models", [])}
    for rn, rd in s.data.get("shadowRegions", {}).items():
        for en in rd["receivers"] + rd["occluders"]:
            if en not in known:
                raise ValueError(f"{s.id}: shadowRegions.{rn} が未知の名前 {en} を参照")
        if rd.get("state", "roi") not in ("roi", "lit"):
            raise ValueError(f"{s.id}: shadowRegions.{rn}.state は roi | lit")
    for v in s.variants():
        if "id" not in v or "sun" not in v:
            raise ValueError(f"{s.id}: variants の要素に id / sun が要る")


def variants(self: GS.GiScene) -> list[dict]:
    v = self.data.get("variants")
    return v if v else [{"id": DEFAULT_VARIANT, "sun": self.data["sun"]}]


GS.GiScene.variants = variants     # type: ignore[attr-defined]


def to_toward_sun(azimuth_deg: float, elevation_deg: float) -> np.ndarray:
    az, el = math.radians(azimuth_deg), math.radians(elevation_deg)
    return np.array([math.cos(el) * math.sin(az), math.sin(el), math.cos(el) * math.cos(az)], dtype=np.float64)


def model_proxy_box(m: dict) -> tuple[np.ndarray, np.ndarray]:
    at = np.array(m["at"], dtype=np.float64)
    sx, sy, sz = (float(x) for x in m["proxy"]["size"])
    c = at + np.array([0.0, sy / 2.0, 0.0])
    h = np.array([sx / 2.0, sy / 2.0, sz / 2.0])
    return c - h, c + h


def _occluder_boxes(scene: GS.GiScene, names: list[str]) -> list[tuple[np.ndarray, np.ndarray]]:
    ents = {e["name"]: e for e in scene.entities}
    models = {m["name"]: m for m in scene.data.get("models", [])}
    out = []
    for n in names:
        if n in models:
            out.append(model_proxy_box(models[n]))
        else:
            if ents[n]["kind"] not in ("box", "sphere"):
                raise ValueError(f"{scene.id}: 影の遮蔽物は box / sphere だけ対応(球は外接の箱で近似): {n}")
            out.append(GS.primitive_bounds(ents[n]))
    return out


def occluded(pos: np.ndarray, toward_sun: np.ndarray, boxes: list[tuple[np.ndarray, np.ndarray]]) -> np.ndarray:
    """pos[H,W,3] から太陽方向へ伸ばした光線が boxes のどれかに当たるか(スラブ法。向きは全画素で共通)。"""
    d = toward_sun
    hit = np.zeros(pos.shape[:2], bool)
    o = pos + d[None, None, :] * 1e-3
    for lo, hi in boxes:
        tn = np.full(pos.shape[:2], -np.inf)
        tf = np.full(pos.shape[:2], np.inf)
        ok = np.ones(pos.shape[:2], bool)
        for a in range(3):
            if abs(d[a]) < 1e-12:
                ok &= (o[..., a] >= lo[a]) & (o[..., a] <= hi[a])
                continue
            t0 = (lo[a] - o[..., a]) / d[a]
            t1 = (hi[a] - o[..., a]) / d[a]
            tn = np.maximum(tn, np.minimum(t0, t1))
            tf = np.minimum(tf, np.maximum(t0, t1))
        hit |= ok & (tf >= np.maximum(tn, 0.0))
    return hit


def _dilate(m: np.ndarray, px: int) -> np.ndarray:
    from scipy.ndimage import binary_dilation
    return binary_dilation(m, iterations=px) if px > 0 else m


def build_shadow_masks(scene: GS.GiScene, cam: dict, sun: dict, width: int, height: int) -> dict[str, np.ndarray]:
    """shadowRegions の領域名 → bool[H,W]。"""
    label, pos, _ = GS.cast(scene, cam, width, height)
    idx = {e["name"]: i for i, e in enumerate(scene.entities)}
    out: dict[str, np.ndarray] = {}
    for rn, rd in scene.data.get("shadowRegions", {}).items():
        recv = np.zeros((height, width), bool)
        for n in rd["receivers"]:
            if n in idx:
                recv |= (label == idx[n])
        boxes = _occluder_boxes(scene, rd["occluders"])
        # 影の外周(半影帯)を含めるため、太陽の向きを ±jitter 度ふらして「どれかで遮蔽」を取る
        j = float(rd.get("jitterDeg", 4.0))
        anyocc = np.zeros((height, width), bool)
        for da, de in ((0, 0), (j, 0), (-j, 0), (0, j), (0, -j)):
            d = to_toward_sun(sun["azimuth"] + da, min(max(sun["elevation"] + de, 1.0), 89.0))
            anyocc |= occluded(pos, d, boxes)
        anyocc &= recv
        if rd.get("state", "roi") == "roi":
            m = _dilate(anyocc, int(rd.get("roiPx", 40))) & recv
            near = rd.get("maxDepth")             # 視点からの距離 [m] の上限(遠方の影を別領域に分けるとき)
            if near is not None:
                dist = np.linalg.norm(pos - np.array(cam["position"], dtype=np.float64)[None, None, :], axis=2)
                m &= dist <= float(near)
            lo = rd.get("minDepth")
            if lo is not None:
                dist = np.linalg.norm(pos - np.array(cam["position"], dtype=np.float64)[None, None, :], axis=2)
                m &= dist >= float(lo)
        else:
            m = recv & ~_dilate(anyocc, int(rd.get("roiPx", 40)) + 20)
        out[rn] = GS.erode(m, 1)
    return out


def mannequin_models(scene: GS.GiScene) -> list[dict]:
    return list(scene.data.get("models", []))
