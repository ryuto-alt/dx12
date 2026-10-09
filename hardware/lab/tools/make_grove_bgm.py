"""GroveLab の BGM（assets/bgm/1.mid〜9.mid）を作る。

    python hardware/lab/tools/make_grove_bgm.py

曲はどれも著作権の切れた曲（作曲者の没後 70 年以上・民謡）か、このスクリプトのオリジナル。
メロディはチャンネル 1、ベースはチャンネル 2。GroveSpeaker は 1 音しか出せないので、
GroveJukebox はいちばん高い音（＝メロディ）を拾う（アルペジオ ON のときはベースも混ぜる）。
音符は (音名, 拍) の並び。音名は "C5" "F#4" "Bb3"、休符は "r"。
"""
import os
import struct

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "..", "GroveLab", "assets", "bgm")
PPQ = 480

_NAMES = {"C": 0, "D": 2, "E": 4, "F": 5, "G": 7, "A": 9, "B": 11}


def midi_note(name):
    pitch = _NAMES[name[0]]
    rest = name[1:]
    while rest and rest[0] in "#b":
        pitch += 1 if rest[0] == "#" else -1
        rest = rest[1:]
    return (int(rest) + 1) * 12 + pitch


def parse(seq):
    """ "E5 1 | B4 .5" のような文字列 → [(音名, 拍)]。| は小節線（読み飛ばす）"""
    toks = [t for t in seq.replace("|", " ").split() if t]
    assert len(toks) % 2 == 0, seq
    return [(toks[i], float(toks[i + 1])) for i in range(0, len(toks), 2)]


def bars_to_seq(bars, beats_per_bar, octave_note=None):
    """ベース: 小節ごとの根音リスト（["C3"] や ["F2","G2"]）を、小節を等分した長さの音符にする"""
    out = []
    for roots in bars:
        d = beats_per_bar / len(roots)
        out += [(r, d) for r in roots]
    return out


def varlen(n):
    b = [n & 0x7F]
    n >>= 7
    while n:
        b.insert(0, (n & 0x7F) | 0x80)
        n >>= 7
    return bytes(b)


def track(events):
    """events: [(tick, bytes)] → MTrk"""
    events = sorted(events, key=lambda e: (e[0], e[2] if len(e) > 2 else 0))
    data = b""
    last = 0
    for e in events:
        data += varlen(e[0] - last) + e[1]
        last = e[0]
    data += varlen(0) + b"\xFF\x2F\x00"
    return b"MTrk" + struct.pack(">I", len(data)) + data


def notes_track(seq, ch, vel, legato=0.9, transpose=0):
    ev = []
    t = 0
    for name, beats in seq:
        dur = int(round(beats * PPQ))
        if name != "r":
            n = midi_note(name) + transpose
            ev.append((t, bytes([0x90 | ch, n, vel]), 1))
            ev.append((t + max(1, int(dur * legato)), bytes([0x80 | ch, n, 0]), 0))
        t += dur
    return ev, t


def write_song(slot, title, tempos, melody, bass=None, repeat=1):
    """tempos: [(拍, bpm)] / melody, bass: [(音名, 拍)]。repeat 回つなげる"""
    mel, length = [], 0
    for _ in range(repeat):
        ev, t = notes_track(melody, 0, 100)
        mel += [(a + length, b, c) for a, b, c in ev]
        length += t
    tr0 = [(0, b"\xFF\x03" + varlen(len(title.encode("utf-8"))) + title.encode("utf-8"))]
    for beat, bpm in tempos:
        us = int(round(60_000_000 / bpm))
        tr0.append((int(beat * PPQ), b"\xFF\x51\x03" + us.to_bytes(3, "big")))
    tracks = [track(tr0), track(mel)]
    if bass:
        bev, blen = [], 0
        while blen < length:
            ev, t = notes_track(bass, 1, 80)
            bev += [(a + blen, b, c) for a, b, c in ev if a + blen < length]
            blen += t
        tracks.append(track(bev))
    head = b"MThd" + struct.pack(">IHHH", 6, 1, len(tracks), PPQ)
    path = os.path.join(OUT, f"{slot}.mid")
    with open(path, "wb") as f:
        f.write(head + b"".join(tracks))
    print(f"{slot}.mid  {title}  ({length / PPQ:.0f} 拍)")


