@echo off
rem Compressor NEXT - Diagnose. Liest nur aus (keine Adminrechte, keine Aenderungen), spielt einen kurzen
rem leisen Testton und schreibt den Bericht auf den Desktop: CompressorNext-Diagnose.txt
set "CNX_SELF=%~f0"
powershell -NoProfile -ExecutionPolicy Bypass -Command "$s = Get-Content -LiteralPath $env:CNX_SELF -Raw; Invoke-Expression $s.Substring($s.LastIndexOf('#PSSTART'))"
exit /b
#PSSTART
$ErrorActionPreference = 'Continue'
try { [Console]::OutputEncoding = [Text.Encoding]::UTF8 } catch {}
$out = New-Object System.Collections.Generic.List[string]
function Add($s) { $script:out.Add([string]$s) }
function Section($t) { Add ''; Add ('===== ' + $t + ' =====') }
function Guard($block) { try { & $block } catch { Add ('FEHLER: ' + $_.Exception.Message) } }
function Short($m, $n) { if (-not $m) { return '' }; $t = ($m -replace '\s+', ' '); if ($t.Length -gt $n) { $t.Substring(0, $n) } else { $t } }

Add-Type -TypeDefinition @'
using System; using System.Runtime.InteropServices;
public static class CnxDiag {
  [DllImport("kernel32.dll")] public static extern ulong GetTickCount64();
  [ComImport, Guid("BCDE0395-E52F-467C-8E3D-C4579291692E")] class Enumerator {}
  [ComImport, InterfaceType(ComInterfaceType.InterfaceIsIUnknown), Guid("A95664D2-9614-4F35-A746-DE8DB63617E6")]
  interface IMMDeviceEnumerator { [PreserveSig] int EnumAudioEndpoints(int f, int m, out IntPtr d); [PreserveSig] int GetDefaultAudioEndpoint(int f, int r, out IMMDevice d); }
  [ComImport, InterfaceType(ComInterfaceType.InterfaceIsIUnknown), Guid("D666063F-1587-4E43-81F1-B948E807363F")]
  interface IMMDevice { [PreserveSig] int Activate(ref Guid i, int c, IntPtr p, out IntPtr o); [PreserveSig] int OpenPropertyStore(int a, out IntPtr s); [PreserveSig] int GetId([MarshalAs(UnmanagedType.LPWStr)] out string id); }
  public static string DefaultRender() { try { var e = (IMMDeviceEnumerator)new Enumerator(); IMMDevice d; if (e.GetDefaultAudioEndpoint(0, 1, out d) != 0 || d == null) return "(keins)"; string id; d.GetId(out id); return id; } catch (Exception x) { return "Fehler: " + x.Message; } }
}
'@ -ErrorAction SilentlyContinue

$app = Join-Path $env:ProgramFiles 'Compressor NEXT'
$stateFile = Join-Path $env:ProgramData 'CompressorNext\state.bin'
$clsid = '{CE339205-A997-4182-B027-A9637647F8CA}'

function Read-State {
  $fs = [IO.File]::Open($stateFile, 'Open', 'Read', 'ReadWrite,Delete'); $b = New-Object byte[] 4096; [void]$fs.Read($b, 0, 4096); $fs.Close(); return ,$b
}
function Show-State($label) {
  Guard {
    $b = Read-State; $now = [CnxDiag]::GetTickCount64()
    Add ('{0}: magic 0x{1:X8}, seq {2}, ein {3}, Schwelle {4}, Ausgang {5}, Ausnahmen {6} | Modul geladen {7}x, Stroeme gestartet {8}x' -f $label, [BitConverter]::ToUInt32($b,0), [BitConverter]::ToUInt32($b,8), [BitConverter]::ToUInt32($b,12), [BitConverter]::ToSingle($b,16), [BitConverter]::ToSingle($b,20), [BitConverter]::ToUInt32($b,24), [BitConverter]::ToUInt32($b,192), [BitConverter]::ToUInt32($b,196))
    for ($i = 0; $i -lt 16; $i++) { $o = 256 + 64 * $i; if ([BitConverter]::ToUInt32($b, $o + 8) -ne 0) {
      Add ('   Platz {0}: Alter {1} ms, Geraet 0x{2:X8}, {3} Hz, {4} Kanaele, arbeitet={5}, inL {6:N4}, outL {7:N4}, minGain {8:N3}' -f $i, ($now - [BitConverter]::ToUInt64($b, $o)), [BitConverter]::ToUInt32($b, $o + 16), [BitConverter]::ToUInt32($b, $o + 20), [BitConverter]::ToUInt32($b, $o + 24), [BitConverter]::ToUInt32($b, $o + 28), [BitConverter]::ToSingle($b, $o + 32), [BitConverter]::ToSingle($b, $o + 40), [BitConverter]::ToSingle($b, $o + 48)) } }
  }
}

