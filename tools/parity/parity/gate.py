"""合否判定(G0 健全 / G1 近い / G2 同等)。

しきい値キー = `<指標名>_max`(値 <= 限界) / `<指標名>_min`(値 >= 限界) / `<指標名>_abs_max`(|値| <= 限界)。
指標名は metrics.LABELS のキー(例: flip_ldr_mean_max, ssim_min, lum_mean_ev_abs_max, de2000_median_max)。
実効しきい値 = max(固定値, ノイズ床 x 1.5)(ノイズ床があるとき。noisefloor.effective_limit)。
しきい値を緩めて通すのは禁止: 緩める変更は仕様 JSON の差分としてレビューされ、baseline の approve と同様に理由を残す。

判定の状態:
  pass    = 全チェック合格(取れなかった指標があれば warnings に出す。--strict なら fail)
  fail    = どれかが不合格
  skipped = 基準が無い等で判定できない(合格扱いにしない)
"""
from __future__ import annotations

import math

import numpy as np

from . import metrics as M
from .noisefloor import effective_limit

GATES = ("G0", "G1", "G2")
_OPS = ("_abs_max", "_max", "_min")


class GateError(Exception):
    pass


def parse_key(key: str) -> tuple[str, str]:
    for suf in _OPS:
        if key.endswith(suf):
            metric = key[: -len(suf)]
            if metric in M.LABELS:
                return metric, suf[1:]
    raise GateError(f"しきい値キーが不明: {key}(<指標>_max / _min / _abs_max。指標: {', '.join(M.LABELS)})")


def sanity(test_display: np.ndarray, ref_display: np.ndarray | None = None) -> dict:
    """G0: 全黒 / 全白 / NaN / 無変化の平面。常に見る。"""
    a = np.asarray(test_display, dtype=np.float32)
    flags = {}
    flags["nan_or_inf"] = bool(not np.isfinite(a).all())
    a = np.nan_to_num(a)
    flags["all_black"] = bool(a.max() < 0.01)
    flags["all_white"] = bool(a.min() > 0.99)
    flags["flat"] = bool(a.std() < 0.002 and not flags["all_black"] and not flags["all_white"])
    flags["mean"] = float(a.mean())
    flags["std"] = float(a.std())
    bad = [k for k in ("nan_or_inf", "all_black", "all_white", "flat") if flags[k]]
    flags["ok"] = not bad
    flags["problems"] = bad
    return flags


def _check(scope: str, key: str, value, fixed: float, floors: dict | None, k: float) -> dict:
    metric, op = parse_key(key)
    limit, relaxed = effective_limit(metric, op, fixed, floors, k)
    c = {"scope": scope, "metric": metric, "op": op, "fixed": fixed, "limit": limit, "floor_applied": relaxed,
         "value": value}
    if value is None:
        c["status"] = "unavailable"
        return c
    ok = (value <= limit) if op == "max" else (abs(value) <= limit) if op == "abs_max" else (value >= limit)
    c["status"] = "pass" if ok else "fail"
    return c


def _fmt(c: dict) -> str:
    sym = {"max": "<=", "abs_max": "|x|<=", "min": ">="}[c["op"]]
    lab = M.LABELS.get(c["metric"], c["metric"])
    v = c["value"]
    extra = f"(ノイズ床で緩和: 固定 {c['fixed']:g})" if c["floor_applied"] else ""
    return f"[{c['scope']}] {lab} = {v:.4g} が限界 {sym} {c['limit']:.4g} {extra}".strip()


