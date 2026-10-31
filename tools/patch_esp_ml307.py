# -*- coding: utf-8 -*-
"""让 esp-ml307 的 TLS 强制走 IPv4（幂等）。

managed_components/ 不入库，重装依赖后会回退成默认配置；esp-tls 只取 DNS
返回的第一个地址且不做 fallback，遇到 IPv6 优先的域名（jsDelivr、Cloudflare
后的站点）在无 IPv6 的板子上必然连接失败，所以每次拉完依赖都要重打这个补丁。
"""
import io
import os
import sys

TARGET = os.path.join("managed_components", "78__esp-ml307", "src", "esp", "esp_ssl.cc")
ANCHOR = "    cfg.crt_bundle_attach = esp_crt_bundle_attach;\n"
PATCH = ANCHOR + "    cfg.addr_family = ESP_TLS_AF_INET;\n"


def main() -> int:
    if not os.path.exists(TARGET):
        print("[skip] 未找到 %s" % TARGET)
        return 0

    text = io.open(TARGET, encoding="utf-8").read()
    if "ESP_TLS_AF_INET" in text:
        print("[ok] 已打过补丁")
        return 0
    if ANCHOR not in text:
        print("[fail] 锚点不匹配，组件可能已改版，需人工确认")
        return 1

    io.open(TARGET, "w", encoding="utf-8", newline="\n").write(text.replace(ANCHOR, PATCH, 1))
    print("[done] 已写入 cfg.addr_family = ESP_TLS_AF_INET")
    return 0


if __name__ == "__main__":
    sys.exit(main())
