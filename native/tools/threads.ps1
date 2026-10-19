# Which threads of the process use CPU over a few seconds.
param([int]$Seconds = 10, [string]$Name = "Murmur")
$p = Get-Process -Name $Name -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $p) { "no process"; exit }
$a = @{}
foreach ($t in $p.Threads) { $a[$t.Id] = $t.TotalProcessorTime.TotalSeconds }
$t0 = [DateTime]::UtcNow
Start-Sleep -Seconds $Seconds
$p.Refresh()
$dt = ([DateTime]::UtcNow - $t0).TotalSeconds
$rows = foreach ($t in $p.Threads) {
  $d = $t.TotalProcessorTime.TotalSeconds - $(if ($a.ContainsKey($t.Id)) { $a[$t.Id] } else { 0 })
  [pscustomobject]@{ Id = $t.Id; CpuPct = [math]::Round(100 * $d / $dt, 2); State = $t.ThreadState; Wait = $t.WaitReason }
}
$rows | Sort-Object CpuPct -Descending | Select-Object -First 6 | Format-Table -AutoSize
