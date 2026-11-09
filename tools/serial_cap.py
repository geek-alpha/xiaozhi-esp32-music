import sys
import time

import serial

port, dur, out = sys.argv[1], float(sys.argv[2]), sys.argv[3]
ser = serial.Serial(port, 115200, timeout=0.2)
end = time.time() + dur
with open(out, "wb") as f:
    while time.time() < end:
        chunk = ser.read(8192)
        if chunk:
            f.write(chunk)
            f.flush()
ser.close()
