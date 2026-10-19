# Samples CPU and memory of Murmur (and the mpv it starts) for a few seconds.
#   measure.ps1 [-Seconds 10] [-Name murmur_dev]
param([int]$Seconds = 10, [string]$Name = "murmur_dev")
function Sum($procs, $prop) { $s = 0.0; foreach ($x in @($procs)) { if ($x) { $s += $prop.Invoke($x) } }; return $s }
function Snap {
  $m = @(Get-Process -Name $Name -ErrorAction SilentlyContinue)
  $p = @(Get-Process -Name mpv -ErrorAction SilentlyContinue)
  $r = [pscustomobject]@{ t = [DateTime]::UtcNow; mCpu = 0.0; pCpu = 0.0; mPriv = 0.0; pPriv = 0.0; mWs = 0.0; pWs = 0.0; pCount = $p.Count; mThreads = 0 }
  foreach ($x in $m) { $r.mCpu += $x.TotalProcessorTime.TotalSeconds; $r.mPriv += $x.PrivateMemorySize64; $r.mWs += $x.WorkingSet64; $r.mThreads += $x.Threads.Count }
  foreach ($x in $p) { $r.pCpu += $x.TotalProcessorTime.TotalSeconds; $r.pPriv += $x.PrivateMemorySize64; $r.pWs += $x.WorkingSet64 }
  return $r
}
$a = Snap
Start-Sleep -Seconds $Seconds
$b = Snap
$dt = ($b.t - $a.t).TotalSeconds
"{0}: cpu {1:N2}% of one core, private {2:N1} MB, working set {3:N1} MB, threads {4}" -f $Name, (100 * ($b.mCpu - $a.mCpu) / $dt), ($b.mPriv / 1MB), ($b.mWs / 1MB), $b.mThreads
"mpv: cpu {0:N2}% of one core, private {1:N1} MB, working set {2:N1} MB (count {3})" -f (100 * ($b.pCpu - $a.pCpu) / $dt), ($b.pPriv / 1MB), ($b.pWs / 1MB), $b.pCount
