"""ambientCG(CC0)の 2K テクスチャを落としてキャッシュする。forge_bake.py が読む。

  python fetch_cc0.py                 # 既定セット(今の 13 種の 2K + 汚れ用)を揃える
  python fetch_cc0.py Metal032 Rust004 --fmt PNG
  python fetch_cc0.py --list          # キャッシュにあるもの

キャッシュ: %LOCALAPPDATA%\\UnoEngine\\cc0\\<id>\\<id>_{Color,NormalGL,Roughness,Metalness,AmbientOcclusion,Opacity}.png|jpg
          (必要な地図だけ zip から取り出す。zip は残さない。)
ライセンスは CC0 のみ(API の licence 表記を見て、CC0 以外は捨てる)。CREDITS.txt を同じ場所に書く。
"""
import argparse
import io
import json
import os
import sys
import time
import urllib.request
import zipfile

CACHE = os.path.join(os.environ.get("LOCALAPPDATA", os.path.expanduser("~")), "UnoEngine", "cc0")
API = "https://ambientcg.com/api/v2/full_json?id=%s&include=downloadData"
KEEP = ("Color", "NormalGL", "Roughness", "Metalness", "AmbientOcclusion", "Opacity")

# 今の小物が使う 13 種(BrickGen01 は ambientCG ではなく自作なので入れない)
BASE = ["PaintedMetal012", "Metal032", "Metal046A", "Rust004", "Plastic010", "Rubber004", "Cardboard004",
        "Paper001", "Wood051", "Concrete034", "Leather011", "Fabric030"]
# 汚れ・傷・液だれ系(グランジのまだら、指紋/ヤニ、傷、液だれ)。粗さ地図をマスクの崩しに使う
EXTRA = ["SurfaceImperfections001", "SurfaceImperfections003", "SurfaceImperfections013", "Smear002",
         "Fingerprints002", "Metal009", "Leaking004", "Leaking012A"]


def http(url, tries=3):
    last = None
    for i in range(tries):
        try:
            req = urllib.request.Request(url, headers={"User-Agent": "uno-forge/1.0"})
            with urllib.request.urlopen(req, timeout=120) as r:
                return r.read()
        except Exception as e:  # noqa: BLE001
            last = e
            time.sleep(2 + i * 3)
    raise last


def fetch(aid, fmt):
    d = os.path.join(CACHE, aid)
    if os.path.isdir(d) and any(f.endswith((".png", ".jpg")) and "_Color" in f for f in os.listdir(d)):
        print("  cached:", aid)
        return True
    info = json.loads(http(API % aid))
    found = info.get("foundAssets") or []
    if not found:
        print("  not found:", aid)
        return False
    a = found[0]
    lic = (a.get("license") or a.get("licence") or "CC0 1.0")
    if "CC0" not in str(lic).upper() and "creative commons zero" not in str(lic).lower():
        print("  skip (not CC0):", aid, lic)
        return False
    want = "2K-" + fmt
    link = None
    for dl in a["downloadFolders"]["default"]["downloadFiletypeCategories"]["zip"]["downloads"]:
        if dl["attribute"] == want:
            link = dl["downloadLink"]
    if not link:
        print("  no %s for %s" % (want, aid))
        return False
    t = time.time()
    z = zipfile.ZipFile(io.BytesIO(http(link)))
    os.makedirs(d, exist_ok=True)
    n = 0
    for name in z.namelist():
        stem, ext = os.path.splitext(name)
        key = stem.split("_")[-1]
        if key in KEEP and ext.lower() in (".png", ".jpg"):
            with open(os.path.join(d, "%s_%s%s" % (aid, key, ext.lower())), "wb") as f:
                f.write(z.read(name))
            n += 1
    with open(os.path.join(d, "meta.json"), "w", encoding="utf-8") as f:
        json.dump({"id": aid, "name": a.get("displayName"), "type": a.get("dataType"), "license": "CC0 1.0",
                   "url": "https://ambientcg.com/a/" + aid, "fmt": fmt}, f, ensure_ascii=False, indent=1)
    print("  ok: %s (%d maps, %.0fs)" % (aid, n, time.time() - t))
    return True


def write_credits():
    ids = sorted(x for x in os.listdir(CACHE) if os.path.isfile(os.path.join(CACHE, x, "meta.json")))
    with open(os.path.join(CACHE, "CREDITS.txt"), "w", encoding="utf-8") as f:
        f.write("CC0 1.0 textures from ambientCG (https://ambientcg.com). No attribution required; listed for the record.\n\n")
        for i in ids:
            m = json.load(open(os.path.join(CACHE, i, "meta.json"), encoding="utf-8"))
            f.write("%s  %s  %s\n" % (i, m.get("name", ""), m["url"]))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("ids", nargs="*")
    ap.add_argument("--fmt", default=None, help="PNG or JPG (既定: 基本は PNG、汚れ系は JPG)")
    ap.add_argument("--list", action="store_true")
    a = ap.parse_args()
    os.makedirs(CACHE, exist_ok=True)
    if a.list:
        for x in sorted(os.listdir(CACHE)):
            print(x)
        return
    todo = [(i, a.fmt or "PNG") for i in a.ids] if a.ids else \
        [(i, a.fmt or "PNG") for i in BASE] + [(i, a.fmt or "JPG") for i in EXTRA]
    bad = 0
    for aid, fmt in todo:
        print(aid)
        try:
            if not fetch(aid, fmt):
                bad += 1
        except Exception as e:  # noqa: BLE001
            print("  failed:", aid, e)
            bad += 1
    write_credits()
    print("done, failed=%d, cache=%s" % (bad, CACHE))
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
