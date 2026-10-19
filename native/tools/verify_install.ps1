# Checks that Murmur is installed for the current user and findable from Start.
$exe = Join-Path $env:LOCALAPPDATA "Programs\Murmur\Murmur.exe"
$lnk = Join-Path $env:APPDATA "Microsoft\Windows\Start Menu\Programs\Murmur.lnk"
"exe exists:      {0} {1}" -f (Test-Path $exe), $(if (Test-Path $exe) { "({0:N0} bytes)" -f (Get-Item $exe).Length })
"shortcut exists: {0}" -f (Test-Path $lnk)
$apps = Get-StartApps | Where-Object { $_.Name -like "*urmur*" }
"Start apps:      {0}" -f (($apps | ForEach-Object { "$($_.Name) [$($_.AppID)]" }) -join "; ")
$k = Get-ItemProperty "HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\Murmur" -ErrorAction SilentlyContinue
if ($k) { "uninstall entry: {0} {1} -> {2}" -f $k.DisplayName, $k.DisplayVersion, $k.UninstallString } else { "uninstall entry: missing" }
$p = Get-Process -Name Murmur -ErrorAction SilentlyContinue
"running:         {0}" -f $(if ($p) { "pid " + $p.Id + ", " + [math]::Round($p.WorkingSet64 / 1MB, 1) + " MB working set" } else { "no" })
