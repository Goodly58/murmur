Add-Type -AssemblyName System.Windows.Forms
foreach ($s in [System.Windows.Forms.Screen]::AllScreens) {
  "{0} primary={1} bounds={2} work={3}" -f $s.DeviceName, $s.Primary, $s.Bounds, $s.WorkingArea
}
