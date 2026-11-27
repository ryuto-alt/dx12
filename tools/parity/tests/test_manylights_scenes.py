"""SPK-1 用の多灯シーン(scenes/manylights/ml_64, ml_256)の形の検査。エンジンは起動しない。"""
from __future__ import annotations

import json
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / "gen"))

from parity import gi_scene as GS  # noqa: E402

DIR = HERE.parent / "scenes" / "manylights"


def _points(s):
    return [e for e in s.entities if e["kind"] == "light" and e["light"] == "point"]


def test_light_counts_and_unique_names():
    scenes = {s.id: s for s in GS.discover_scenes(DIR)}
    assert set(scenes) == {"ML-64", "ML-256"}
    assert len(_points(scenes["ML-64"])) == 64
    assert len(_points(scenes["ML-256"])) == 256
    for s in scenes.values():
        names = [e["name"] for e in s.entities]
        assert len(names) == len(set(names))


def test_no_shadow_casting_and_within_engine_budget():
    for s in GS.discover_scenes(DIR):
        pts = _points(s)
        assert len(pts) <= 1024                                   # kMaxSceneLights
        assert all(not e["components"]["pointLight"]["castShadows"] for e in pts)
        # 1 クラスタ 128 灯の上限: 各灯の範囲に入る灯の数(自分を含む)が余裕を持って 128 未満
        for e in pts:
            r = e["components"]["pointLight"]["range"]
            near = sum(1 for o in pts if sum((a - b) ** 2 for a, b in zip(e["at"], o["at"])) <= (2 * r) ** 2)
            assert near < 128


def test_cameras_inside_and_regions_resolve():
    for s in GS.discover_scenes(DIR):
        for c in s.cameras:
            x, y, z = c["position"]
            assert -12.0 < x < 12.0 and 0.5 < y < 3.0 and -42.0 < z < 12.0
        names = {e["name"] for e in s.entities}
        for r in s.data["regions"].values():
            assert set(r["entities"]) <= names


def test_lights_clear_of_occluders():
    """全灯が遮蔽物(箱・球の AABB)の外で、表面から 0.3m 以上離れている(中に閉じた灯・壁際の灯は基準線と PT をずらす)。"""
    import manylights_scenes as M
    occ = M.occluders()
    for s in GS.discover_scenes(DIR):
        for e in _points(s):
            assert M.blocked(e["at"], occ) is None, (s.id, e["name"], M.blocked(e["at"], occ))


def test_generator_is_deterministic(tmp_path, monkeypatch):
    import manylights_scenes as M
    monkeypatch.setattr(sys, "argv", ["x", "--out", str(tmp_path)])
    M.main()
    for f in ("ml_64.json", "ml_256.json"):
        assert (tmp_path / f).read_text(encoding="utf-8") == (DIR / f).read_text(encoding="utf-8")
    assert json.loads((DIR / "ml_64.json").read_text(encoding="utf-8"))["spike"]["id"] == "SPK-1"
