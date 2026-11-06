# Probe CN radio streams: pull 8s each, report rate/latency (64k stream needs >=8KB/s)
$ErrorActionPreference = 'SilentlyContinue'

$urls = @(
    @('cnr1_zgzs',      'https://lhttp.qtfm.cn/live/15318317/64k.mp3'),
    @('huaiji_music',   'https://lhttp.qingting.fm/live/4804/64k.mp3'),
    @('qingchen_music', 'http://lhttp.qingting.fm/live/4915/64k.mp3'),
    @('intl_news',      'https://lhttp.qtfm.cn/live/20500172/64k.mp3'),
    @('liangguang',     'https://lhttp.qtfm.cn/live/20500149/64k.mp3'),
    @('huayu_500',      'https://lhttp.qtfm.cn/live/5022308/64k.mp3'),
    @('dongting',       'https://lhttp-hw.qtfm.cn/live/5022107/64k.mp3'),
    @('xingkong',       'https://lhttp-hw.qtfm.cn/live/5022379/64k.mp3'),
    @('gd_music',       'https://lhttp.qtfm.cn/live/1260/64k.mp3'),
    @('js_classic',     'https://lhttp.qtfm.cn/live/4938/64k.mp3'),
    @('asiafm_yue',     'https://lhttp.qtfm.cn/live/15318569/64k.mp3'),
    @('asiafm_hd',      'http://asiafm.hk:8000/asiahd'),
    @('deyunshe',       'https://stream.zeno.fm/yqawwmweq8mtv')
)

$jobs = foreach ($u in $urls) {
    Start-Job -ScriptBlock {
        param($name, $url)
        $out = & curl.exe -s -o NUL --max-time 8 -w '%{http_code}|%{size_download}|%{speed_download}|%{time_starttransfer}|%{content_type}' $url 2>&1
        "$name|$out"
    } -ArgumentList $u[0], $u[1]
}

$jobs | Wait-Job -Timeout 25 | Out-Null
$jobs | ForEach-Object { Receive-Job $_ }
$jobs | Remove-Job -Force
