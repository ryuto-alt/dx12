"""PS-2(屋外昼)の「空のみ」= 物理大気 A1 のパリティシーンの生成器。使い捨てプロジェクトを作る(実プロジェクトには触れない)。

  python ps2_sky.py --out <dir> [--id ps2_sky_h12] [--args-json '{"hour":12}']     # ランナーが呼ぶ(シーンとプロジェクトを作る)
  python ps2_sky.py --emit-specs <scenes_dir>                                        # 仕様 JSON(5 時刻)とマスク PNG を書き出す

作るもの(プリミティブだけ。外部アセット無し):
  * 巨大な平面の地面(40 km 四方・ランバート反射率 0.3・粗さ 1)。建物なし。
  * 物理大気(atmosphere.enabled)。時刻 hour(現地太陽時)。緯度 35°・日付 81(春分)・groundAlbedo 0.3・sunIlluminance 128000 lux・
    multiScatteringFactor 1 を**明示**する(既定に頼らない)。太陽の平行光は大気が向き・色・強度を毎フレーム決める。
  * 物理ライティング単位(lightingUnits=1)・手動 EV100=15・UE Filmic・ポスト全 OFF・SSAO/SSGI/SSR/TAA/DDGI OFF。
カメラ 3 本(太陽の高度・方位は AtmosphereMath.h の ComputeSunAngles と同じ式を Python に写して決める):
  horizon … 太陽と反対側の方位。地平線が画面の上から 40% あたり(俯角 -4.7°)。空だけ(太陽が写らない)。
  sun     … 太陽の方位へ向け、仰角 +15°(地平線の少し下〜高度 37.5° が写る)。太陽が低い時刻は円盤・アウレオール・地平線の帯が写る。
  up      … 太陽の方位へ向け、仰角 +60° の見上げ(空だけ)。
マスク(regions): sky = 地平線より上(地平線の行 - 4 px まで)/ ground = 地平線より下(+4 px から)。カメラ・時刻ごとに 1 枚ずつ。
"""
from __future__ import annotations

import argparse
import json
import math
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from ps0_calibration import _ent, _guid  # noqa: E402

HOURS = [6, 9, 12, 17, 19]
LAT, DOY = 35.0, 81
EV100 = 15.0
EV100_BY_HOUR = {19: 3.0}   # 薄明の空は約 1 nit 前後。EV100=15 だと真っ黒(G0 健全性で落ちる)なので露出だけ変える(両エンジン同じ EV)
FOV_V = 45.0           # エディタカメラの垂直 FOV(set_editor_camera は変えられない)
CAM_POS = [0.0, 1.7, 0.0]
W, H = 1920, 1080


def sun_angles(hour: float, lat: float = LAT, doy: int = DOY) -> tuple[float, float]:
    """AtmosphereMath.h ComputeSunAngles の写し。戻り値 = (高度°, 方位°。北=+Z=0°・時計回り)。"""
    rad = math.pi / 180.0
    decl = 23.45 * math.sin(rad * 360.0 / 365.0 * (284.0 + doy))
    hh = (hour - 12.0) * 15.0 * rad
    phi, dl = lat * rad, decl * rad
    e = -math.cos(dl) * math.sin(hh)
    n = math.sin(dl) * math.cos(phi) - math.cos(dl) * math.sin(phi) * math.cos(hh)
    u = math.sin(dl) * math.sin(phi) + math.cos(dl) * math.cos(phi) * math.cos(hh)
    el = math.asin(max(-1.0, min(1.0, u))) / rad
    az = math.atan2(e, n) / rad
    return el, az + 360.0 if az < 0 else az


def dir_from(el_deg: float, az_deg: float) -> list[float]:
    el, az = math.radians(el_deg), math.radians(az_deg)
    return [math.cos(el) * math.sin(az), math.sin(el), math.cos(el) * math.cos(az)]


def cameras(hour: float) -> list[dict]:
    sel, saz = sun_angles(hour)
    p_h = -math.degrees(math.atan(0.2 * math.tan(math.radians(FOV_V / 2))))   # 地平線が上から 40%
    specs = [("horizon", (saz + 180.0) % 360.0, p_h,
              "太陽と反対側・地平線が画面の 40%(空だけ。夕方は反太陽側の帯・ビーナスベルト)"),
             ("sun", saz, 15.0, "太陽の方位・仰角 +15°(低い太陽なら円盤・アウレオール・地平線の帯)"),
             ("up", saz, 60.0, "太陽側の方位・仰角 +60° の見上げ(空だけ。天頂付近の色と勾配)")]
    out = []
    for name, az, pitch, note in specs:
        d = dir_from(pitch, az)
        tgt = [round(CAM_POS[i] + d[i] * 100.0, 4) for i in range(3)]
        out.append({"name": name, "position": list(CAM_POS), "target": tgt, "pitch": pitch, "azimuth": az, "note": note})
    return out


