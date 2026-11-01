#!/bin/sh
# 烧全量镜像到 0x0。不传参数时自动取 ota/ 下版本最新的 *-full.bin。
# 用法：
#   sh scripts/flash.sh                  # 自动找包、自动找串口
#   sh scripts/flash.sh <bin> [port]     # 手动指定
set -e

cd "$(dirname "$0")/.."

BIN=${1:-$(ls -1 ota/*-full.bin | sort -V | tail -1)}
PORT=${2:-}

if [ -z "$BIN" ] || [ ! -f "$BIN" ]; then
    echo "找不到全量镜像，先跑 py -3 tools/make_full_bin.py" >&2
    exit 1
fi

echo "flash $BIN -> 0x0"
esptool.py --chip esp32s3 -b 460800 ${PORT:+-p "$PORT"} write_flash 0x0 "$BIN"
