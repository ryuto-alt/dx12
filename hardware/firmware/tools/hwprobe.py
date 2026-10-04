#!/usr/bin/env python3
"""hwprobe - UnoLink (プロトコル v1) 実機確認ツール。

  python hwprobe.py COM8 --seconds 5                 # @hello〜@ready と値の流れを見る
  python hwprobe.py COM8 "!led=0.5" --seconds 3      # 起動後に出力を送る（複数可。?pin ... も可）
  python hwprobe.py COM8 "!led=0.5" --ping-stop 2 --seconds 5
                                                     # 2 秒後に ?ping を止める（1 秒後に safe へ戻るか確認）
  python hwprobe.py COM8 "?pin 2 pwm led" "?pin 34 adc dial"   # 汎用スケッチ

DTR=False / RTS=False で開き、RTS を 0.12 秒だけ立ててリセットしてから起動ログを読む
（--no-reset で省略）。pyserial が無ければ: python -m pip install --user pyserial
"""
import argparse
import sys
import time

try:
    import serial
except ImportError:
    print("pyserial がありません: python -m pip install --user pyserial")
    sys.exit(2)


def main():
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(errors="replace")
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("port", help="COM ポート (例 COM8)")
    ap.add_argument("send", nargs="*", help="ready 後に送る行（例 !led=0.5 / ?pin 2 pwm led）")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--seconds", type=float, default=10.0, help="全体の実行秒数")
    ap.add_argument("--no-reset", action="store_true", help="RTS リセットをしない")
    ap.add_argument("--ping-stop", type=float, default=None, metavar="S",
                    help="ready の S 秒後に ?ping を止める（心拍断のテスト）")
    ap.add_argument("--hello-timeout", type=float, default=5.0)
    args = ap.parse_args()

    # 先に DTR/RTS を False にしてから open（ESP32 を書き込みモードに落とさない）
    ser = serial.Serial()
    ser.port = args.port
    ser.baudrate = args.baud
    ser.timeout = 0.02
    ser.dtr = False
    ser.rts = False
    try:
        ser.open()
    except Exception as e:
        print(f"open 失敗: {e}")
        return 2

    t0 = time.monotonic()

    def ts():
        return f"[{time.monotonic() - t0:6.2f}]"

    try:
        ser.reset_input_buffer()
        if not args.no_reset:
            ser.rts = True
            time.sleep(0.12)
            ser.rts = False
            print(f"{ts()} RTS リセット")

        buf = b""
        channels = []
        hello = None
        ready_at = None
        ready = False
        sent_extra = False
        ping_seq = 0
        next_ping = 0.0
        pings = {}
        rtts = []
        value_lines = 0
        last_vals = {}
        asked_hello = False
        errs = 0
        deadline = t0 + args.seconds

        while time.monotonic() < deadline:
            now = time.monotonic()

            # ---- 送信 ----
            if ready:
                stop_ping = args.ping_stop is not None and now - ready_at >= args.ping_stop
                if not sent_extra:
                    for line in args.send:
                        ser.write((line + "\n").encode("ascii"))
                        print(f"{ts()} -> {line}")
                    sent_extra = True
                if not stop_ping and now >= next_ping:
                    ping_seq += 1
                    pings[ping_seq] = now
                    ser.write(f"?ping {ping_seq}\n".encode("ascii"))
                    next_ping = now + 0.2
                if stop_ping and not getattr(main, "_stop_logged", False):
                    main._stop_logged = True
                    print(f"{ts()} (?ping を停止)")
            elif not asked_hello and now - t0 >= args.hello_timeout:
                print(f"{ts()} @hello が来ない -> ?hello を送る")
                ser.write(b"?hello\n")
                asked_hello = True

            # ---- 受信 ----
            data = ser.read(256)
            if not data:
                continue
            buf += data
            while b"\n" in buf:
                raw, buf = buf.split(b"\n", 1)
                line = raw.replace(b"\r", b"").decode("ascii", "replace").strip()
                if not line:
                    continue
                if line.startswith("@hello"):
                    hello = line
                    channels = []
                    ready = False
                    print(f"{ts()} {line}")
                elif line.startswith("@ch "):
                    channels.append(line)
                    print(f"{ts()} {line}")
                elif line == "@ready":
                    ready = True
                    ready_at = time.monotonic()
                    next_ping = 0.0
                    print(f"{ts()} @ready  (チャンネル {len(channels)} 本)")
                elif line.startswith("@pong"):
                    try:
                        seq = int(line.split()[1])
                        if seq in pings:
                            rtts.append((time.monotonic() - pings.pop(seq)) * 1000)
                    except (IndexError, ValueError):
                        pass
                elif line.startswith("@log"):
                    print(f"{ts()} {line}")
                elif line.startswith("@err"):
                    errs += 1
                    print(f"{ts()} {line}")
                elif line[0].isalpha():
                    value_lines += 1
                    for tok in line.split():
                        if "=" in tok:
                            k, v = tok.split("=", 1)
                            if last_vals.get(k) != v:
                                print(f"{ts()} {k}={v}")
                            last_vals[k] = v
                else:
                    print(f"{ts()} ?? {line}")

        print("---- 結果 ----")
        print(f"hello: {hello or '(なし)'}")
        print(f"channels: {len(channels)}  ready: {ready}")
        print(f"value lines: {value_lines}  errs: {errs}  last: {last_vals}")
        if rtts:
            print(f"pong {len(rtts)}/{ping_seq}  RTT min/avg/max = "
                  f"{min(rtts):.1f}/{sum(rtts)/len(rtts):.1f}/{max(rtts):.1f} ms")
        return 0 if (hello and ready) else 1
    finally:
        ser.close()


if __name__ == "__main__":
    sys.exit(main())
