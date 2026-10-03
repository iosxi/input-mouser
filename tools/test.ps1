# input-mouser の自動検証(1 台の PC で、送り手と受け手を別の ini で並べて動かす)
#
#   powershell -ExecutionPolicy Bypass -File tools\test.ps1
#
# 受け手は -bind 127.0.0.1 -dryrun で動かす。127.0.0.1 だけで待ち受けるので
# ファイアウォールの確認は出ず、受けた入力は再現せずログに書くだけ。
# 送り手には「相手を操作中」の間だけ入力を注入する(フックが握りつぶすので
# 利用者のアプリには届かない)。カーソルは最後に元の位置へ戻す。
# 画面は撮らない。
param([string]$Exe = (Join-Path $PSScriptRoot '..\input-mouser.exe'))

$ErrorActionPreference = 'Stop'
$Exe = (Resolve-Path $Exe).Path
Add-Type -Namespace T -Name W -MemberDefinition @'
[StructLayout(LayoutKind.Sequential)] public struct MOUSEINPUT { public int dx, dy; public uint mouseData, dwFlags, time; public System.IntPtr extra; }
[StructLayout(LayoutKind.Sequential)] public struct KEYBDINPUT { public ushort vk, scan; public uint flags, time; public System.IntPtr extra; }
[StructLayout(LayoutKind.Explicit)] public struct INPUT { [FieldOffset(0)] public uint type; [FieldOffset(8)] public MOUSEINPUT mi; [FieldOffset(8)] public KEYBDINPUT ki; }
[StructLayout(LayoutKind.Sequential)] public struct POINT { public int x, y; }
[StructLayout(LayoutKind.Sequential)] public struct CURSORINFO { public int cbSize, flags; public System.IntPtr hCursor; public POINT pt; }
[DllImport("user32.dll")] public static extern uint SendInput(uint n, INPUT[] i, int cb);
[DllImport("user32.dll")] public static extern bool GetCursorPos(out POINT p);
[DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
[DllImport("user32.dll")] public static extern bool GetCursorInfo(ref CURSORINFO ci);
[DllImport("user32.dll")] public static extern int GetSystemMetrics(int i);
[DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(System.IntPtr v);
public static void Move(int dx, int dy) { var a = new INPUT[1]; a[0].type = 0; a[0].mi.dx = dx; a[0].mi.dy = dy; a[0].mi.dwFlags = 1; SendInput(1, a, System.Runtime.InteropServices.Marshal.SizeOf(typeof(INPUT))); }
public static void Wheel(int d) { var a = new INPUT[1]; a[0].type = 0; a[0].mi.mouseData = (uint)d; a[0].mi.dwFlags = 0x800; SendInput(1, a, System.Runtime.InteropServices.Marshal.SizeOf(typeof(INPUT))); }
public static void Button(bool down) { var a = new INPUT[1]; a[0].type = 0; a[0].mi.dwFlags = down ? 2u : 4u; SendInput(1, a, System.Runtime.InteropServices.Marshal.SizeOf(typeof(INPUT))); }
public static void Key(ushort vk, ushort scan, bool up, bool ext) { var a = new INPUT[1]; a[0].type = 1; a[0].ki.vk = vk; a[0].ki.scan = scan; a[0].ki.flags = (up ? 2u : 0u) | (ext ? 1u : 0u); SendInput(1, a, System.Runtime.InteropServices.Marshal.SizeOf(typeof(INPUT))); }
'@
[T.W]::SetProcessDpiAwarenessContext([IntPtr](-4)) | Out-Null

$work = Join-Path $env:TEMP 'input-mouser-test'
if (Test-Path $work) { Remove-Item -Recurse -Force $work }
New-Item -ItemType Directory $work | Out-Null
$key  = '00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff'
$bad  = 'ffeeddccbbaa99887766554433221100ffeeddccbbaa99887766554433221100'
$sIni = Join-Path $work 's.ini'; $s2Ini = Join-Path $work 's2.ini'; $mIni = Join-Path $work 'm.ini'
$sLog = Join-Path $work 's.log'; $s2Log = Join-Path $work 's2.log'; $mLog = Join-Path $work 'm.log'
$utf8 = New-Object Text.UTF8Encoding $false
[IO.File]::WriteAllText($sIni,  "[general]`r`naccept=1`r`nport=31861`r`nkey=$key`r`nlog=1`r`n", $utf8)
[IO.File]::WriteAllText($s2Ini, "[general]`r`naccept=1`r`nport=31863`r`nkey=$bad`r`nlog=1`r`n", $utf8)
[IO.File]::WriteAllText($mIni,  ("[general]`r`naccept=0`r`nport=31862`r`nkey=$key`r`nlog=1`r`nosd=0`r`nhotkey_home=Ctrl+Alt+Home`r`n" +
    "`r`n[peer]`r`nhost=127.0.0.1`r`nport=31861`r`nx=1`r`ny=0`r`n" +
    "`r`n[peer]`r`nhost=127.0.0.1`r`nport=31863`r`nx=-1`r`ny=0`r`n"), $utf8)

$results = New-Object System.Collections.ArrayList
function Check($name, $ok, $detail = '') { [void]$results.Add([pscustomobject]@{ 項目 = $name; 結果 = $(if ($ok) { 'OK' } else { 'NG' }); 詳細 = $detail }) }
function LogHas($path, $pattern) { (Test-Path $path) -and ((Get-Content -Encoding UTF8 $path) -match $pattern) }
function WaitLog($path, $pattern, $ms = 3000) { $t = 0; while ($t -lt $ms) { if (LogHas $path $pattern) { return $true }; Start-Sleep -Milliseconds 50; $t += 50 }; $false }

$orig = New-Object T.W+POINT; [T.W]::GetCursorPos([ref]$orig) | Out-Null
$vw = [T.W]::GetSystemMetrics(78); $vh = [T.W]::GetSystemMetrics(79)

$ps = @()
try {
    $ps += Start-Process $Exe -ArgumentList "-ini `"$sIni`" -bind 127.0.0.1 -dryrun" -PassThru
    $ps += Start-Process $Exe -ArgumentList "-ini `"$s2Ini`" -bind 127.0.0.1 -dryrun" -PassThru
    Start-Sleep -Milliseconds 400
    $ps += Start-Process $Exe -ArgumentList "-ini `"$mIni`"" -PassThru

    Check '接続(握手と暗号化)' (WaitLog $mLog '127.0.0.1 \(127.0.0.1\) につながりました' 6000)
    Check 'パスワード違いを断る' (WaitLog $mLog 'パスワードが一致しません' 6000)
    Check '  … 受け手にも記録が残る' (WaitLog $s2Log 'パスワードが一致しませんでした')
    Check '受け手が受け付けた' (WaitLog $sLog 'から操作できるようになりました')

    # --- コマンドで切り替え ---
    Start-Process $Exe -ArgumentList "-ini `"$mIni`" -switch 1" -Wait
    Check '切り替え(コマンド)' (WaitLog $sLog 'enter side=255')
    Start-Sleep -Milliseconds 150
    $ci = New-Object T.W+CURSORINFO; $ci.cbSize = [Runtime.InteropServices.Marshal]::SizeOf($ci)
    [T.W]::GetCursorInfo([ref]$ci) | Out-Null
    $cx = [int]($vw / 2); $cy = [int]($vh / 2)
    Check 'カーソルを中央に留める' ([Math]::Abs($ci.pt.x - $cx) -le 1 -and [Math]::Abs($ci.pt.y - $cy) -le 1) ("({0},{1})" -f $ci.pt.x, $ci.pt.y)
    Check 'カーソルを隠す' ($ci.hCursor -eq [IntPtr]::Zero) ("hCursor={0} flags={1}" -f $ci.hCursor, $ci.flags)

    # --- 入力を送る ---
    for ($i = 0; $i -lt 5; $i++) { [T.W]::Move(10, 0); Start-Sleep -Milliseconds 10 }
    [T.W]::Key(0x41, 0x1E, $false, $false); [T.W]::Key(0x41, 0x1E, $true, $false)
    [T.W]::Key(0x25, 0x4B, $false, $true);  [T.W]::Key(0x25, 0x4B, $true, $true)     # ← (拡張キー)
    [T.W]::Wheel(120)
    [T.W]::Button($true); [T.W]::Button($false)
    Start-Sleep -Milliseconds 300
    $pt = New-Object T.W+POINT; [T.W]::GetCursorPos([ref]$pt) | Out-Null
    Check 'このPC のカーソルは動かない' ([Math]::Abs($pt.x - $cx) -le 1) ("({0},{1})" -f $pt.x, $pt.y)
    Check '移動が届く' (LogHas $sLog '\[dryrun\] move \d+,0 ->')
    Check 'キーが届く(スキャン コード付き)' ((LogHas $sLog 'key vk=41 scan=1E down') -and (LogHas $sLog 'key vk=41 scan=1E up'))
    Check '拡張キーが届く' (LogHas $sLog 'key vk=25 scan=4B down ext')
    Check 'ホイールが届く' (LogHas $sLog 'wheel v 120')
    Check 'ボタンが届く' ((LogHas $sLog 'button 0 down') -and (LogHas $sLog 'button 0 up'))

    # --- 受け手の左端を越える → このPC に戻る ---
    [T.W]::Move(-3000, 0)
    Check '受け手の端で戻る' (WaitLog $mLog 'このPC に戻りました')
    Start-Sleep -Milliseconds 100
    [T.W]::GetCursorPos([ref]$pt) | Out-Null
    Check '戻った位置は右端' ($pt.x -eq $vw - 1) ("({0},{1})" -f $pt.x, $pt.y)

    # --- このPC の右端を越える → 相手へ ---
    [T.W]::SetCursorPos($vw - 1, [int]($vh / 3)) | Out-Null
    Start-Sleep -Milliseconds 50
    [T.W]::Move(15, 0)
    Check 'このPC の端で切り替わる' (WaitLog $sLog 'enter side=0 ')
    Start-Sleep -Milliseconds 100

    # --- ホットキーで戻る(Ctrl+Alt+Home) ---
    [T.W]::Key(0xA2, 0x1D, $false, $false); [T.W]::Key(0xA4, 0x38, $false, $false)
    [T.W]::Key(0x24, 0x47, $false, $true);  [T.W]::Key(0x24, 0x47, $true, $true)
    [T.W]::Key(0xA4, 0x38, $true, $false);  [T.W]::Key(0xA2, 0x1D, $true, $false)
    Start-Sleep -Milliseconds 300
    $homeCount = @((Get-Content -Encoding UTF8 $mLog) -match 'このPC に戻りました').Count
    Check 'ホットキーで戻る' ($homeCount -ge 2) "戻った回数 $homeCount"
    Check 'Home は相手に届かない' (-not (LogHas $sLog 'key vk=24'))
    Check 'Alt を離した後の空打ち(メニュー防止)' (LogHas $sLog 'key vk=E8 scan=00 down')
    Check 'Ctrl・Alt の離しは相手へ' ((LogHas $sLog 'key vk=A2 scan=1D up') -and (LogHas $sLog 'key vk=A4 scan=38 up'))

    # --- 受け手が落ちたら戻る ---
    Start-Process $Exe -ArgumentList "-ini `"$mIni`" -switch 1" -Wait
    Start-Sleep -Milliseconds 300
    Start-Process $Exe -ArgumentList "-ini `"$sIni`" -exit" -Wait
    Check '受け手が終わると戻る' (WaitLog $mLog '切れました' 4000)
    Start-Sleep -Milliseconds 200
    $homeCount = @((Get-Content -Encoding UTF8 $mLog) -match 'このPC に戻りました').Count
    Check '  … そしてこのPC に戻る' ($homeCount -ge 3) "戻った回数 $homeCount"
}
finally {
    foreach ($ini in @($mIni, $sIni, $s2Ini)) { Start-Process $Exe -ArgumentList "-ini `"$ini`" -exit" -Wait }
    Start-Sleep -Milliseconds 300
    foreach ($p in $ps) { if (-not $p.HasExited) { $p.Kill() } }
    [T.W]::SetCursorPos($orig.x, $orig.y) | Out-Null
}
$results | Format-Table -AutoSize | Out-String -Width 200
"ログ: $work"
if ($results | Where-Object { $_.結果 -eq 'NG' }) { exit 1 }
