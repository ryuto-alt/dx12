#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
起動演出の確認用 mp4 を作る（オーナーへ送る用。W1-NB5。EXEC §2.8）。窓は出さない・実音も鳴らさない。

  python tools/splash_av_preview.py <dump_dir> -o boot_calibrate.mp4 [--audio <wav>] [--fps 60] [--ffmpeg <path>] [--dry-run]

入力は check_splash_av.py と同じダンプ（frames.csv + PNG + av.json の audio / audioOriginMs）。
  - 絵は frames.csv の t（正規タイムライン）どおりの長さで並べる（可変フレーム間隔でも時間がずれない）。
  - 音は av.json の audioOriginMs だけ絵の先頭からずらして合わせる（--audio を省くと av.json の audio）。
  - ffmpeg は --ffmpeg、環境変数 FFMPEG、PATH の順に探す。見つからなければ明記して終了コード 2。
  - 出力は H.264 / yuv420p / AAC（Windows の標準プレーヤーと Discord で再生できる設定）。
"""
from __future__ import annotations

import argparse
import csv
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path


def read_rows(dump: Path):
    with open(dump / "frames.csv", encoding="utf-8", newline="") as f:
        rows = list(csv.DictReader(f))
    rows.sort(key=lambda r: float(r["t"]))
    return rows


def write_concat_list(list_path: Path, base: Path, rows, tail_s: float = 0.5) -> int:
    """ffmpeg の concat デマルチプレクサ用。各フレームの長さ = 次のフレームの t との差。最後は tail_s。"""
    lines = []
    for i, r in enumerate(rows):
        dur = (float(rows[i + 1]["t"]) - float(r["t"])) if i + 1 < len(rows) else tail_s
        p = (base / r["file"]).resolve().as_posix().replace("'", r"'\''")
        lines.append(f"file '{p}'")
        lines.append(f"duration {dur:.6f}")
    # concat の仕様: 最後のファイルをもう一度書かないと最後の duration が効かない
    last = (base / rows[-1]["file"]).resolve().as_posix().replace("'", r"'\''")
    lines.append(f"file '{last}'")
    list_path.write_text("\n".join(lines) + "\n", encoding="utf-8")
    return len(rows)


def build_command(ffmpeg, list_path: Path, wav: Path | None, out: Path, audio_origin_ms: float, first_t_ms: float, fps: float = 60.0):
    delay = audio_origin_ms - first_t_ms     # 正: 音は絵の先頭より後に始まる / 負: 音の頭を捨てる
    cmd = [str(ffmpeg), "-y", "-hide_banner", "-loglevel", "error", "-f", "concat", "-safe", "0", "-i", str(list_path)]
    if wav is not None:
        cmd += ["-i", str(wav)]
    cmd += ["-vf", f"fps={fps:g},scale=trunc(iw/2)*2:trunc(ih/2)*2,format=yuv420p", "-c:v", "libx264", "-crf", "18", "-preset", "medium"]
    if wav is not None:
        if delay > 0:
            cmd += ["-af", f"adelay={delay:.3f}:all=1"]
        elif delay < 0:
            cmd += ["-af", f"atrim=start={-delay / 1000.0:.6f},asetpts=PTS-STARTPTS"]
        cmd += ["-c:a", "aac", "-b:a", "192k", "-shortest"]
    cmd += ["-movflags", "+faststart", str(out)]
    return cmd


def find_ffmpeg(explicit: str | None):
    for c in (explicit, os.environ.get("FFMPEG")):
        if c and Path(c).exists():
            return c
    return shutil.which("ffmpeg")


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("dump", type=Path)
    ap.add_argument("-o", "--out", type=Path, required=True)
    ap.add_argument("--audio", type=Path)
    ap.add_argument("--fps", type=float)
    ap.add_argument("--ffmpeg")
    ap.add_argument("--dry-run", action="store_true", help="ffmpeg を実行せず、コマンドだけ表示する")
    a = ap.parse_args(argv)

    rows = read_rows(a.dump)
    if not rows:
        print("frames.csv が空です", file=sys.stderr)
        return 1
    av = json.loads((a.dump / "av.json").read_text(encoding="utf-8")) if (a.dump / "av.json").exists() else {}
    wav = a.audio or ((a.dump / av["audio"]) if av.get("audio") else None)
    fps = a.fps or float(av.get("fps", 60))
    list_path = a.out.with_suffix(".concat.txt")
    write_concat_list(list_path, a.dump, rows)
    ff = find_ffmpeg(a.ffmpeg) or "ffmpeg"
    cmd = build_command(ff, list_path, wav, a.out, float(av.get("audioOriginMs", 0.0)), float(rows[0]["t"]) * 1000.0, fps)
    if a.dry_run:
        print(" ".join(cmd))
        return 0
    if not find_ffmpeg(a.ffmpeg):
        print("ffmpeg が見つかりません（--ffmpeg か環境変数 FFMPEG で指定してください）", file=sys.stderr)
        return 2
    try:
        subprocess.run(cmd, check=True)
    finally:
        list_path.unlink(missing_ok=True)
    print(f"書き出しました: {a.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
