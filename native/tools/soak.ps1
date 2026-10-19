# Samples memory, handles and threads of a running process to spot leaks.
param([int]$Minutes = 3, [int]$EverySeconds = 20, [string]$Name = "Murmur")
$end = (Get-Date).AddMinutes($Minutes)
while ((Get-Date) -lt $end) {
  $p = Get-Process -Name $Name -ErrorAction SilentlyContinue | Select-Object -First 1
  if (-not $p) { "process gone"; break }
  $id = $p.Id
  $c = Get-Counter "\Process V2(${Name}:$id)\Working Set - Private" -ErrorAction SilentlyContinue
  $pws = if ($c) { [math]::Round($c.CounterSamples[0].CookedValue / 1MB, 1) } else { -1 }
  "{0:HH:mm:ss}  private ws {1} MB  commit {2:N1} MB  handles {3}  threads {4}" -f (Get-Date), $pws, ($p.PrivateMemorySize64 / 1MB), $p.HandleCount, $p.Threads.Count
  Start-Sleep -Seconds $EverySeconds
}
