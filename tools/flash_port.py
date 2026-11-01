# -*- coding: utf-8 -*-
"""自动探测 ESP32-S3 串口，避免脚本里写死 COM 号。

优先级：ESP32-S3 原生 USB-Serial/JTAG (303A:xxxx) > 其它 Espressif 串口 > 唯一可用串口。
只输出一行端口名（如 COM22），供 bat 用 for /f 捕获；探测不到时退出码 1。

用法：
    py -3 tools/flash_port.py
"""
import sys

from serial.tools import list_ports

ESP_VID = 0x303A
KNOWN_UART_VIDS = (0x10C4, 0x1A86, 0x0403)  # CP210x / CH34x / FTDI


def rank(p):
    if p.vid == ESP_VID:
        return 0
    if p.vid in KNOWN_UART_VIDS:
        return 1
    return 2


def main() -> int:
    ports = [p for p in list_ports.comports() if p.device]
    if not ports:
        print("没有可用串口", file=sys.stderr)
        return 1

    cands = [p for p in ports if p.vid in (ESP_VID,) + KNOWN_UART_VIDS]
    if not cands:
        if len(ports) > 1:
            print("多个串口且都不是 Espressif 设备：%s" % ", ".join(p.device for p in ports), file=sys.stderr)
            return 1
        cands = ports

    cands.sort(key=lambda p: (rank(p), p.device))
    best = cands[0]
    print(best.device)
    print("[port] %s  VID:PID=%04X:%04X  %s" % (best.device, best.vid or 0, best.pid or 0,
                                               best.description or ""), file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
