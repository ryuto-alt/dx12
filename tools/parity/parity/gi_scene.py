"""GI 評価ハーネス(S1): 検証シーンの読み込み・エンジン用シーン JSON への変換・領域マスクの自動生成。

シーン定義は tools/parity/scenes/gi/*.json(生成器 gen/gi_scenes.py)。`spec` は SceneSpec v1 で、MCP の
`dx12_apply_scene_spec` にもそのまま渡せる。ランナーは同じ spec を**エンジンのシーン JSON**へ変換して使い捨てプロジェクトに書く
(box / sphere / light だけの小さな部分集合。MCP の spec コンパイラと同じ規則: size = 実寸 m・"#rrggbb" は sRGB→リニア・球の直径 = scale)。

領域マスクは「カメラから光線を投げて最初に当たるエンティティ」で作る(解析的な光線投射。手描き無し・エンジンの描画に依存しない)。
カメラは垂直 FOV 45°・ロール 0・左手系 Y 上(エンジンのエディタカメラと同じ)。マスクがエンジンの絵と合っているかは
ランナーが出力する overlay(masks.png)で目視確認できる。
"""
from __future__ import annotations

import hashlib
import json
import math
from dataclasses import dataclass
from pathlib import Path

import numpy as np

FOV_V_DEG = 45.0


# ── 読み込み ────────────────────────────────────────────────────────────────

@dataclass
class GiScene:
    id: str
    data: dict
    path: Path

    @property
    def entities(self) -> list[dict]:
        return self.data["spec"]["entities"]

    @property
    def cameras(self) -> list[dict]:
        return self.data["cameras"]

    def camera(self, name: str) -> dict:
        for c in self.cameras:
            if c["name"] == name:
                return c
        raise KeyError(f"{self.id}: カメラが無い: {name}")


def load_scene(path: str | Path) -> GiScene:
    p = Path(path)
    d = json.loads(p.read_text(encoding="utf-8"))
    for k in ("id", "spec", "cameras", "regions"):
        if k not in d:
            raise ValueError(f"{p.name}: '{k}' が無い")
    names = [e["name"] for e in d["spec"]["entities"]]
    if len(set(names)) != len(names):
        raise ValueError(f"{p.name}: エンティティ名が重複している")
    for rn, rd in d["regions"].items():
        for en in rd["entities"] + ([rd["nearEntity"]] if rd.get("nearEntity") else []):
            if en not in names:
                raise ValueError(f"{p.name}: 領域 {rn} が未知のエンティティ {en} を参照")
    return GiScene(d["id"], d, p)


def discover_scenes(dir_: Path) -> list[GiScene]:
    return [load_scene(p) for p in sorted(Path(dir_).glob("*.json"))]


# ── spec → エンジンのシーン JSON ────────────────────────────────────────────

def _guid(name: str) -> str:
    return hashlib.md5(name.encode("utf-8")).hexdigest()[:16]


def srgb_to_linear(v: float) -> float:
    return v / 12.92 if v <= 0.04045 else ((v + 0.055) / 1.055) ** 2.4


def parse_color(c) -> list[float]:
    if isinstance(c, str):
        h = c.lstrip("#")
        return [round(srgb_to_linear(int(h[i:i + 2], 16) / 255.0), 4) for i in (0, 2, 4)]
    return [float(x) for x in c]


def _size3(e: dict) -> list[float]:
    s = e.get("size", 1.0)
    return [float(s)] * 3 if isinstance(s, (int, float)) else [float(x) for x in s]


def ddgi_grid(ddgi: dict) -> dict:
    """バウンディングボックスを覆う DDGI 格子。各軸 n = round(extent / spacing)(最低 1 個)、残りを両端に均等に割る。"""
    sp = float(ddgi["spacing"])
    lo, hi = ddgi["min"], ddgi["max"]
    counts, origin = [], []
    for a in range(3):
        ext = float(hi[a]) - float(lo[a])
        n = max(1, int(round(ext / sp)))
        n = min(n, 32)
        counts.append(n)
        origin.append(float(lo[a]) + (ext - (n - 1) * sp) / 2.0)
    return {"probeCountX": counts[0], "probeCountY": counts[1], "probeCountZ": counts[2],
            "spacing": sp, "originX": origin[0], "originY": origin[1], "originZ": origin[2]}


