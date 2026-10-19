Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class T {
  [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern IntPtr FindWindow(string c, string t);
  [DllImport("user32.dll")] public static extern int GetWindowLong(IntPtr h, int i);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
}
"@
$h = [T]::FindWindow("MurmurMainWindow", [NullString]::Value)
$ex = [T]::GetWindowLong($h, -20); $st = [T]::GetWindowLong($h, -16)
$r = New-Object T+RECT; [T]::GetWindowRect($h, [ref]$r) | Out-Null
"topmost={0} thickframe={1} maximizebox={2} rect=({3},{4},{5},{6}) size={7}x{8}" -f (($ex -band 0x8) -ne 0), (($st -band 0x40000) -ne 0), (($st -band 0x10000) -ne 0), $r.L, $r.T, $r.R, $r.B, ($r.R - $r.L), ($r.B - $r.T)
