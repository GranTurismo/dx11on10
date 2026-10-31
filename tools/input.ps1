# Plays a little script of waits, key presses and screenshots against the game window.
#   input.ps1 "<steps>" <process-name> <output-dir>
# steps: "wait:50 key:DOWN wait:2 key:ENTER shot:menu wait:60 shot:world"
#        "key:GRAVE type:coc_riverwood key:ENTER"   (types "coc riverwood" into a console)
#        "waitlog:first_frame_presented"            (until the proxy log contains the text; "_" = space)
# Keys are sent as hardware scan codes so DirectInput games see them.
# For the DX11on10 app (started with --hits <file>, passed here as the 5th argument), with the real mouse:
#        "click:row:Skyrim click:switch click:fake:1"  (control kind, then index or game name)
#        "wheel:play:-5"                                (mouse wheel over a control, in notches)
#        "dialog:Turn|on|anyway check:I|understand"     (a button / check box in a Windows dialog; "|" = space)
#        "text:C:\Games\Plain\Plain.exe rawkey:ENTER"   (type into the focused window; no refocusing)
#        "closewin:Test|Game kill:notepad clickat:1090,305"  (clickat: physical screen pixels)
#        "focus clickrel:0.93,0.11"                     (bring the target forward; click at a fraction of its window)
param([string]$steps, [string]$process, [string]$outDir, [string]$log = "", [string]$hits = "")

Add-Type -AssemblyName System.Windows.Forms, System.Drawing, UIAutomationClient, UIAutomationTypes
Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
public static class Input {
    [StructLayout(LayoutKind.Explicit, Size = 40)]
    struct INPUT {
        [FieldOffset(0)] public uint type;
        [FieldOffset(8)] public ushort wVk;
        [FieldOffset(10)] public ushort wScan;
        [FieldOffset(12)] public uint dwFlags;
        [FieldOffset(16)] public uint time;
        [FieldOffset(24)] public IntPtr dwExtraInfo;
    }
    [DllImport("user32.dll")] static extern uint SendInput(uint count, INPUT[] inputs, int size);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr window);
    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
    [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr window, out RECT r);
    [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr window, ref POINT p);
    [DllImport("user32.dll")] static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] static extern void mouse_event(uint flags, int dx, int dy, int data, IntPtr extra);
    public static void Click(int x, int y) {
        SetCursorPos(x, y); System.Threading.Thread.Sleep(120);
        mouse_event(0x0002, 0, 0, 0, IntPtr.Zero); System.Threading.Thread.Sleep(70);
        mouse_event(0x0004, 0, 0, 0, IntPtr.Zero);
    }
    public static void Wheel(int x, int y, int delta) {
        SetCursorPos(x, y); System.Threading.Thread.Sleep(120);
        mouse_event(0x0800, 0, 0, delta, IntPtr.Zero);
    }
    public static void Press(ushort scan, bool extended) {
        uint flags = 0x0008u | (extended ? 0x0001u : 0u);  // KEYEVENTF_SCANCODE (| EXTENDEDKEY)
        INPUT[] down = new INPUT[1];
        down[0].type = 1; down[0].wScan = scan; down[0].dwFlags = flags;
        SendInput(1, down, Marshal.SizeOf(typeof(INPUT)));
        System.Threading.Thread.Sleep(80);
        INPUT[] up = new INPUT[1];
        up[0].type = 1; up[0].wScan = scan; up[0].dwFlags = flags | 0x0002u;  // KEYEVENTF_KEYUP
        SendInput(1, up, Marshal.SizeOf(typeof(INPUT)));
    }
}
"@
[Input]::SetProcessDPIAware() | Out-Null

$scan = @{ ENTER = 0x1C; ESC = 0x01; SPACE = 0x39; TAB = 0x0F; E = 0x12; W = 0x11; A = 0x1E; S = 0x1F; D = 0x20;
           UP = 0x48; DOWN = 0x50; LEFT = 0x4B; RIGHT = 0x4D; GRAVE = 0x29 }
$extended = @('UP', 'DOWN', 'LEFT', 'RIGHT')

# For "type:" steps; "_" types a space (steps are separated by spaces).
$chars = @{}
$rows = @(@('1234567890', 0x02), @('qwertyuiop', 0x10), @('asdfghjkl', 0x1E), @('zxcvbnm', 0x2C))
foreach ($row in $rows) { for ($i = 0; $i -lt $row[0].Length; $i++) { $chars[$row[0][$i]] = $row[1] + $i } }
$chars[[char]'_'] = 0x39
$chars[[char]'.'] = 0x34

function Focus {
    $p = Get-Process -Name $process -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($p -and $p.MainWindowHandle -ne 0) { [Input]::SetForegroundWindow($p.MainWindowHandle) | Out-Null }
}

function Shot([string]$name) {
    $b = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
    $bmp = New-Object System.Drawing.Bitmap $b.Width, $b.Height
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.CopyFromScreen($b.Location, [System.Drawing.Point]::Empty, $b.Size)
    $bmp.Save((Join-Path $outDir "$name.png"))
}

