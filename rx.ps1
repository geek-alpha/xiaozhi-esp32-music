param([int]$Seconds = 60, [string]$Out = 'D:\AI\tmp_xz_music\rx.log')
$p = New-Object System.IO.Ports.SerialPort 'COM19', 115200, 'None', 8, 'One'
$p.Encoding = [Text.Encoding]::UTF8
$p.ReadTimeout = 500
$p.Open()
$sw = [Diagnostics.Stopwatch]::StartNew()
$sb = New-Object Text.StringBuilder
while ($sw.Elapsed.TotalSeconds -lt $Seconds) {
    try { $sb.Append($p.ReadExisting()) | Out-Null } catch {}
    Start-Sleep -Milliseconds 100
}
$p.Close()
[IO.File]::WriteAllText($Out, $sb.ToString(), [Text.Encoding]::UTF8)
Write-Output "saved $Out"
