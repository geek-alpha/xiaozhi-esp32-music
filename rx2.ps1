param([int]$Seconds = 120, [string]$Out = 'D:\AI\tmp_xz_music\live.log')
$p = New-Object System.IO.Ports.SerialPort 'COM19', 115200, 'None', 8, 'One'
$p.Encoding = [Text.Encoding]::UTF8
$p.ReadTimeout = 500
$p.Open()
$fs = [IO.File]::Open($Out, 'Create', 'Write', 'ReadWrite')
$sw_out = New-Object IO.StreamWriter($fs)
$sw_out.AutoFlush = $true
$sw = [Diagnostics.Stopwatch]::StartNew()
while ($sw.Elapsed.TotalSeconds -lt $Seconds) {
    try { $s = $p.ReadExisting(); if ($s.Length -gt 0) { $sw_out.Write($s) } } catch {}
    Start-Sleep -Milliseconds 80
}
$sw_out.Flush(); $sw_out.Close(); $fs.Close(); $p.Close()
Write-Output "saved $Out"
