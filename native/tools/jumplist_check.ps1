# Looks for Murmur's jump list entries in the shell's custom destination files.
$dir = Join-Path $env:APPDATA "Microsoft\Windows\Recent\CustomDestinations"
$needle = [System.Text.Encoding]::Unicode.GetBytes("Sleep 60 min")
function Has($bytes, $pat) {
  for ($i = 0; $i -le $bytes.Length - $pat.Length; $i++) {
    $ok = $true
    for ($j = 0; $j -lt $pat.Length; $j++) { if ($bytes[$i + $j] -ne $pat[$j]) { $ok = $false; break } }
    if ($ok) { return $true }
  }
  return $false
}
$found = $false
Get-ChildItem $dir -Filter *.customDestinations-ms -ErrorAction SilentlyContinue | Sort-Object LastWriteTime -Descending | Select-Object -First 8 | ForEach-Object {
  $b = [System.IO.File]::ReadAllBytes($_.FullName)
  $hit = Has $b $needle
  "{0}  {1}  {2:N0} bytes  {3}" -f $_.LastWriteTime.ToString("HH:mm:ss"), $_.Name, $b.Length, $(if ($hit) { "<-- Murmur tasks here" } else { "" })
  if ($hit) { $script:found = $true }
}
"jump list present: $found"