Write-Host 'Compressor NEXT - Diagnose laeuft (etwa 10 Sekunden, gleich kommt ein kurzer leiser Testton) ...'

Section 'System'
Guard { $os = Get-CimInstance Win32_OperatingSystem; $cv = Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion'
  Add ('{0} | {1} | Build {2}.{3} ({4}) | {5}' -f $env:COMPUTERNAME, $os.Caption, $os.BuildNumber, $cv.UBR, $cv.DisplayVersion, $env:PROCESSOR_ARCHITECTURE)
  Add ('Zeit {0} | letzter Windows-Start {1}' -f (Get-Date).ToString('yyyy-MM-dd HH:mm:ss'), $os.LastBootUpTime.ToString('yyyy-MM-dd HH:mm:ss')) }
Guard { Add ('Smart App Control (0=aus, 1=an, 2=Testphase): ' + (Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Control\CI\Policy' -ErrorAction Stop).VerifiedAndReputablePolicyState) }
Guard { Add ('DisableProtectedAudioDG: ' + (Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Audio' -ErrorAction Stop).DisableProtectedAudioDG) }
Guard { Add ('.NET Desktop Runtime: ' + ((Get-ChildItem (Join-Path $env:ProgramFiles 'dotnet\shared\Microsoft.WindowsDesktop.App') -ErrorAction Stop | ForEach-Object { $_.Name }) -join ', ')) }

Section 'Programmordner'
Guard { Get-ChildItem $app -ErrorAction Stop | ForEach-Object { Add ('{0,10} {1} {2} {3}' -f $_.Length, $_.LastWriteTime.ToString('yyyy-MM-dd HH:mm:ss'), $_.Name, $_.VersionInfo.FileVersion) } }

Section 'Dienste und Prozesse'
Guard { Get-Service CompressorNext, Audiosrv, AudioEndpointBuilder -ErrorAction SilentlyContinue | ForEach-Object { Add ('{0}: {1} ({2})' -f $_.Name, $_.Status, $_.StartType) } }
Guard { Get-Process CompressorNext, CompressorNextSvc, audiodg -ErrorAction SilentlyContinue | ForEach-Object { $st = ''; try { $st = $_.StartTime.ToString('yyyy-MM-dd HH:mm:ss') } catch {}; Add ('{0} PID {1} Start {2}' -f $_.ProcessName, $_.Id, $st) } }

Section 'Registrierung des Moduls'
Guard { $k = 'HKLM:\SOFTWARE\Classes\CLSID\' + $clsid + '\InprocServer32'; if (Test-Path $k) { $p = (Get-ItemProperty $k).'(default)'; Add ('InprocServer32: {0} (Datei vorhanden: {1}), ThreadingModel {2}' -f $p, (Test-Path $p), (Get-ItemProperty $k).ThreadingModel) } else { Add 'CLSID-Eintrag FEHLT' } }
Guard { $a = 'HKLM:\SOFTWARE\Classes\AudioEngine\AudioProcessingObjects\' + $clsid; if (Test-Path $a) { $v = Get-ItemProperty $a; Add ('APO-Eintrag: {0}, Flags {1}, Schnittstellen {2}' -f $v.FriendlyName, $v.Flags, $v.NumAPOInterfaces) } else { Add 'APO-Eintrag FEHLT' } }

Section 'Aktive Ausgabegeraete'
Guard { Add ('Standardgeraet: ' + [CnxDiag]::DefaultRender()) }
Guard {
  $base = 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Render'
  Get-ChildItem $base -ErrorAction Stop | ForEach-Object {
    $ep = $_; $state = (Get-ItemProperty $ep.PSPath -ErrorAction SilentlyContinue).DeviceState
    if ($state -ne 1) { return }
    $p = Get-ItemProperty ($ep.PSPath + '\Properties') -ErrorAction SilentlyContinue
    Add ''
    Add ('## {0} | {1} | {2} | Disable_SysFx={3} | BT-kombiniert={4}' -f $ep.PSChildName, $p.'{a45c254e-df1c-4efd-8020-67d146a850e0},2', $p.'{b3f8fa53-0004-438e-9003-51a46e139bfc},6', $p.'{1da5d803-d492-4edd-8c23-e0c0ffee7f0e},5', ($null -ne $p.'{b3f8fa53-0004-438e-9003-51a46e139bfc},41'))
    $fxPath = $ep.PSPath + '\FxProperties'
    if (-not (Test-Path $fxPath)) { Add '   (kein FxProperties-Schluessel)'; return }
    $fx = Get-Item $fxPath
    foreach ($n in $fx.GetValueNames()) { $v = $fx.GetValue($n); if ($v -is [string[]]) { $v = $v -join ' ; ' } elseif ($v -is [byte[]]) { $v = 'bin[' + $v.Length + ']' }; Add ('   {0} [{1}] = {2}' -f $n, $fx.GetValueKind($n), $v) }
    $subs = (Get-ChildItem $fxPath -ErrorAction SilentlyContinue | ForEach-Object { $_.PSChildName }) -join ', '
    if ($subs) { Add ('   Unterschluessel: ' + $subs) }
  }
}

Section 'Effekt-Module der aktiven Geraete (angemeldet? Datei vorhanden?)'
Guard {
  $base = 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Render'
  $ids = New-Object System.Collections.Generic.List[string]
  Get-ChildItem $base -ErrorAction Stop | ForEach-Object {
    if ((Get-ItemProperty $_.PSPath -ErrorAction SilentlyContinue).DeviceState -ne 1) { return }
    $texts = @()
    $fxPath = $_.PSPath + '\FxProperties'
    if (Test-Path $fxPath) { $fx = Get-Item $fxPath; foreach ($n in $fx.GetValueNames()) { if ($n -match '^\{d04e05a6-594b-4fb6-a80d-01af5eed7d1d\},(1|2|5|6|7|13|14|15|19|20)$') { $texts += @($fx.GetValue($n)) } } }
    # Was Compressor NEXT auf diesem Geraet ersetzt hat, steht als Ursprungswert in seinem Vermerk.
    $rec = 'HKLM:\SOFTWARE\ShakieVan\CompressorNext\Endpoints\' + $_.PSChildName
    if (Test-Path $rec) { $r = Get-Item $rec; foreach ($n in $r.GetValueNames()) { $texts += @($r.GetValue($n)) } }
    foreach ($t in $texts) { foreach ($m in [regex]::Matches([string]$t, '\{[0-9A-Fa-f-]{36}\}')) { $u = $m.Value.ToUpper(); if (-not $ids.Contains($u)) { $ids.Add($u) } } }
  }
  foreach ($id in $ids) {
    $name = (Get-ItemProperty ('HKLM:\SOFTWARE\Classes\AudioEngine\AudioProcessingObjects\' + $id) -ErrorAction SilentlyContinue).FriendlyName
    $dll = (Get-ItemProperty ('HKLM:\SOFTWARE\Classes\CLSID\' + $id + '\InprocServer32') -ErrorAction SilentlyContinue).'(default)'
    if ($dll) { $state = 'angemeldet: ' + $dll + ' (Datei vorhanden: ' + (Test-Path ([Environment]::ExpandEnvironmentVariables($dll))) + ')' } else { $state = 'NICHT allgemein angemeldet (evtl. nur ueber den Treiber)' }
    Add ('{0} {1} | {2}' -f $id, $name, $state)
  }
}
Guard { Add '-- Effekt-Komponenten der Treiber (PnP) --'; Get-PnpDevice -Class AudioProcessingObject -ErrorAction Stop | ForEach-Object { Add ('{0} | {1} | {2}' -f $_.Status, $_.FriendlyName, $_.InstanceId) } }

Section 'status (CompressorNextSvc.exe)'
Guard { & (Join-Path $app 'CompressorNextSvc.exe') status 400 2>&1 | ForEach-Object { Add ([string]$_) } }

Section 'Zustandsdatei'
Guard { icacls $stateFile 2>&1 | ForEach-Object { if ("$_".Trim()) { Add ([string]$_) } } }
Show-State 'vor dem Testton'

Section 'Testton (1,5 s, leise, ueber das Standardgeraet im normalen Windows-Modus)'
Guard {
  $rate = 48000; $n = [int]($rate * 1.5); $ms = New-Object IO.MemoryStream; $w = New-Object IO.BinaryWriter($ms)
  $w.Write([Text.Encoding]::ASCII.GetBytes('RIFF')); $w.Write([int](36 + $n * 4)); $w.Write([Text.Encoding]::ASCII.GetBytes('WAVEfmt ')); $w.Write([int]16); $w.Write([int16]1); $w.Write([int16]2); $w.Write([int]$rate); $w.Write([int]($rate * 4)); $w.Write([int16]4); $w.Write([int16]16)
  $w.Write([Text.Encoding]::ASCII.GetBytes('data')); $w.Write([int]($n * 4))
  for ($i = 0; $i -lt $n; $i++) { $env = [Math]::Min(1.0, [Math]::Min($i, $n - $i) / 2400.0); $s = [int16](1000 * $env * [Math]::Sin(2 * [Math]::PI * 440 * $i / $rate)); $w.Write($s); $w.Write($s) }
  $ms.Position = 0; $player = New-Object System.Media.SoundPlayer($ms); $player.Play(); Start-Sleep -Milliseconds 900
  Show-State 'waehrend des Testtons'
  Start-Sleep -Milliseconds 1200; $player.Stop()
}
Show-State 'nach dem Testton'

Section 'service.log (Ende)'
Guard { Get-Content (Join-Path $app 'service.log') -Encoding UTF8 -ErrorAction Stop | Select-Object -Last 90 | ForEach-Object { Add $_ } }

Section 'Windows-Ereignisse'
$since = (Get-Date).AddHours(-24)
foreach ($log in 'Microsoft-Windows-Audio/Operational', 'Microsoft-Windows-Audio/GlitchDetection', 'Microsoft-Windows-Audio/PlaybackManager') {
  try { $ev = Get-WinEvent -FilterHashtable @{ LogName = $log; StartTime = $since } -MaxEvents 15 -ErrorAction Stop; Add ('## ' + $log); $ev | ForEach-Object { Add ('{0} id={1} [{2}] {3}' -f $_.TimeCreated.ToString('MM-dd HH:mm:ss'), $_.Id, $_.LevelDisplayName, (Short $_.Message 300)) } } catch { Add ('## ' + $log + ': ' + $_.Exception.Message) }
}
foreach ($log in 'Application', 'System', 'Microsoft-Windows-CodeIntegrity/Operational') {
  try { $ev = Get-WinEvent -FilterHashtable @{ LogName = $log; StartTime = $since } -ErrorAction Stop | Where-Object { $_.Message -match 'audiodg|CompressorNext|Compressor NEXT|Audiosrv|AudioSrv' } | Select-Object -First 12; Add ('## ' + $log + ' (audiodg / Compressor NEXT / Audiosrv)'); $ev | ForEach-Object { Add ('{0} {1} id={2} [{3}] {4}' -f $_.TimeCreated.ToString('MM-dd HH:mm:ss'), $_.ProviderName, $_.Id, $_.LevelDisplayName, (Short $_.Message 400)) } } catch { Add ('## ' + $log + ': ' + $_.Exception.Message) }
}

$report = Join-Path ([Environment]::GetFolderPath('Desktop')) 'CompressorNext-Diagnose.txt'
[IO.File]::WriteAllLines($report, $out, (New-Object Text.UTF8Encoding($true)))
try { Set-Clipboard -Value ($out -join "`r`n") } catch {}
Write-Host ''
Write-Host ('Fertig. Bericht: ' + $report)
Write-Host 'Der Text liegt auch in der Zwischenablage (Strg+V zum Einfuegen).'
Start-Process notepad.exe $report