def horizon_row(pitch_deg: float) -> float:
    """画像の行(上から。float)。水平線(仰角 0°)が写る行。地面は y=0 の平面で、カメラ高 1.7 m は地平線の深さ 0.005°(20 km 先)で無視できる。"""
    t = math.tan(math.radians(pitch_deg)) / math.tan(math.radians(FOV_V / 2))
    return H / 2.0 * (1.0 + t)


def write_masks(out_dir: Path, hour_tag: str, hour: float, margin: int = 4) -> None:
    from PIL import Image
    import numpy as np
    out_dir.mkdir(parents=True, exist_ok=True)
    for c in cameras(hour):
        hr = horizon_row(c["pitch"])
        rows = np.arange(H, dtype=np.float32)[:, None]
        sky = np.broadcast_to(rows < hr - margin, (H, W))
        ground = np.broadcast_to(rows > hr + margin, (H, W))
        for nm, m in (("sky", sky), ("ground", ground)):
            if m.any():
                Image.fromarray((m.astype(np.uint8) * 255)).save(out_dir / f"ps2_sky_{hour_tag}_{c['name']}_{nm}.png", optimize=True)


def build_scene(hour: float, ev100: float | None = None) -> dict:
    ev100 = EV100_BY_HOUR.get(int(hour), EV100) if ev100 is None else ev100
    ents = [{"guid": _guid("Sun"), "name": "Sun",
             "directionalLight": {"ambient": 0.0, "color": [1.0, 1.0, 1.0], "direction": [0.0, -1.0, 0.0], "intensity": 100000.0},
             "transform": {"position": [0.0, 10.0, 0.0], "rotation": [-90.0, 0.0, 0.0], "scale": [1.0, 1.0, 1.0]}},
            _ent("Ground", "box", (0.0, -0.5, 0.0), (40000.0, 1.0, 40000.0), (0.3, 0.3, 0.3), roughness=1.0)]
    return {
        "version": 1, "shadows": True, "entities": ents,
        "ssao": {"enabled": False}, "contactShadow": {"enabled": False}, "ssr": {"enabled": False},
        "ssgi": {"enabled": False}, "taa": {"enabled": False},
        "raytracing": {"enabled": False},
        "skybox": {"drawSkybox": True, "envMapPath": "__procedural_sky__", "iblIntensity": 1.0, "skyboxIntensity": 1.0},
        "postProcess": {"enabled": False, "lightingUnits": 1, "exposureMode": 1, "ev100": ev100, "evComp": 0.0,
                        "tonemapper": 3, "debandOn": False},
        # 大気: 既定に頼らず主要値を明示する(両エンジンへ同じ値を書くための作法)
        "atmosphere": {"enabled": True, "timeOfDay": float(hour), "sunMode": 0, "driveSun": True, "driveIBL": True,
                       "latitudeDeg": LAT, "dayOfYear": DOY, "northYawDeg": 0.0,
                       "groundAlbedo": [0.3, 0.3, 0.3], "sunIlluminance": 128000.0, "multiScatteringFactor": 1.0,
                       "aerialPerspective": True, "apStartDepth": 100.0, "apMaxDistanceKm": 64.0, "apStrength": 1.0,
                       "drawStars": True, "drawMoon": True},
    }


GATES_G2 = {"flip_ldr_mean_max": 0.08, "flip_ldr_p95_max": 0.3, "flip_hdr_mean_max": 0.1, "lum_mean_ev_abs_max": 0.25,
            "ssim_min": 0.9, "de2000_median_max": 3.0, "hue_abs_median_deg_max": 8.0}
GATES_G1 = {"flip_ldr_mean_max": 0.15, "lum_mean_ev_abs_max": 0.5, "ssim_min": 0.75, "de2000_median_max": 6.0}