def judge(values: dict, gate: dict, floors: dict | None = None, k: float = 1.5, scope: str = "全体",
          strict: bool = False) -> dict:
    """1 つの範囲(全体または領域)の指標値 dict を、gate(しきい値 dict)で判定する。"""
    checks = [_check(scope, key, values.get(parse_key(key)[0]), float(v), floors, k) for key, v in gate.items()
              if not key.startswith("$")]
    fails = [c for c in checks if c["status"] == "fail"]
    unav = [c for c in checks if c["status"] == "unavailable"]
    reasons = [_fmt(c) for c in fails]
    warnings = [f"[{c['scope']}] {M.LABELS.get(c['metric'], c['metric'])}: 計算できなかった(入力がリニア HDR でない等)" for c in unav]
    if strict:
        reasons += warnings
    status = "fail" if (fails or (strict and unav)) else "pass"
    return {"status": status, "checks": checks, "reasons": reasons, "warnings": warnings}


def merge(verdicts: list[dict]) -> dict:
    checks = [c for v in verdicts for c in v["checks"]]
    reasons = [r for v in verdicts for r in v["reasons"]]
    warnings = [w for v in verdicts for w in v["warnings"]]
    status = "fail" if any(v["status"] == "fail" for v in verdicts) else "pass"
    return {"status": status, "checks": checks, "reasons": reasons, "warnings": warnings}


def judge_camera(values_full: dict, values_regions: dict[str, dict], gate_name: str, gate_full: dict,
                 gate_regions: dict[str, dict], floors: dict | None, k: float, san: dict | None,
                 strict: bool = False) -> dict:
    """G0 の健全性 + 選んだ段階のしきい値(全体と領域)で 1 カメラを判定する。"""
    vs = []
    if san is not None and not san.get("ok", True):
        vs.append({"status": "fail", "checks": [], "warnings": [],
                   "reasons": [f"G0 健全性: エンジン画像が異常({', '.join(san['problems'])})"]})
    vs.append(judge(values_full, gate_full or {}, floors, k, "全体", strict))
    for name, g in (gate_regions or {}).items():
        if name not in values_regions:
            vs.append({"status": "fail" if strict else "pass", "checks": [], "reasons": [f"領域 '{name}' のマスクが無く判定できない"] if strict else [],
                       "warnings": [f"領域 '{name}' の指標が無い(マスクが空か未指定)"]})
            continue
        vs.append(judge(values_regions[name], g, floors, k, f"領域:{name}", strict))
    out = merge(vs)
    out["gate"] = gate_name
    return out


def overall(statuses: list[str]) -> str:
    """カメラ・性能などの状態の集約: fail が 1 つでもあれば fail / 全部 skipped なら skipped / それ以外 pass。"""
    if any(s == "fail" for s in statuses):
        return "fail"
    real = [s for s in statuses if s != "skipped"]
    if not real:
        return "skipped"
    return "pass"


# 視覚回帰(同じエンジンの前回承認画像との差)。決定論撮影なら 0 のはずなので厳しい。ドライバ更新の LSB 揺れだけ許す。
DEFAULT_REGRESSION = {
    "flip_ldr_mean_max": 0.002, "flip_ldr_p95_max": 0.01, "ssim_min": 0.995,
    "de2000_p95_max": 1.0, "lum_mean_ev_abs_max": 0.02,
}


def resolve_gate(spec: dict, stage: str) -> tuple[str, dict]:
    """--stage に G0/G1/G2 か、仕様の milestones(例 'Q2': 'G1')の名前、または 'regression' を受ける。"""
    if stage.lower() == "regression":
        return "REG", dict(spec.get("regression") or DEFAULT_REGRESSION)
    ms = spec.get("milestones") or {}
    name = ms.get(stage, stage)
    if name not in GATES:
        raise GateError(f"段階が不明: {stage}(G0 / G1 / G2 か、仕様の milestones のキー: {', '.join(ms) or 'なし'})")
    return name, dict((spec.get("gates") or {}).get(name, {}))


def gate_regions_for(spec: dict, gate: str) -> dict[str, dict]:
    out = {}
    for r in spec.get("regions") or []:
        g = (r.get("gates") or {}).get(gate)
        if g:
            out[r["name"]] = g
    return out


def fmt_value(v) -> str:
    if v is None or (isinstance(v, float) and math.isnan(v)):
        return "-"
    return f"{v:.4g}"
