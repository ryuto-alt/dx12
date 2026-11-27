"""マネキン(NB14)の足が床に着いているかを、エンジン(--headless)で読み込んで確かめる(FU-E-34)。

  pwsh -NoProfile -File tools\\parity\\mannequin_feet.ps1 --build-dir <build\\release のあるフォルダ>     # 既定 port 8854(lane-d)
  pwsh -NoProfile -File tools\\parity\\mannequin_feet.ps1 --from-dir <撮影済みフォルダ>                 # 撮影済みから判定だけやり直す(エンジン不要)

★原理(床の描画に頼らない): 床の上面 = ワールド y=0。カメラを **y=0・ピッチ 0**(水平)に置くと、y=0 の点は奥行きに関係なく画面の中心行に写る
  (y=0 のモデル底面は必ず中心行)。マネキンのいないシーンとの差分でマネキンの画素を取り、最下行が中心行からどれだけ下 / 上かを
  `gap_m = (中心行 - 最下行) * 奥行き * tan(fov/2) / (H/2)` でメートルに戻す。
    gap_m > 0 = 浮いている / gap_m < 0 = 床にめり込んでいる。
  歩きのループ(`Walk` 1.0 秒・30fps)を 30 コマ(固定 dt = 1/30 秒)ぶん撮り、全コマで |gap| <= 許容(既定 2cm)を合格とする。
  描画はラスタのみ(RT・DDGI・TAA・DLSS すべて OFF)。窓は出さない。マウス・フォーカスに触らない。
"""
from __future__ import annotations

import argparse
import datetime as dt
import json
import math
import sys
import time
from pathlib import Path

import numpy as np

from . import paths
from .engine import EngineClient, EngineError, PwshLauncher, wait_ping
from .imgio import load_image

DEFAULT_PORT = 8854            # lane-d(NB)の MCP ポート(エンジンはこの lane の自分のポートだけ使う)
DEFAULT_NAME = "mannequinfeet"
FOV_V_DEG = 45.0               # エディタカメラの垂直 FOV(gi_scene.FOV_V_DEG と同じ)
CAM_DIST_M = 6.0               # カメラから原点までの水平距離(側面から X 方向に見る)
TOL_M = 0.02                   # 合格の許容(足裏の浮き / めり込みの絶対値)
DIFF_THRESH = 0.02             # 参照(マネキン無し)との差(リニア 0..1 の最大チャンネル差)がこれ以上ならマネキンの画素
MIN_PIXELS = 400               # マネキンの画素数がこれ未満なら「写っていない」= 失敗
FRAMES = 30                    # Walk のコマ数(0..29。30 は 0 と同じ)
SCHEMA = 1


# ── 純粋関数(エンジン不要。単体テスト対象) ────────────────────────────────────

def figure_mask(img: np.ndarray, ref: np.ndarray, thresh: float = DIFF_THRESH) -> np.ndarray:
    """マネキン入りの画像と、いない画像の差(最大チャンネル差 >= thresh)。"""
    if img.shape != ref.shape:
        raise ValueError(f"画像の大きさが違う: {img.shape} vs {ref.shape}")
    return np.abs(img.astype(np.float32) - ref.astype(np.float32)).max(axis=2) >= thresh


def bottom_row(mask: np.ndarray, min_pixels: int = MIN_PIXELS) -> int | None:
    """マスクの最下行(画素の行番号。下ほど大きい)。画素数が足りなければ None。"""
    if int(mask.sum()) < min_pixels:
        return None
    rows = np.flatnonzero(mask.any(axis=1))
    return int(rows[-1])


def row_to_gap_m(row: int, height: int, depth_m: float, fov_v_deg: float = FOV_V_DEG) -> float:
    """最下行 → 床からの高さ(m)。カメラ y=0・ピッチ 0 のとき、画素行 r の下端(r+1)が示す高さ y = (H/2 - (r+1)) / (H/2) * depth * tan(fov/2)。
    行 r は下端が r+1(画素 r を完全に覆う = 足の最下点は行 r の下端以下)。正 = 浮き、負 = めり込み。"""
    half = height / 2.0
    return (half - (row + 1)) / half * depth_m * math.tan(math.radians(fov_v_deg) / 2.0)


