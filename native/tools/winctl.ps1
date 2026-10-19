# Sends input to the running Murmur window and prints its state. Coordinates are client pixels.
#   winctl.ps1 -Cmd info
#   winctl.ps1 -Cmd click -X 500 -Y 300
#   winctl.ps1 -Cmd dblclick -X 500 -Y 300
#   winctl.ps1 -Cmd move -X 500 -Y 300
#   winctl.ps1 -Cmd drag -X 500 -Y 300 -X2 500 -Y2 250
#   winctl.ps1 -Cmd key -Vk 32        (virtual-key code; 32 = space)
#   winctl.ps1 -Cmd wheel -X 500 -Y 300 -Delta -120
#   winctl.ps1 -Cmd close
param(
  [string]$Text = "",
  [string]$Cmd = "info", [int]$X = 0, [int]$Y = 0, [int]$X2 = 0, [int]$Y2 = 0, [int]$Vk = 0, [int]$Delta = 120,
  [string]$Class = "MurmurMainWindow"
)
Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class W {
  [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern IntPtr FindWindow(string c, string t);
  [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern bool SendMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern uint GetDpiForWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetWindowText(IntPtr h, System.Text.StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
  [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int w, int hgt, uint flags);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool IsIconic(IntPtr h);
  [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern IntPtr FindWindowEx(IntPtr p, IntPtr a, string c, string t);
  [DllImport("user32.dll", CharSet = CharSet.Unicode, EntryPoint = "SendMessageW")] public static extern IntPtr SendMessageStr(IntPtr h, uint m, IntPtr w, string l);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
}
"@
[W]::SetProcessDPIAware() | Out-Null
$h = [W]::FindWindow($Class, [NullString]::Value)
if ($h -eq [IntPtr]::Zero) { Write-Output "no window"; exit 2 }
function MkLp($x, $y) { return [IntPtr](($y -shl 16) -bor ($x -band 0xFFFF)) }
switch ($Cmd) {
  "info" {
    $c = New-Object W+RECT; [W]::GetClientRect($h, [ref]$c) | Out-Null
    $p = New-Object W+POINT; [W]::ClientToScreen($h, [ref]$p) | Out-Null
    $r = New-Object W+RECT; [W]::GetWindowRect($h, [ref]$r) | Out-Null
    $sb = New-Object System.Text.StringBuilder 256; [void][W]::GetWindowText($h, $sb, 256)
    Write-Output ("dpi={0} client={1}x{2} clientOffset=({3},{4}) title='{5}'" -f [W]::GetDpiForWindow($h), $c.R, $c.B, ($p.X - $r.L), ($p.Y - $r.T), $sb.ToString())
  }
  "click" { [W]::SendMessage($h, 0x0200, [IntPtr]0, (MkLp $X $Y)) | Out-Null; [W]::SendMessage($h, 0x0201, [IntPtr]1, (MkLp $X $Y)) | Out-Null; Start-Sleep -Milliseconds 40; [W]::SendMessage($h, 0x0202, [IntPtr]0, (MkLp $X $Y)) | Out-Null }
  "dblclick" { [W]::SendMessage($h, 0x0200, [IntPtr]0, (MkLp $X $Y)) | Out-Null; [W]::SendMessage($h, 0x0203, [IntPtr]1, (MkLp $X $Y)) | Out-Null; [W]::SendMessage($h, 0x0202, [IntPtr]0, (MkLp $X $Y)) | Out-Null }
  "move" { [W]::SendMessage($h, 0x0200, [IntPtr]0, (MkLp $X $Y)) | Out-Null }
  "drag" {
    [W]::SendMessage($h, 0x0200, [IntPtr]0, (MkLp $X $Y)) | Out-Null
    [W]::SendMessage($h, 0x0201, [IntPtr]1, (MkLp $X $Y)) | Out-Null
    $steps = 8
    for ($i = 1; $i -le $steps; $i++) {
      $xx = [int]($X + ($X2 - $X) * $i / $steps); $yy = [int]($Y + ($Y2 - $Y) * $i / $steps)
      [W]::SendMessage($h, 0x0200, [IntPtr]1, (MkLp $xx $yy)) | Out-Null
      Start-Sleep -Milliseconds 15
    }
    [W]::SendMessage($h, 0x0202, [IntPtr]0, (MkLp $X2 $Y2)) | Out-Null
  }
  "key" { [W]::SendMessage($h, 0x0100, [IntPtr]$Vk, [IntPtr]0) | Out-Null; [W]::SendMessage($h, 0x0101, [IntPtr]$Vk, [IntPtr]0) | Out-Null }
  "wheel" {
    $p = New-Object W+POINT; $p.X = $X; $p.Y = $Y; [W]::ClientToScreen($h, [ref]$p) | Out-Null
    $wp = [IntPtr](($Delta -shl 16) -band 0xFFFF0000)
    [W]::SendMessage($h, 0x020A, $wp, (MkLp $p.X $p.Y)) | Out-Null
  }
  "restore" { if ([W]::IsIconic($h)) { [W]::ShowWindow($h, 9) | Out-Null }; [W]::SetForegroundWindow($h) | Out-Null }
  "edittext" { $e = [W]::FindWindowEx($h, [IntPtr]::Zero, "EDIT", [NullString]::Value); if ($e -eq [IntPtr]::Zero) { Write-Output "no edit" } else { [W]::SendMessageStr($e, 0x000C, [IntPtr]0, $Text) | Out-Null; Start-Sleep -Milliseconds 50; [W]::SendMessage($e, 0x0100, [IntPtr]13, [IntPtr]0) | Out-Null; Write-Output "edit set" } }
  "settext" { $e = [W]::FindWindowEx($h, [IntPtr]::Zero, "EDIT", [NullString]::Value); if ($e -eq [IntPtr]::Zero) { Write-Output "no edit" } else { [W]::SendMessageStr($e, 0x000C, [IntPtr]0, $Text) | Out-Null; Write-Output "text set" } }
  "resize" { [W]::SetWindowPos($h, [IntPtr]::Zero, $X2, $Y2, $X, $Y, 0x0014) | Out-Null }   # -X/-Y = outer size, -X2/-Y2 = position
  "close" { [W]::PostMessage($h, 0x0010, [IntPtr]0, [IntPtr]0) | Out-Null }
}