def build_spec(hour: int, overrides: dict | None = None) -> dict:
    tag = f"h{hour:02d}"
    sid = f"ps2_sky_{tag}"
    cams = cameras(hour)
    el, az = sun_angles(hour)
    cam_specs, regs = [], []
    for c in cams:
        names = []
        for r in ("sky", "ground"):
            if not _mask_exists(tag, c["name"], r, hour):
                continue
            nm = f"{r}_{c['name']}"
            names.append(nm)
            regs.append({"name": nm, "mask": f"masks/ps2_sky_{tag}_{c['name']}_{r}.png",
                         "gates": {"G2": GATES_G2} if r == "sky" else {"G1": GATES_G1}})
        cam_specs.append({"name": c["name"], "position": c["position"], "target": c["target"], "note": c["note"], "regions": names})
    spec = {
        "$schema": "./schema.json", "specVersion": 1, "id": sid, "parityScene": "PS-2",
        "title": f"PS-2 空のみ(物理大気 A1)・{hour} 時(太陽 高度 {el:.1f}° 方位 {az:.0f}°)",
        "description": "平面の地面 + 物理大気だけ。空(Sky-View LUT + 太陽円盤)・地平線の帯・エアリアルパースペクティブ(遠方の地面)・地面の IBL を、"
                       "同じ大気モデルの空を載せたパストレーサーと比べる。「空のみ G2」= 領域 sky のゲート。",
        "status": "ready",
        "notes": [
            "大気パラメータは既定に頼らず明示(groundAlbedo 0.3 / sunIlluminance 128000 lux / multiScatteringFactor 1 / 緯度 35° / 日付 81)。PT は同じ LUT から引く(A1 の PT 連携)",
            "エディタカメラの far は大気 ON のとき 20 km(地面は 40 km 四方)。EV100=15・UE Filmic・ポスト全 OFF",
            "領域 sky = 地平線より上(-4 px)。領域 ground = 地平線より下(+4 px)。ground は IBL(64² キューブ・遮蔽なし)/ AP の一次のみ等の既知の差を数値で載せる",
            "PT は bounces=1・maxRadiance=6e4(フォワードの太陽円盤クランプと同値)",
        ],
        "scene": {"source": "generator", "script": "gen/ps2_sky.py", "args": {"hour": hour}, "scene": f"scenes/{sid}.json"},
        "cameras": cam_specs,
        "engine": {
            "resolution": [W, H], "size": [W, H], "linear": True, "mode": "headless", "warmupFrames": 60, "settleFrames": 8,
            "png": "srgb",
            "features": {"ddgi": False, "ssgi": False, "ssr": False, "ssao": False, "taa": False,
                         "lightingUnits": "physical", "ev100": EV100_BY_HOUR.get(hour, EV100), "tonemapper": "ue_filmic"},
            "requiresMethods": ["render_reference"],
        },
        "reference": {"kind": "pt", "pt": {"spp": 1024, "seeds": [1, 2], "format": "pfm", "api": "engine", "timeoutSec": 1800,
                                            "params": {"bounces": 1, "maxRadiance": 60000}}},
        "alignment": {"tonemap": "ue_filmic", "exposure": f"ev100:{EV100_BY_HOUR.get(hour, EV100):g}", "sizePolicy": "error", "ppd": 67},
        "displayCheck": {"mean_lsb_max": 0.6, "p99_lsb_max": 2.0, "max_lsb_max": 4.0},
        "regions": regs,
        "gates": {"G0": {"lum_mean_ev_abs_max": 1.0}, "G1": GATES_G1, "G2": GATES_G2},
        "milestones": {"A1": "G2", "Q1": "G1"},
        "noiseFloor": {"k": 1.5},
        "perf": {"enabled": False},
    }
    if overrides:
        spec.update(overrides)
    return spec


_MASK_CACHE: dict = {}


def _mask_exists(tag: str, cam: str, region: str, hour: int) -> bool:
    hr = horizon_row(next(c for c in cameras(hour) if c["name"] == cam)["pitch"])
    return (hr - 4 > 0) if region == "sky" else (hr + 4 < H)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--out")
    ap.add_argument("--id", default="ps2_sky_h12")
    ap.add_argument("--args-json", default="{}")
    ap.add_argument("--emit-specs")
    a = ap.parse_args()
    if a.emit_specs:
        d = Path(a.emit_specs)
        for h in HOURS:
            write_masks(d / "masks", f"h{h:02d}", h)
            spec = build_spec(h)
            (d / f"ps2_sky_h{h:02d}.json").write_text(json.dumps(spec, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
        print(json.dumps({"emitted": len(HOURS)}))
        return 0
    if not a.out:
        raise SystemExit("--out か --emit-specs が要る")
    args = json.loads(a.args_json or "{}")
    hour = float(args.get("hour", 12.0))
    out = Path(a.out)
    (out / "assets" / "scenes").mkdir(parents=True, exist_ok=True)
    (out / "scripts").mkdir(exist_ok=True)
    scene_rel = f"scenes/{a.id}.json"
    (out / "assets" / "scenes" / f"{a.id}.json").write_text(json.dumps(build_scene(hour, float(args["ev100"]) if "ev100" in args else None), indent=2), encoding="utf-8")
    (out / "ParityGen.dx12proj").write_text(json.dumps(
        {"assetsDir": "assets", "defaultScene": scene_rel, "lastOpenedScene": scene_rel, "name": "ParityGen",
         "scriptsDir": "scripts", "version": "0.1.0"}, indent=2), encoding="utf-8")
    print(json.dumps({"project": str(out), "scene": scene_rel}))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