def judge(gaps: list[float | None], tol_m: float = TOL_M) -> dict:
    """全コマの gap から合否。None(写っていない)が 1 つでもあれば失敗。"""
    vals = [g for g in gaps if g is not None]
    missing = len(gaps) - len(vals)
    if not gaps or not vals:
        return {"pass": False, "reason": "no frames", "missing": missing}
    mx, mn = max(vals), min(vals)
    worst = max(abs(mx), abs(mn))
    ok = missing == 0 and worst <= tol_m
    return {"pass": bool(ok), "frames": len(gaps), "missing": missing, "maxGapM": mx, "minGapM": mn,
            "worstAbsM": worst, "tolM": tol_m,
            "reason": "" if ok else ("missing frames" if missing else f"|gap| {worst * 100:.2f}cm > {tol_m * 100:.1f}cm")}


# ── エンジンでの撮影 ────────────────────────────────────────────────────────────

def write_project(out: Path) -> str:
    """使い捨てプロジェクト: 太陽 1 つだけの空シーン + マネキンの glb。シーンの JSON は gi_scene と同じ形式(version 1)。"""
    src = paths.REPO_ROOT / "assets" / "engine" / "characters" / "mannequin" / "mannequin.glb"
    if not src.exists():
        raise FileNotFoundError(f"マネキンが無い: {src}(NB14)")
    (out / "assets" / "scenes").mkdir(parents=True, exist_ok=True)
    (out / "assets" / "models").mkdir(parents=True, exist_ok=True)
    (out / "scripts").mkdir(exist_ok=True)
    (out / "assets" / "models" / "mannequin.glb").write_bytes(src.read_bytes())
    scene = {
        "version": 1, "shadows": False,
        "entities": [{"guid": "00000000-0000-0000-0000-00000000f001", "name": "Sun",
                      "directionalLight": {"color": [1.0, 1.0, 1.0], "direction": [0.3, -0.8, 0.5]},
                      "transform": {"position": [0, 10, 0], "rotation": [0, 0, 0], "scale": [1, 1, 1]}},
                     # Play には有効なカメラが要る。実際の視点は Play 中の set_editor_camera(MCP の上書き)で決める。
                     {"guid": "00000000-0000-0000-0000-00000000f002", "name": "Cam",
                      "camera": {"fovDegrees": FOV_V_DEG, "isActive": True},
                      "transform": {"position": [-CAM_DIST_M, 0, 0], "rotation": [0, 90, 0], "scale": [1, 1, 1]}}],
        "ssao": {"enabled": False}, "contactShadow": {"enabled": False}, "ssr": {"enabled": False},
        "ssgi": {"enabled": False}, "taa": {"enabled": False},
        "raytracing": {"shadowEnabled": False, "aoEnabled": False, "ddgi": {"enabled": False}},
        "skybox": {"drawSkybox": True, "envMapPath": "__procedural_sky__", "iblIntensity": 1.0, "skyboxIntensity": 1.0},
        "postProcess": {"enabled": False, "debandOn": False},
    }
    (out / "assets" / "scenes" / "feet.json").write_text(json.dumps(scene, indent=1), encoding="utf-8")
    (out / "Feet.dx12proj").write_text(json.dumps(
        {"assetsDir": "assets", "defaultScene": "scenes/feet.json", "lastOpenedScene": "scenes/feet.json",
         "name": "Feet", "scriptsDir": "scripts", "version": "0.1.0"}, indent=2), encoding="utf-8")
    return "scenes/feet.json"


def _shot(cli: EngineClient, base: Path, size: tuple[int, int]) -> Path:
    r = cli.call("screenshot_final", {"path": str(base), "format": "pfm", "width": size[0], "height": size[1], "gizmos": False,
                                       "deterministic": True, "settleFrames": 4}, timeout=240)
    files = r.get("files") or {}
    cand = files.get("pfm") if isinstance(files, dict) else None
    f = Path(cand) if cand else base.with_suffix(".pfm")
    if not f.exists():
        raise RuntimeError(f"PFM が書かれていない: {f}(応答: {str(r)[:300]})")
    return f


