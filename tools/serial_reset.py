import sys
import time

import serial


def main():
    port = sys.argv[1] if len(sys.argv) > 1 else "COM19"
    ser = serial.Serial(port, 115200, timeout=0.2)
    ser.setDTR(False)
    ser.setRTS(True)
    time.sleep(0.15)
    ser.setRTS(False)
    time.sleep(0.3)
    ser.close()
    print("reset pulsed on", port)


if __name__ == "__main__":
    main()