# Screen point at the centre of an app control from the --hits file: "kind" or "kind:index-or-name".
function HitPoint([string]$spec) {
    $kind, $which = $spec -split ':', 2
    for ($try = 0; $try -lt 20; $try++) {
        $lines = @(Get-Content $hits -ErrorAction SilentlyContinue)
        if ($lines.Count -gt 0) {
            $dpi = [double](($lines[0] -split "`t")[1])
            foreach ($l in ($lines | Select-Object -Skip 1)) {
                $f = $l -split "`t"
                if ($f[0] -ne $kind) { continue }
                if ($which -and $f[1] -ne $which -and -not ($f[6] -like "*$($which.Replace('|', ' '))*")) { continue }
                $p = Get-Process -Name $process | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
                $pt = New-Object Input+POINT
                $pt.X = [int](([double]$f[2] + [double]$f[4]) / 2 * $dpi / 96)
                $pt.Y = [int](([double]$f[3] + [double]$f[5]) / 2 * $dpi / 96)
                [Input]::ClientToScreen($p.MainWindowHandle, [ref]$pt) | Out-Null
                return $pt
            }
        }
        Start-Sleep -Milliseconds 250
    }
    return $null
}

# A control by its name in the target process's windows. UI Automation lists a dialog under
# the window that owns it, so task dialogs and file dialogs are found there.
function DialogElement([string]$name) {
    $A = [System.Windows.Automation.AutomationElement]
    $named = New-Object System.Windows.Automation.PropertyCondition($A::NameProperty, $name)
    for ($try = 0; $try -lt 25; $try++) {
        $ids = @(Get-Process -Name $process -ErrorAction SilentlyContinue | ForEach-Object { $_.Id })
        $windows = $A::RootElement.FindAll([System.Windows.Automation.TreeScope]::Children,
                                           [System.Windows.Automation.Condition]::TrueCondition) |
                   Where-Object { $ids -contains $_.Current.ProcessId }
        foreach ($d in $windows) {
            $e = $d.FindFirst([System.Windows.Automation.TreeScope]::Descendants, $named)
            if ($e) { return $e }
        }
        Start-Sleep -Milliseconds 300
    }
    return $null
}

function Note([string]$text) { Add-Content (Join-Path $outDir 'input.log') "$(Get-Date -Format HH:mm:ss)   $text" }

foreach ($step in $steps -split '\s+') {
    $kind, $arg = $step -split ':', 2
    switch ($kind) {
        'wait' { Start-Sleep -Seconds ([double]$arg) }
        'key' { Focus; Start-Sleep -Milliseconds 200; [Input]::Press([uint16]$scan[$arg], $extended -contains $arg) }
        'type' {
            Focus
            foreach ($c in $arg.ToLower().ToCharArray()) { [Input]::Press([uint16]$chars[$c], $false); Start-Sleep -Milliseconds 60 }
        }
        'shot' { Shot $arg }
        'rawkey' { [Input]::Press([uint16]$scan[$arg], $extended -contains $arg) }
        'click' {
            $pt = HitPoint $arg
            if ($pt) { [Input]::Click($pt.X, $pt.Y) } else { Note "NOT FOUND: $arg" }
        }
        'wheel' {
            $target, $notches = $arg -split ':(?=-?\d+$)'
            $pt = HitPoint $target
            if ($pt) { [Input]::Wheel($pt.X, $pt.Y, [int]$notches * 120) } else { Note "NOT FOUND: $target" }
        }
        { $_ -in 'dialog', 'check' } {
            $e = DialogElement $arg.Replace('|', ' ')
            if ($e) {
                $r = $e.Current.BoundingRectangle
                Note "$kind '$($arg.Replace('|', ' '))' enabled=$($e.Current.IsEnabled) type=$($e.Current.ControlType.ProgrammaticName) rect=$r"
                [Input]::Click([int]($r.X + $r.Width / 2), [int]($r.Y + $r.Height / 2))
            } else { Note "NOT FOUND: $arg" }
        }
        'text' {
            $t = [regex]::Replace($arg.Replace('|', ' '), '[+^%~(){}\[\]]', '{$0}')
            [System.Windows.Forms.SendKeys]::SendWait($t)
        }
        'closewin' {
            $A = [System.Windows.Automation.AutomationElement]
            $w = $A::RootElement.FindAll([System.Windows.Automation.TreeScope]::Children,
                                         [System.Windows.Automation.Condition]::TrueCondition) |
                 Where-Object { $_.Current.Name -like "*$($arg.Replace('|', ' '))*" } | Select-Object -First 1
            if ($w) { $w.GetCurrentPattern([System.Windows.Automation.WindowPattern]::Pattern).Close() } else { Note "NOT FOUND: window $arg" }
        }
        'copy' { $src, $dst = $arg -split '>', 2; Copy-Item $src.Replace('|', ' ') (Join-Path $outDir $dst) -ErrorAction SilentlyContinue }
        'focus' { Focus }
        'clickrel' {
            # fractions of the target window's width and height
            $fx, $fy = $arg -split ','
            $p = Get-Process -Name $process | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
            $r = New-Object Input+RECT
            [Input]::GetWindowRect($p.MainWindowHandle, [ref]$r) | Out-Null
            [Input]::Click([int]($r.Left + [double]$fx * ($r.Right - $r.Left)), [int]($r.Top + [double]$fy * ($r.Bottom - $r.Top)))
        }
        'clickat' { $x, $y = $arg -split ','; [Input]::Click([int]$x, [int]$y) }
        'kill' { Stop-Process -Name $arg -Force -ErrorAction SilentlyContinue }
        'waitlog' {
            $text = $arg.Replace('_', ' ')
            for ($i = 0; $i -lt 240; $i++) {
                if ($log -and (Test-Path $log) -and (Select-String -Path $log -SimpleMatch $text -Quiet)) { break }
                Start-Sleep -Milliseconds 500
            }
        }
    }
    Add-Content (Join-Path $outDir 'input.log') "$(Get-Date -Format HH:mm:ss) $step"
}
