#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""Graphite Gauge 仕様（final）の取り込み（W1-A2 / GG-1）。

  python -I tools/theme/sync_gg_spec.py --from <identity/gg/final のフォルダ>

取り込むのは `final/tokens.json` だけ（foundation / brand / motion / components を手で重ねる経路は作らない）。
`final/_build/integrate_check.json` の全項目が緑でなければ止まる。
書くもの:
  assets/editor/theme/gg/tokens.json         final/tokens.json のバイトコピー
  assets/editor/theme/gg/gg_spec.lock.json   tokens の SHA256、meta.version、integrate_check の SHA256 と結果、取得日時

仕様が改訂されたら、このスクリプトの再実行 → `python -I tools/theme/gen_gg_tokens.py` の再生成だけで追従する。
標準ライブラリだけで動く。
"""
import argparse
import datetime
import hashlib
import json
import pathlib
import shutil
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
DEST = ROOT / "assets" / "editor" / "theme" / "gg"


def sha256(path):
    # 改行は LF に正規化してから数える（Windows の autocrlf でチェックアウトされても同じ値になる）
    return hashlib.sha256(pathlib.Path(path).read_bytes().replace(bytes([13, 10]), bytes([10]))).hexdigest()


def check_green(chk):
    """integrate_check.json が全項目緑か。緑でない項目の説明の一覧を返す（空なら全緑）。"""
    bad = []
    if chk.get("contrastFails") != 0:
        bad.append("contrastFails != 0")
    if chk.get("cvdNG") != []:
        bad.append("cvdNG が空でない")
    if chk.get("yellowZoneViolations") != 0:
        bad.append("yellowZoneViolations != 0")
    for k in ("removedTokensAbsent", "cssHasNew", "reduceMotionKeepsWaits", "tabNoUnique"):
        if chk.get(k) is not True:
            bad.append(k + " が true でない")
    return bad


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--from", dest="src", required=True, help="GG の final フォルダ（tokens.json と _build/integrate_check.json がある）")
    args = ap.parse_args()
    src = pathlib.Path(args.src)
    tokens = src / "tokens.json"
    chk_path = src / "_build" / "integrate_check.json"
    for p in (tokens, chk_path):
        if not p.is_file():
            print("見つかりません: %s" % p.name, file=sys.stderr)
            return 2
    chk = json.loads(chk_path.read_text(encoding="utf-8"))
    bad = check_green(chk)
    if bad:
        print("integrate_check.json が全項目緑ではありません。取り込みを止めます:", file=sys.stderr)
        for b in bad:
            print("  - " + b, file=sys.stderr)
        return 3
    tok = json.loads(tokens.read_text(encoding="utf-8"))
    DEST.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(tokens, DEST / "tokens.json")
    lock = {
        "schema": "uno/gg_spec.lock@1",
        "tokensSha256": sha256(DEST / "tokens.json"),
        "metaVersion": tok["meta"]["version"],
        "integrateCheck": {"sha256": sha256(chk_path), "allGreen": True, "result": chk},
        "syncedAt": datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
    }
    (DEST / "gg_spec.lock.json").write_text(json.dumps(lock, ensure_ascii=False, indent=2) + "\n", encoding="utf-8", newline="\n")
    print("取り込み完了: tokens.json v%s sha256=%s" % (lock["metaVersion"], lock["tokensSha256"][:16]))
    print("次に: python -I tools/theme/gen_gg_tokens.py")
    return 0


if __name__ == "__main__":
    sys.exit(main())
