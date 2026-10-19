param([string]$Name = "murmur_dev")
$id = (Get-Process -Name $Name -ErrorAction SilentlyContinue | Select-Object -First 1).Id
$c = Get-Counter "\Process V2(${Name}:$id)\Working Set - Private" -ErrorAction SilentlyContinue
if (-not $c) { $c = Get-Counter "\Process($Name)\Working Set - Private" }
$v = $c.CounterSamples[0].CookedValue
"{0}: private working set {1:N1} MB" -f $Name, ($v / 1MB)
