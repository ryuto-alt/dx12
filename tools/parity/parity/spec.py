"""パリティシーン仕様(tools/parity/scenes/*.json)の読み込みと検証。

JSON Schema(scenes/schema.json)で形を検証し、その上でスキーマでは書けない意味の検査をする
(カメラ名の重複・領域の参照・しきい値キーの解釈・マスクの存在)。
"""
from __future__ import annotations

import copy
import hashlib
import json
import os
import re
from dataclasses import dataclass, field
from pathlib import Path

from . import gate as G
from .paths import TOOL_DIR

SCHEMA_PATH = TOOL_DIR / "scenes" / "schema.json"
_ENV = re.compile(r"\$\{([A-Za-z_][A-Za-z0-9_]*)\}")


class SpecError(Exception):
    pass


@dataclass
class Spec:
    data: dict
    path: Path | None = None
    warnings: list[str] = field(default_factory=list)

    # 便利アクセサ
    @property
    def id(self) -> str:
        return self.data["id"]

    @property
    def base_dir(self) -> Path:
        return self.path.parent if self.path else Path.cwd()

    @property
    def cameras(self) -> list[dict]:
        return [c for c in self.data["cameras"] if not c.get("skip")]

    @property
    def engine(self) -> dict:
        return self.data.get("engine") or {}

    @property
    def reference(self) -> dict:
        return self.data["reference"]

    @property
    def alignment(self) -> dict:
        return self.data.get("alignment") or {}

    @property
    def status(self) -> str:
        return self.data.get("status", "ready")

    def camera(self, name: str) -> dict:
        for c in self.data["cameras"]:
            if c["name"] == name:
                return c
        raise SpecError(f"カメラ '{name}' が仕様に無い(候補: {', '.join(c['name'] for c in self.data['cameras'])})")

    def hash(self) -> str:
        """基準画像キャッシュのキー用: 正規化 JSON + 領域マスクの内容。"""
        h = hashlib.sha256(json.dumps(self.data, sort_keys=True, ensure_ascii=False).encode("utf-8"))
        for r in self.data.get("regions") or []:
            p = Path(r["mask"])
            p = p if p.is_absolute() else self.base_dir / p
            if p.exists():
                h.update(p.read_bytes())
        return h.hexdigest()[:16]


def expand_env(s: str, missing: list[str] | None = None) -> str:
    def rep(m):
        v = os.environ.get(m.group(1))
        if v is None:
            if missing is not None:
                missing.append(m.group(1))
            return m.group(0)
        return v
    return _ENV.sub(rep, s)


def _schema() -> dict:
    return json.loads(SCHEMA_PATH.read_text(encoding="utf-8"))


def validate_data(data: dict) -> list[str]:
    """スキーマ違反の一覧(空 = OK)。"""
    try:
        import jsonschema
    except ImportError as e:  # pragma: no cover
        raise SpecError("jsonschema が入っていない(pip install jsonschema。tools/parity/setup.ps1)") from e
    v = jsonschema.Draft202012Validator(_schema())
    errs = []
    for e in sorted(v.iter_errors(data), key=lambda e: list(e.absolute_path)):
        loc = "/".join(str(x) for x in e.absolute_path) or "(root)"
        errs.append(f"{loc}: {e.message}")
    return errs


def semantic_check(data: dict) -> tuple[list[str], list[str]]:
    """(errors, warnings)。"""
    errs: list[str] = []
    warns: list[str] = []
    names = [c["name"] for c in data.get("cameras", [])]
    if len(set(names)) != len(names):
        errs.append("cameras: 名前が重複している")
    reg_names = [r["name"] for r in data.get("regions", [])]
    if len(set(reg_names)) != len(reg_names):
        errs.append("regions: 名前が重複している")
    for c in data.get("cameras", []):
        for r in c.get("regions", []) or []:
            if r not in reg_names:
                errs.append(f"cameras[{c['name']}].regions: 領域 '{r}' が regions に無い")
        for g, th in (c.get("gates") or {}).items():
            _check_thresholds(th, f"cameras[{c['name']}].gates.{g}", errs)
        if c.get("fovDeg") and abs(c["fovDeg"] - 45.0) > 0.01:
            warns.append(f"cameras[{c['name']}].fovDeg={c['fovDeg']}: set_editor_camera は FOV を変えられない(エディタ既定 45)。シーンのカメラ側で合わせること")
    for g, th in (data.get("gates") or {}).items():
        _check_thresholds(th, f"gates.{g}", errs)
    for r in data.get("regions", []) or []:
        for g, th in (r.get("gates") or {}).items():
            _check_thresholds(th, f"regions[{r['name']}].gates.{g}", errs)
    for m, g in (data.get("milestones") or {}).items():
        if g not in G.GATES:
            errs.append(f"milestones.{m}: {g} は G0/G1/G2 ではない")
    if data.get("reference", {}).get("kind") == "pt":
        pt = data["reference"].get("pt") or {}
        if not pt.get("spp"):
            warns.append("reference.pt.spp が無い(PT 側の既定に任せる。再現性のため明示を推奨)")
    return errs, warns


def _check_thresholds(th: dict, where: str, errs: list[str]) -> None:
    for k in th:
        if k.startswith("$"):
            continue
        try:
            G.parse_key(k)
        except G.GateError as e:
            errs.append(f"{where}: {e}")
        if not isinstance(th[k], (int, float)):
            errs.append(f"{where}.{k}: 数値でない")


def load_spec(path: str | Path, check_masks: bool = False) -> Spec:
    p = Path(path)
    if not p.exists():
        raise SpecError(f"シーン仕様が無い: {p}")
    try:
        data = json.loads(p.read_text(encoding="utf-8"))
    except json.JSONDecodeError as e:
        raise SpecError(f"{p.name}: JSON が壊れている: {e}") from e
    return from_data(data, p, check_masks)


def from_data(data: dict, path: Path | None = None, check_masks: bool = False) -> Spec:
    data = copy.deepcopy(data)
    errs = validate_data(data)
    if errs:
        raise SpecError(f"{path.name if path else 'spec'}: スキーマ違反 {len(errs)} 件\n  " + "\n  ".join(errs[:20]))
    se, sw = semantic_check(data)
    if se:
        raise SpecError(f"{path.name if path else 'spec'}: 仕様の矛盾 {len(se)} 件\n  " + "\n  ".join(se))
    spec = Spec(data, path, warnings=sw)
    if check_masks:
        for r in data.get("regions", []) or []:
            mp = Path(r["mask"])
            mp = mp if mp.is_absolute() else spec.base_dir / mp
            if not mp.exists():
                spec.warnings.append(f"regions[{r['name']}].mask が無い: {mp}")
    return spec


def list_specs(dir_: Path | None = None) -> list[Path]:
    d = dir_ or (TOOL_DIR / "scenes")
    return sorted(p for p in d.glob("*.json") if p.name != "schema.json")


def resolve_project(spec: Spec, project_override: str | None = None) -> tuple[Path | None, str | None, list[str]]:
    """scene.source=project の (プロジェクトのフォルダ, シーン相対パス, 未定義の環境変数)。generator/current は (None, None, [])。"""
    sc = spec.data["scene"]
    if sc["source"] != "project":
        return None, None, []
    missing: list[str] = []
    proj = expand_env(project_override or sc["project"], missing)
    return Path(proj), sc["scene"], missing