def capture(a, out: Path, log) -> dict:
    """エンジンを起動して参照 + 30 コマを撮る。戻り値 = manifest。必ず停止して終わる。"""
    proj = out / "_project"
    rel = write_project(proj)
    size = (a.width, a.height)
    launcher = PwshLauncher(name=DEFAULT_NAME, port=a.port, mode="headless", build_dir=a.build_dir)
    cli: EngineClient | None = None
    man: dict = {"schema": SCHEMA, "size": list(size), "camDistM": CAM_DIST_M, "clipTimes": [], "frames": []}
    try:
        log(f"エンジンを起動: name={DEFAULT_NAME} port={a.port}")
        launcher.start(str(proj), [f"--view-size={size[0]}x{size[1]}"])
        cli = EngineClient(a.port, connect_timeout=120)
        man["engine"] = {"ping": wait_ping(cli, 240.0)}
        from .runner import _open_scene
        _open_scene(cli, rel, log)
        for m, p in (("set_ssgi", {"enabled": False}), ("set_ssr", {"enabled": False}), ("set_ssao", {"enabled": False}),
                     ("set_contact_shadow", {"enabled": False}), ("set_upscaler", {"mode": "off"}),
                     ("set_volumetric_fog", {"enabled": False}),
                     ("set_dxr", {"shadowEnabled": False, "aoEnabled": False, "ddgiEnabled": False, "forceBuildTlas": False})):
            try:
                cli.call(m, p, timeout=60)
            except EngineError as e:                      # 無い設定は無視(この build に無いだけ)
                log(f"  (skip {m}: {str(e)[:80]})")
        # ★エディタモードは時間が止まる(Scene::Update に dt=0)ので、アニメを動かすには Play(ヘッドレスでも可)にする。
        #   Play 中は spawn できないので、エディタで原点に置いてから Play へ入り、参照の撮影だけ視界の外(z=50)へ動かす。
        out.mkdir(parents=True, exist_ok=True)
        cli.call("spawn_model", {"name": "Mannequin", "path": "models/mannequin.glb", "position": [0.0, 0.0, 0.0]}, timeout=120)
        cli.call("step_frames", {"frames": 4, "deterministic": True}, timeout=120)
        cli.call("play", {}, timeout=120)
        cli.call("step_frames", {"frames": 4, "deterministic": True}, timeout=120)
        # カメラ: y=0・水平。-X 側から +X を見る(マネキンは +Z 向きなので側面)。
        cli.call("set_editor_camera", {"position": [-CAM_DIST_M, 0.0, 0.0], "target": [0.0, 0.0, 0.0]}, timeout=60)
        cli.call("set_transform", {"name": "Mannequin", "position": [0.0, 0.0, 50.0]}, timeout=60)
        cli.call("step_frames", {"frames": 2, "deterministic": True}, timeout=120)
        _shot(cli, out / "ref", size)
        cli.call("set_transform", {"name": "Mannequin", "position": [0.0, 0.0, 0.0]}, timeout=60)
        cli.call("step_frames", {"frames": 2, "deterministic": True}, timeout=120)
        st0 = cli.call("get_anim_state", {"name": "Mannequin"}, timeout=30)
        man["animState"] = {k: st0.get(k) for k in ("hasSkeletalAnimation", "boneCount", "clips")}
        if not st0.get("hasSkeletalAnimation") or "Walk" not in (st0.get("clips") or []):
            raise RuntimeError(f"スキンドの Walk が読めていない: {man['animState']}")
        cli.call("play_anim", {"name": "Mannequin", "clipName": "Walk", "loop": True, "blend": 0.0}, timeout=60)
        for k in range(FRAMES):
            # 1 コマずつ固定 dt = 1/30 秒で進める(最初の撮影前にも 1 コマ進めて blend=0 を反映させる)
            cli.call("step_frames", {"frames": 1, "dt": 1.0 / 30.0, "deterministic": True}, timeout=120)
            st = cli.call("get_anim_state", {"name": "Mannequin"}, timeout=30)
            man["clipTimes"].append(st.get("clipTime"))
            f = _shot(cli, out / f"f{k:02d}", size)
            man["frames"].append(str(f.relative_to(out)))
        return man
    finally:
        if cli is not None:
            cli.close()
        launcher.stop()


