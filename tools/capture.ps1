# 指定プロセスの見えているトップレベル窓を、全画面を撮らずに 1 枚ずつ画像にする。
# PrintWindow を使うので、ほかの窓に隠れていても撮れ、前面化もしない。
#
#   powershell -File tools\capture.ps1 -ProcessId <pid> -Out <フォルダ> [-Click <ボタンの ID>]
#
# -Click を付けると、撮る前にその ID のボタンが押されたことを WM_COMMAND で親へ知らせる(キー操作は送らない)。
param([int]$ProcessId, [string]$Out, [int]$Click = 0, [int]$Wait = 600)

Add-Type -AssemblyName System.Drawing
Add-Type -Namespace Cap -Name W -MemberDefinition @'
public delegate bool EnumProc(System.IntPtr h, System.IntPtr l);
[DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc p, System.IntPtr l);
[DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(System.IntPtr h, out uint pid);
[DllImport("user32.dll")] public static extern bool IsWindowVisible(System.IntPtr h);
[DllImport("user32.dll")] public static extern bool GetWindowRect(System.IntPtr h, out RECT r);
[DllImport("user32.dll")] public static extern bool PrintWindow(System.IntPtr h, System.IntPtr dc, uint f);
[DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowText(System.IntPtr h, System.Text.StringBuilder s, int n);
[DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(System.IntPtr h, System.Text.StringBuilder s, int n);
[DllImport("user32.dll")] public static extern System.IntPtr GetDlgItem(System.IntPtr h, int id);
[DllImport("user32.dll")] public static extern System.IntPtr SendMessage(System.IntPtr h, uint m, System.IntPtr w, System.IntPtr l);
[DllImport("user32.dll")] public static extern bool PostMessage(System.IntPtr h, uint m, System.IntPtr w, System.IntPtr l);
[DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(System.IntPtr v);
public struct RECT { public int L, T, R, B; }
'@
[Cap.W]::SetProcessDpiAwarenessContext([IntPtr](-4)) | Out-Null
New-Item -ItemType Directory -Force $Out | Out-Null

function Get-Windows {
    $list = New-Object System.Collections.ArrayList
    $cb = [Cap.W+EnumProc]{ param($h, $l)
        $p = 0; [Cap.W]::GetWindowThreadProcessId($h, [ref]$p) | Out-Null
        if ($p -eq $ProcessId -and [Cap.W]::IsWindowVisible($h)) { [void]$list.Add($h) }
        $true }
    [Cap.W]::EnumWindows($cb, [IntPtr]::Zero) | Out-Null
    $list
}

if ($Click) {
    foreach ($h in Get-Windows) {
        $b = [Cap.W]::GetDlgItem($h, $Click)
        if ($b -ne [IntPtr]::Zero) { [Cap.W]::PostMessage($h, 0x111, [IntPtr]$Click, $b) | Out-Null; break }   # WM_COMMAND / BN_CLICKED
    }
    Start-Sleep -Milliseconds $Wait
}

$i = 0
foreach ($h in Get-Windows) {
    $r = New-Object Cap.W+RECT
    [Cap.W]::GetWindowRect($h, [ref]$r) | Out-Null
    $w = $r.R - $r.L; $hh = $r.B - $r.T
    if ($w -le 0 -or $hh -le 0) { continue }
    $t = New-Object Text.StringBuilder 256; [Cap.W]::GetWindowText($h, $t, 256) | Out-Null
    $c = New-Object Text.StringBuilder 256; [Cap.W]::GetClassName($h, $c, 256) | Out-Null
    $bmp = New-Object Drawing.Bitmap $w, $hh
    $g = [Drawing.Graphics]::FromImage($bmp)
    $dc = $g.GetHdc()
    [Cap.W]::PrintWindow($h, $dc, 2) | Out-Null     # PW_RENDERFULLCONTENT
    $g.ReleaseHdc($dc); $g.Dispose()
    $file = Join-Path $Out ("win{0}.png" -f $i)
    $bmp.Save($file, [Drawing.Imaging.ImageFormat]::Png); $bmp.Dispose()
    "{0}  {1}x{2}  class={3}  title={4}" -f $file, $w, $hh, $c, $t
    $i++
}
