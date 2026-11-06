import argparse
import re
import time

import serial


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("port")
    ap.add_argument("seconds", type=int, nargs="?", default=150)
    ap.add_argument("--log", default="ota_watch.log")
    args = ap.parse_args()

    ser = serial.Serial(args.port, 115200, timeout=0.3)
    ser.setDTR(False)
    ser.setRTS(False)
    t0 = time.time()
    speeds = []
    progress = []
    with open(args.log, "w", encoding="utf-8", errors="replace") as out:
        while time.time() - t0 < args.seconds:
            raw = ser.readline()
            if not raw:
                continue
            line = raw.decode("utf-8", "replace").rstrip()
            out.write(line + "\n")
            out.flush()
            m = re.search(r"Speed:\s*(\d+)B/s", line)
            if m:
                speeds.append(int(m.group(1)))
                print(line, flush=True)
            elif "Progress:" in line:
                progress.append(line)
                print(line, flush=True)
            elif re.search(r"(Upgrad|Ota|OTA|version)", line, re.I):
                print(line, flush=True)
    ser.close()

    print("---SUMMARY---", flush=True)
    if speeds:
        print(f"samples={len(speeds)} max={max(speeds)}B/s avg={sum(speeds)//len(speeds)}B/s", flush=True)
        print("first5=" + ",".join(str(s) for s in speeds[:5]), flush=True)
        print("last5=" + ",".join(str(s) for s in speeds[-5:]), flush=True)
    else:
        print("no Speed lines captured", flush=True)


if __name__ == "__main__":
    main()