def to_engine_scene(scene: GiScene) -> dict:
    """検証シーンをエンジンのシーン JSON(version 1)へ。GI 系の設定は全部 OFF の素の状態で書く(構成はランナーが MCP で切り替える)。"""
    ents = []
    for e in scene.entities:
        kind = e["kind"]
        pos = [float(x) for x in e.get("at", [0, 0, 0])]
        rot = [float(x) for x in e.get("rotation", [0, 0, 0])]
        out: dict = {"guid": _guid(e["name"]), "name": e["name"]}
        if kind in ("box", "sphere"):
            out["primitive"] = kind
            out["color"] = parse_color(e.get("color", "#ffffff"))
            m = e.get("material") or {}
            mat = {"metallic": float(m.get("metallic", 0.0)), "roughness": float(m.get("roughness", 0.5))}
            if "emissive" in m:
                mat["emissiveColor"] = [float(x) for x in m["emissive"]]
                mat["emissiveIntensity"] = float(m.get("emissiveIntensity", 1.0))
            out["material"] = mat
            out["transform"] = {"position": pos, "rotation": rot, "scale": _size3(e)}
        elif kind == "light":
            comps = e.get("components") or {}
            key = {"point": "pointLight", "directional": "directionalLight", "spot": "spotLight"}[e["light"]]
            body = dict(comps.get(key, {}))
            if key == "directionalLight":
                body.setdefault("color", [1.0, 1.0, 1.0])
                body.setdefault("direction", [0.0, -1.0, 0.0])
            out[key] = body
            out["transform"] = {"position": pos, "rotation": rot, "scale": [1.0, 1.0, 1.0]}
        else:
            raise ValueError(f"{scene.id}: 未対応の kind: {kind}")
        ents.append(out)
    sc = {
        "version": 1, "shadows": True, "entities": ents,
        "ssao": {"enabled": False}, "contactShadow": {"enabled": False}, "ssr": {"enabled": False},
        "ssgi": {"enabled": False}, "taa": {"enabled": False},
        "raytracing": {"shadowEnabled": False, "aoEnabled": False,
                       "ddgi": {"enabled": False, **ddgi_grid(scene.data["ddgi"]), "rayLength": 30.0, "hysteresis": 0.97,
                                "intensity": 1.0, "normalBias": 0.02, "bounceIntensity": 0.0}},
        "skybox": {"drawSkybox": True, "envMapPath": "__procedural_sky__", "iblIntensity": 1.0, "skyboxIntensity": 1.0},
        "postProcess": {"enabled": False, "debandOn": False},
    }
    return sc


def write_project(scenes: list[GiScene], out: Path) -> dict[str, str]:
    """使い捨てプロジェクトを作る。戻り値 = {scene id: open_scene に渡すパス}。"""
    (out / "assets" / "scenes").mkdir(parents=True, exist_ok=True)
    (out / "scripts").mkdir(exist_ok=True)
    rels = {}
    for s in scenes:
        rel = f"scenes/{s.id}.json"
        (out / "assets" / "scenes" / f"{s.id}.json").write_text(json.dumps(to_engine_scene(s), indent=1), encoding="utf-8")
        rels[s.id] = rel
    first = rels[scenes[0].id]
    (out / "GiEval.dx12proj").write_text(json.dumps(
        {"assetsDir": "assets", "defaultScene": first, "lastOpenedScene": first, "name": "GiEval",
         "scriptsDir": "scripts", "version": "0.1.0"}, indent=2), encoding="utf-8")
    return rels


# ── 光線投射(領域マスク) ───────────────────────────────────────────────────

def camera_rays(cam: dict, width: int, height: int, fov_v_deg: float = FOV_V_DEG):
    """画素中心を通る光線。戻り値 = (origin[3], dirs[H,W,3])。エンジンは左手系 Y 上: right = cross(up, forward)。"""
    pos = np.array(cam["position"], dtype=np.float64)
    tgt = np.array(cam["target"], dtype=np.float64)
    f = tgt - pos
    f /= np.linalg.norm(f)
    r = np.cross(np.array([0.0, 1.0, 0.0]), f)
    r /= np.linalg.norm(r)
    u = np.cross(f, r)
    th = math.tan(math.radians(fov_v_deg) / 2.0)
    aspect = width / height
    xs = (2.0 * (np.arange(width) + 0.5) / width - 1.0) * th * aspect
    ys = (1.0 - 2.0 * (np.arange(height) + 0.5) / height) * th
    d = f[None, None, :] + xs[None, :, None] * r[None, None, :] + ys[:, None, None] * u[None, None, :]
    d /= np.linalg.norm(d, axis=2, keepdims=True)
    return pos, d


