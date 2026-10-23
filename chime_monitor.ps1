$ErrorActionPreference = 'SilentlyContinue'
$file = 'C:\Users\GUO-notebook\.openclaw\workspace\xiaozhi-esp32\111.txt'
$report = 'C:\Users\GUO-notebook\.openclaw\workspace\xiaozhi-esp32\chime_monitor_report.txt'
$state = 'C:\Users\GUO-notebook\.openclaw\workspace\xiaozhi-esp32\chime_monitor_offset.txt'

# Specific first, then broad catch-alls (case-insensitive substring match, break after first hit)
$keywords = @(
  'Hourly chime trigger',
  'Half-hour chime trigger',
  'Skipping chime',
  'quiet_mode=',
  'Starting performance',
  'Performance: ENTER',
  'Performance complete',
  'is_dark',
  'LDR',
  'SetTime',
  'NTP',
  'Auto-synced',
  'Heartbeat t=',
  'Reset reason',
  'crash',
  'WDT',
  'E ('
)

function Write-Report([string]$msg) {
  $ts = Get-Date -Format 'yyyy-MM-dd HH:mm:ss'
  Add-Content -Path $report -Value ("{0} | {1}" -f $ts, $msg) -Encoding UTF8
}

$endTime = Get-Date -Year 2026 -Month 8 -Day 3 -Hour 8 -Minute 0 -Second 0
$lastOffset = 0
if (Test-Path $state) {
  try { $lastOffset = [int64](Get-Content $state -Raw) } catch { $lastOffset = 0 }
}
if (-not (Test-Path $report)) {
  Write-Report ("MONITOR START offset={0} end={1}" -f $lastOffset, $endTime.ToString('yyyy-MM-dd HH:mm:ss'))
} else {
  Write-Report ("MONITOR RESUME offset={0}" -f $lastOffset)
}

$utf8 = New-Object System.Text.UTF8Encoding($false, $true)
$gbk = [System.Text.Encoding]::GetEncoding(936)
$lastSnap = Get-Date

while ((Get-Date) -lt $endTime) {
  Start-Sleep -Seconds 60
  if (-not (Test-Path $file)) { continue }
  $size = (Get-Item $file).Length
  if ($size -lt $lastOffset) {
    Write-Report ("FILE RESET: size={0} < offset={1} (file truncated/rotated)" -f $size, $lastOffset)
    $lastOffset = 0
  }
  if ($size -le $lastOffset) { continue }
  try {
    $fs = [System.IO.File]::Open($file, 'Open', 'Read', 'ReadWrite')
    $fs.Position = $lastOffset
    $remaining = $fs.Length - $fs.Position
    $buf = New-Object byte[] $remaining
    $total = 0
    while ($total -lt $remaining) {
      $n = $fs.Read($buf, $total, $remaining - $total)
      if ($n -le 0) { break }
      $total += $n
    }
    $fs.Close()
    $lastOffset += $total
    $text = ''
    try { $text = $utf8.GetString($buf) } catch { $text = $gbk.GetString($buf) }
    $lines = $text -split "`r?`n"
    foreach ($line in $lines) {
      $l = $line.Trim()
      if ($l.Length -eq 0) { continue }
      foreach ($kw in $keywords) {
        if ($l.IndexOf($kw, [System.StringComparison]::OrdinalIgnoreCase) -ge 0) {
          Write-Report ("MATCH: " + $l)
          break
        }
      }
    }
  } catch {
    Write-Report ("READ ERROR: " + $_.Exception.Message)
  }
  Set-Content -Path $state -Value $lastOffset -Encoding ASCII
  # 30-min snapshot for device-time <-> PC-time correlation
  if (((Get-Date) - $lastSnap).TotalMinutes -ge 30) {
    Write-Report ("SNAPSHOT size={0} offset={1}" -f $size, $lastOffset)
    $lastSnap = Get-Date
  }
}

# Final summary
$matchCount = @{}
if (Test-Path $report) {
  $all = Get-Content $report
  $matchLines = $all | Where-Object { $_ -match 'MATCH: ' }
  Write-Report ("=== MONITOR END @ {0} ===" -f (Get-Date -Format 'yyyy-MM-dd HH:mm:ss'))
  Write-Report ("=== FINAL SUMMARY: total matched lines = {0} ===" -f $matchLines.Count)
  foreach ($kw in $keywords) {
    $c = ($matchLines | Where-Object { $_ -match [regex]::Escape($kw) }).Count
    if ($c -gt 0) { Write-Report ("COUNT {0} = {1}" -f $kw, $c) }
  }
}
