$ErrorActionPreference='Stop'
Add-Type -AssemblyName System.Drawing
Add-Type -ReferencedAssemblies System.Drawing -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
using System.Drawing;
public class Cap2 {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L,T,R,B; }
  [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr v);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool BringWindowToTop(IntPtr h);
  [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
  public static bool Save(IntPtr h, string path) {
    RECT r; GetWindowRect(h, out r); int w=r.R-r.L, ht=r.B-r.T;
    var bmp=new Bitmap(w,ht); bool ok;
    using(var g=Graphics.FromImage(bmp)){ IntPtr hdc=g.GetHdc(); ok=PrintWindow(h,hdc,2); if(!ok) ok=PrintWindow(h,hdc,0); g.ReleaseHdc(hdc); }
    bmp.Save(path, System.Drawing.Imaging.ImageFormat.Png); bmp.Dispose(); return ok;
  }
  public static void Chars(IntPtr h, string s){ foreach(char c in s){ PostMessage(h,0x0102,(IntPtr)(int)c,IntPtr.Zero); System.Threading.Thread.Sleep(60);} }
}
'@
[Cap2]::SetProcessDpiAwarenessContext([IntPtr](-4)) | Out-Null
Get-Process -Name Bare -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 500
Start-Process -FilePath 'E:\PythonFiles\Bare\Bare.exe' -ArgumentList '"E:\PythonFiles\Bare\docs\demo.txt"' | Out-Null
$p = $null
for ($i=0; $i -lt 150 -and $null -eq $p; $i++) { Start-Sleep -Milliseconds 100; $p = Get-Process -Name Bare -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1 }
$h = $p.MainWindowHandle
[Cap2]::BringWindowToTop($h) | Out-Null
[Cap2]::SetForegroundWindow($h) | Out-Null
Start-Sleep -Milliseconds 500
$ws = New-Object -ComObject WScript.Shell
$ws.SendKeys('^f')
Start-Sleep -Milliseconds 600
[Cap2]::Chars($h, 'Bare')
Start-Sleep -Milliseconds 900
$ok = [Cap2]::Save($h, 'E:\PythonFiles\Bare\docs\screenshot.png')
Write-Output ('OK=' + $ok)