def _intersect_box(o, d, lo, hi):
    with np.errstate(divide="ignore", invalid="ignore"):
        inv = 1.0 / d
        t0 = (lo[None, None, :] - o[None, None, :]) * inv
        t1 = (hi[None, None, :] - o[None, None, :]) * inv
    tn = np.minimum(t0, t1).max(axis=2)
    tf = np.maximum(t0, t1).min(axis=2)
    hit = (tf >= np.maximum(tn, 0.0))
    t = np.where(tn > 0, tn, tf)
    return np.where(hit & (t > 1e-6), t, np.inf)


def _intersect_sphere(o, d, c, r):
    oc = o - c
    b = (d * oc[None, None, :]).sum(axis=2)
    cc = float(oc @ oc) - r * r
    disc = b * b - cc
    ok = disc >= 0
    sq = np.sqrt(np.where(ok, disc, 0.0))
    t0, t1 = -b - sq, -b + sq
    t = np.where(t0 > 1e-6, t0, t1)
    return np.where(ok & (t > 1e-6), t, np.inf)


def primitive_bounds(e: dict) -> tuple[np.ndarray, np.ndarray]:
    c = np.array(e.get("at", [0, 0, 0]), dtype=np.float64)
    s = np.array(_size3(e), dtype=np.float64) / 2.0
    return c - s, c + s


def cast(scene: GiScene, cam: dict, width: int, height: int):
    """最初に当たるエンティティの添字(空 = -1)と、ヒット位置を返す。"""
    o, d = camera_rays(cam, width, height)
    best_t = np.full((height, width), np.inf)
    label = np.full((height, width), -1, dtype=np.int32)
    for i, e in enumerate(scene.entities):
        if e["kind"] == "box":
            lo, hi = primitive_bounds(e)
            t = _intersect_box(o, d, lo, hi)
        elif e["kind"] == "sphere":
            t = _intersect_sphere(o, d, np.array(e["at"], dtype=np.float64), _size3(e)[0] / 2.0)
        else:
            continue
        m = t < best_t
        best_t = np.where(m, t, best_t)
        label = np.where(m, i, label)
    pos = o[None, None, :] + d * np.where(np.isfinite(best_t), best_t, 0.0)[:, :, None]
    return label, pos, best_t


def _dist_to_aabb(p: np.ndarray, lo: np.ndarray, hi: np.ndarray) -> np.ndarray:
    q = np.maximum(np.maximum(lo[None, None, :] - p, 0.0), p - hi[None, None, :])
    return np.linalg.norm(q, axis=2)


def erode(mask: np.ndarray, px: int = 2) -> np.ndarray:
    """境界の混じりを避けるため領域を px だけ内側へ縮める。薄すぎて半分以上消えるなら 1px へ落とす。"""
    from scipy.ndimage import binary_erosion
    n0 = int(mask.sum())
    if n0 == 0 or px <= 0:
        return mask
    for k in range(px, 0, -1):
        m = binary_erosion(mask, structure=np.ones((2 * k + 1, 2 * k + 1), bool), border_value=0)
        if int(m.sum()) >= 0.4 * n0 or k == 1:
            return m
    return mask


def build_masks(scene: GiScene, cam: dict, width: int, height: int, erode_px: int = 2) -> dict[str, np.ndarray]:
    """領域名 -> bool[H,W]。領域定義は {entities:[...], nearEntity?, maxDist?}。"""
    label, pos, _ = cast(scene, cam, width, height)
    idx = {e["name"]: i for i, e in enumerate(scene.entities)}
    ents = {e["name"]: e for e in scene.entities}
    out: dict[str, np.ndarray] = {}
    for rn, rd in scene.data["regions"].items():
        m = np.zeros((height, width), bool)
        for en in rd["entities"]:
            m |= (label == idx[en])
        if rd.get("nearEntity"):
            lo, hi = primitive_bounds(ents[rd["nearEntity"]])
            m &= _dist_to_aabb(pos, lo, hi) <= float(rd.get("maxDist", 1.0))
        out[rn] = erode(m, erode_px)
    return out


# ── マスクの可視化(マスクが絵と合っているかの目視確認用) ──────────────────────

def label_overlay(masks: dict[str, np.ndarray], base: np.ndarray | None = None, alpha: float = 0.55) -> np.ndarray:
    """領域ごとに色を塗った画像(0..1 RGB)。base があればその上に半透明で重ねる。"""
    names = list(masks)
    h, w = next(iter(masks.values())).shape
    img = np.zeros((h, w, 3), np.float32) if base is None else base.astype(np.float32).copy()
    for i, n in enumerate(names):
        hue = (i * 0.61803398875) % 1.0
        import colorsys
        col = np.array(colorsys.hsv_to_rgb(hue, 0.85, 1.0), np.float32)
        m = masks[n]
        img[m] = img[m] * (1 - alpha) + col * alpha if base is not None else col
    return img