def main():
    os.makedirs(OUT, exist_ok=True)

    # 1. きらきら星（フランス民謡）
    a = "C5 1 C5 1 G5 1 G5 1 | A5 1 A5 1 G5 2 | F5 1 F5 1 E5 1 E5 1 | D5 1 D5 1 C5 2 |"
    b = "G5 1 G5 1 F5 1 F5 1 | E5 1 E5 1 D5 2 |"
    write_song(1, "きらきら星", [(0, 100)], parse(a + b + b + a),
               bars_to_seq([["C3"], ["F3", "C3"], ["F3", "C3"], ["G3", "C3"],
                            ["C3", "F3"], ["C3", "G3"], ["C3", "F3"], ["C3", "G3"],
                            ["C3"], ["F3", "C3"], ["F3", "C3"], ["G3", "C3"]], 4))

    # 2. 歓喜の歌（ベートーヴェン 交響曲第 9 番）
    a = "E5 1 E5 1 F5 1 G5 1 | G5 1 F5 1 E5 1 D5 1 | C5 1 C5 1 D5 1 E5 1 |"
    write_song(2, "歓喜の歌", [(0, 120)], parse(
        a + "E5 1.5 D5 .5 D5 2 |" + a + "D5 1.5 C5 .5 C5 2 |"
        "D5 1 D5 1 E5 1 C5 1 | D5 1 E5 .5 F5 .5 E5 1 C5 1 | D5 1 E5 .5 F5 .5 E5 1 D5 1 | C5 1 D5 1 G4 2 |"
        + a + "D5 1.5 C5 .5 C5 2 |"),
        bars_to_seq([["C3"], ["G2"], ["C3"], ["C3", "G2"], ["C3"], ["G2"], ["C3"], ["G2", "C3"],
                     ["G2"], ["G2", "C3"], ["G2"], ["C3", "G2"], ["C3"], ["G2"], ["C3"], ["G2", "C3"]], 4))

    # 3. エリーゼのために（ベートーヴェン、冒頭）
    s = .25
    m = f"E5 {s} D#5 {s} | E5 {s} D#5 {s} E5 {s} B4 {s} D5 {s} C5 {s} | A4 .5 r {s} C4 {s} E4 {s} A4 {s} | B4 .5 r {s} E4 {s} G#4 {s} B4 {s} |"
    end1 = f"C5 .5 r {s} E4 {s} E5 {s} D#5 {s} | E5 {s} D#5 {s} E5 {s} B4 {s} D5 {s} C5 {s} | A4 .5 r {s} C4 {s} E4 {s} A4 {s} | B4 .5 r {s} E4 {s} C5 {s} B4 {s} |"
    mid = (f"A4 .5 r {s} B4 {s} C5 {s} D5 {s} | E5 .75 G4 {s} F5 {s} E5 {s} | D5 .75 F4 {s} E5 {s} D5 {s} |"
           f" C5 .75 E4 {s} D5 {s} C5 {s} | B4 .5 r {s} E4 {s} E5 {s} D#5 {s} |")
    write_song(3, "エリーゼのために", [(0, 66)], parse(m + end1 + mid + m + end1 + "A4 1.5 r .75"))

    # 4. 天国と地獄（オッフェンバック「地獄のオルフェ」ギャロップ）
    e = .5
    a = f"C5 2 | D5 {e} F5 {e} E5 {e} D5 {e} | G5 1 G5 1 | G5 {e} A5 {e} E5 {e} F5 {e} | D5 1 D5 1 | D5 {e} F5 {e} E5 {e} D5 {e} |"
    write_song(4, "天国と地獄", [(0, 150)], parse(
        a + f"C5 {e} C6 {e} B5 {e} A5 {e} | G5 {e} F5 {e} E5 {e} D5 {e} |"
        + a + f"C5 {e} G5 {e} D5 {e} E5 {e} | C5 1 r 1 |"),
        bars_to_seq([["C3", "G2"], ["G2", "G2"], ["G2", "D3"], ["C3", "C3"]] * 4, 2))

    # 5. 山の魔王の宮殿（グリーグ「ペール・ギュント」）— だんだん速く、だんだん高く
    def theme(o):
        n = lambda x: x[:-1] + str(int(x[-1]) + o)
        return (f"{n('B3')} .5 {n('C#4')} .5 {n('D4')} .5 {n('E4')} .5 {n('F#4')} .5 {n('D4')} .5 {n('F#4')} 1 |"
                f" {n('F4')} .5 {n('C#4')} .5 {n('F4')} 1 {n('E4')} .5 {n('C4')} .5 {n('E4')} 1 |"
                f" {n('B3')} .5 {n('C#4')} .5 {n('D4')} .5 {n('E4')} .5 {n('F#4')} .5 {n('D4')} .5 {n('F#4')} .5 {n('B4')} .5 |"
                f" {n('A4')} .5 {n('F#4')} .5 {n('D4')} .5 {n('F#4')} .5 {n('A4')} 2 |")
    write_song(5, "山の魔王の宮殿", [(0, 90), (16, 110), (32, 140), (48, 180), (64, 220)],
               parse(theme(1) + theme(1) + theme(2) + theme(2) + theme(2) + "B5 2 r 2"))

    # 6. コロブチカ（ロシア民謡）
    write_song(6, "コロブチカ", [(0, 140)], parse(
        "E5 1 B4 .5 C5 .5 D5 1 C5 .5 B4 .5 | A4 1 A4 .5 C5 .5 E5 1 D5 .5 C5 .5 | B4 1.5 C5 .5 D5 1 E5 1 | C5 1 A4 1 A4 2 |"
        "r .5 D5 1 F5 .5 A5 1 G5 .5 F5 .5 | E5 1.5 C5 .5 E5 1 D5 .5 C5 .5 | B4 1 B4 .5 C5 .5 D5 1 E5 1 | C5 1 A4 1 A4 1 r 1 |"),
        bars_to_seq([["E2"], ["A2"], ["E2"], ["A2"], ["D3"], ["A2"], ["E2"], ["A2"]], 4), repeat=2)

    # 7. グリーンスリーブス（イングランド民謡）
    a = ("C5 1 D5 .5 E5 .75 F5 .25 E5 .5 | D5 1 B4 .5 G4 .75 A4 .25 B4 .5 |")
    write_song(7, "グリーンスリーブス", [(0, 120)], parse(
        "A4 .5 |" + a + "C5 1 A4 .5 A4 .75 G#4 .25 A4 .5 | B4 1 G#4 .5 E4 1 A4 .5 |"
        + a + "C5 .75 B4 .25 A4 .5 G#4 .75 F#4 .25 G#4 .5 | A4 1.5 A4 1 r .5 |"
        "G5 1.5 G5 .75 F#5 .25 E5 .5 | D5 1 B4 .5 G4 .75 A4 .25 B4 .5 | C5 1 A4 .5 A4 .75 G#4 .25 A4 .5 | B4 1 G#4 .5 E4 1.5 |"
        "G5 1.5 G5 .75 F#5 .25 E5 .5 | D5 1 B4 .5 G4 .75 A4 .25 B4 .5 | C5 .75 B4 .25 A4 .5 G#4 .75 F#4 .25 G#4 .5 | A4 1.5 A4 1.5 |"))

    # 8. オリジナル「爆弾処理班」— 半音ずつ上がって焦らせるループ
    e = .5
    def ost(root, hi):
        r = root
        return " ".join(f"{r} {e} {h} {e}" for h in hi) + " |"
    write_song(8, "爆弾処理班（オリジナル）", [(0, 150)], parse(
        ost("E4", ["E5", "D5", "C5", "B4"]) + ost("E4", ["E5", "D5", "C5", "A#4"])
        + ost("F4", ["F5", "D#5", "C#5", "C5"]) + ost("F4", ["F5", "D#5", "C#5", "B4"])
        + ost("F#4", ["F#5", "E5", "D5", "C#5"]) + ost("G4", ["G5", "F5", "D#5", "D5"])
        + f"B4 {e} C5 {e} B4 {e} C5 {e} B4 {e} C5 {e} B4 {e} C5 {e} | E5 2 r 2 |"))

    # 9. オリジナル「8bit 冒険」
    write_song(9, "8bit 冒険（オリジナル）", [(0, 160)], parse(
        "C5 .5 E5 .5 G5 .5 C6 .5 B5 1 G5 1 | A5 .5 G5 .5 F5 .5 E5 .5 D5 2 |"
        "E5 .5 F5 .5 G5 1 A5 .5 G5 .5 E5 1 | D5 .5 E5 .5 C5 1 r 2 |"
        "F5 .5 A5 .5 C6 1 B5 .5 A5 .5 G5 1 | E5 .5 G5 .5 C6 1 D6 .5 C6 .5 B5 1 |"
        "A5 .5 B5 .5 C6 .5 A5 .5 G5 1 E5 1 | F5 .5 D5 .5 B4 .5 D5 .5 C5 2 |"),
        bars_to_seq([["C3"], ["F2", "G2"], ["C3"], ["G2", "C3"], ["F2"], ["C3"], ["F2", "C3"], ["G2", "C3"]], 4))


if __name__ == "__main__":
    main()