def evaluate(out: Path, man: dict, tol_m: float = TOL_M) -> dict:
    ref = load_image(out / "ref.pfm", colorspace="linear").rgb
    h = ref.shape[0]
    gaps: list[float | None] = []
    rows: list[int | None] = []
    for rel in man["frames"]:
        img = load_image(out / rel, colorspace="linear").rgb
        r = bottom_row(figure_mask(img, ref))
        rows.append(r)
        gaps.append(None if r is None else row_to_gap_m(r, h, float(man.get("camDistM", CAM_DIST_M))))
    res = judge(gaps, tol_m)
    # アニメが本当に動いたか(足の最下行ではなく画像全体がコマごとに違う)
    imgs = [load_image(out / rel, colorspace="linear").rgb for rel in (man["frames"][0], man["frames"][len(man["frames"]) // 2])]
    res["animMovesPx"] = int(figure_mask(imgs[0], imgs[1]).sum())
    res["clipTimes"] = man.get("clipTimes")
    res["bottomRows"] = rows
    res["gapsM"] = [None if g is None else round(g, 5) for g in gaps]
    res["pass"] = bool(res["pass"] and res["animMovesPx"] >= MIN_PIXELS // 4)
    if res["pass"] is False and not res.get("reason"):
        res["reason"] = "animation did not move"
    return res


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description="マネキンの足が床に着いているかをエンジンで確かめる(FU-E-34)")
    ap.add_argument("--build-dir", help="engine_instance.ps1 -BuildDir(exe + DLL + shaders。ビルドしないレーンは本流の build\\release を指す)")
    ap.add_argument("--port", type=int, default=DEFAULT_PORT)
    ap.add_argument("--width", type=int, default=1280)
    ap.add_argument("--height", type=int, default=720)
    ap.add_argument("--tol", type=float, default=TOL_M, help="許容(m)")
    ap.add_argument("--out", help="出力先(既定: <repo>/.dx12/parity/mannequin_feet/<日時>)")
    ap.add_argument("--from-dir", help="撮影済みフォルダ(manifest.json がある)から判定だけやり直す")
    a = ap.parse_args(argv)

    def log(m: str) -> None:
        print(m, flush=True)

    if a.from_dir:
        out = Path(a.from_dir)
        man = json.loads((out / "manifest.json").read_text(encoding="utf-8"))
    else:
        out = Path(a.out) if a.out else paths.REPO_ROOT / ".dx12" / "parity" / "mannequin_feet" / dt.datetime.now().strftime("%Y%m%d-%H%M%S")
        out.mkdir(parents=True, exist_ok=True)
        t0 = time.time()
        man = capture(a, out, log)
        man["captureSec"] = round(time.time() - t0, 1)
        (out / "manifest.json").write_text(json.dumps(man, indent=1), encoding="utf-8")
    res = evaluate(out, man, a.tol)
    (out / "mannequin_feet.json").write_text(json.dumps(res, indent=1), encoding="utf-8")
    log(f"[{'PASS' if res['pass'] else 'FAIL'}] frames={res.get('frames')} maxGap={res.get('maxGapM', 0) * 100:+.2f}cm "
        f"minGap={res.get('minGapM', 0) * 100:+.2f}cm tol={a.tol * 100:.1f}cm animMovesPx={res.get('animMovesPx')} {res.get('reason', '')}")
    log(f"出力: {out}")
    return 0 if res["pass"] else 1


if __name__ == "__main__":
    sys.exit(main())
