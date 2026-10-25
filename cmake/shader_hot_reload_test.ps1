# Hot reload of a .kshader in a running editor (docs/engine/shaders.html), timed. In a fresh copy of the test project:
# 0. A screenshot of the red version, for comparison.
# 1. The editor shows hot.scene (cubes with materials/hot.mat: an unlit .kshader writing red). Once the log says it
#    compiled, hot.kshader is overwritten with blue, and the clock runs until the editor's screenshot (taken 2 frames
#    after "Recompiled shader 'shaders/hot.kshader'") exists. It must show blue and take under MaxSeconds.
# 2. A second editor start loads the blue shader from the cache (no compile), then hot.kshader is overwritten with a GLSL
#    error: the screenshot (Inspector showing hot.mat) must still show blue (the last good shader) and the log the error
#    at the .kshader's line 3.
# Usage: powershell -File shader_hot_reload_test.ps1 -Editor <kast_editor.exe> -Project <tests/project> -Out <dir>
param([string]$Editor, [string]$Project, [string]$Out, [double]$MaxSeconds = 2.0)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

if (Test-Path $Out) { Remove-Item -Recurse -Force $Out }
New-Item -ItemType Directory $Out | Out-Null
$proj = Join-Path $Out 'project'
Copy-Item -Recurse $Project $proj
$shader = Join-Path $proj 'shaders\hot.kshader'
function Write-Shader([string]$text) { [IO.File]::WriteAllText($shader, $text) }
Write-Shader "void fragment()`n{`n    ALBEDO = vec3(1.0, 0.0, 0.0);`n}`n"
[IO.File]::WriteAllText((Join-Path $proj 'materials\hot.mat'),
    "{`"version`": 3, `"unlit`": true, `"shader`": `"shaders/hot.kshader`"}`n")
$scene = [IO.File]::ReadAllText((Join-Path $proj 'shader_showcase.scene')) -replace '(\{ "id": "[0-9a-f-]+", "path": )?"materials/shader_showcase\.mat"( \})?', '"materials/hot.mat"' # a ref: its id too
[IO.File]::WriteAllText((Join-Path $proj 'hot.scene'), $scene)

# Pixels of the screenshot that are clearly pure blue ($channel 0) or red (2), sampling every 4th row and column.
function Count-Color([string]$png, [int]$channel) {
    $bitmap = [Drawing.Bitmap]::new($png)
    try {
        $rect = [Drawing.Rectangle]::new(0, 0, $bitmap.Width, $bitmap.Height)
        $data = $bitmap.LockBits($rect, 'ReadOnly', [Drawing.Imaging.PixelFormat]::Format32bppArgb)
        $bytes = [byte[]]::new($data.Stride * $bitmap.Height)
        [Runtime.InteropServices.Marshal]::Copy($data.Scan0, $bytes, 0, $bytes.Length)
        $bitmap.UnlockBits($data)
        $count = 0
        $o1 = ($channel + 1) % 3
        $o2 = ($channel + 2) % 3
        for ($y = 0; $y -lt $bitmap.Height; $y += 4) {
            for ($x = 0; $x -lt $bitmap.Width; $x += 4) {
                $i = $y * $data.Stride + $x * 4 # B, G, R, A
                if ($bytes[$i + $channel] -gt 150 -and $bytes[$i + $o1] -lt 90 -and $bytes[$i + $o2] -lt 90) { $count++ }
            }
        }
        return $count
    } finally { $bitmap.Dispose() }
}

# Starts the editor on hot.scene; returns the process. Its log goes to <name>.log.
function Start-Editor([string]$name, [string]$onLog, [string[]]$extra) {
    $work = Join-Path $Out "$name.run"
    New-Item -ItemType Directory $work | Out-Null
    $arguments = @('--scene', "`"$proj\hot.scene`"", '--frames', '100000', '--screenshot', "`"$Out\$name.png`"",
                   '--screenshot-on-log', "`"$onLog`"", '--size', '1600x900', '--focus-panel', 'Assets',
                   '--projects-file', "`"$work\projects.json`"", '--settings-file', "`"$work\settings.json`"") + $extra
    return Start-Process -FilePath $Editor -ArgumentList $arguments -WorkingDirectory $work -PassThru -NoNewWindow `
        -RedirectStandardOutput "$Out\$name.log" -RedirectStandardError "$Out\$name.err.log"
}

function Wait-Log([string]$name, [string]$text, [int]$seconds) {
    $deadline = (Get-Date).AddSeconds($seconds)
    while ((Get-Date) -lt $deadline) {
        if ((Test-Path "$Out\$name.log") -and (Get-Content -Raw "$Out\$name.log") -match [regex]::Escape($text)) { return }
        Start-Sleep -Milliseconds 20
    }
    throw "$name.log never said '$text'"
}

function Stop-Editor($process, [int]$seconds) {
    if (-not $process.WaitForExit($seconds * 1000)) {
        $process.Kill()
        throw "the editor did not exit within $seconds s"
    }
}

# 0. The red shader before any edit (a --screenshot run, which compiles before its shot; this fills the cache too).
$work = Join-Path $Out 'hot_before.run'
New-Item -ItemType Directory $work | Out-Null
Push-Location $work
& $Editor --scene "$proj\hot.scene" --frames 60 --screenshot "$Out\hot_before.png" --size 1600x900 --focus-panel Assets `
    --projects-file "$work\projects.json" --settings-file "$work\settings.json" *> "$Out\hot_before.log"
Pop-Location
$red = Count-Color "$Out\hot_before.png" 2
"Before the edit: $red red samples"
if ($red -lt 500) { throw "the red shader isn't on screen before the edit (only $red red samples)" }

# 1. Timed hot reload.
$editor1 = Start-Editor 'hot_reload' "2,Recompiled shader 'shaders/hot.kshader'" @()
try {
    Wait-Log 'hot_reload' "Compiled shader 'shaders/hot.kshader'" 60
    Start-Sleep -Seconds 1 # red on screen for a while
    $clock = [Diagnostics.Stopwatch]::StartNew()
    Write-Shader "void fragment()`n{`n    ALBEDO = vec3(0.0, 0.0, 1.0);`n}`n"
    while (-not ((Test-Path "$Out\hot_reload.png") -and (Get-Item "$Out\hot_reload.png").Length -gt 0)) {
        if ($clock.Elapsed.TotalSeconds -gt 30) { throw 'no screenshot within 30 s of the edit' }
        Start-Sleep -Milliseconds 5
    }
    $seconds = $clock.Elapsed.TotalSeconds
} finally { Stop-Editor $editor1 30 }
Start-Sleep -Milliseconds 200 # the PNG is complete once the editor exits
$blue = Count-Color "$Out\hot_reload.png" 0
"Hot reload: edit saved -> new shader on screen (screenshot written) in {0:N3} s; {1} blue samples" -f $seconds, $blue
if ($blue -lt 500) { throw "the reloaded shader isn't on screen (only $blue blue samples)" }
if ($seconds -ge $MaxSeconds) { throw ("hot reload took {0:N3} s, over {1} s" -f $seconds, $MaxSeconds) }

# 2. Cache hit on restart, then a broken edit.
$editor2 = Start-Editor 'hot_broken' "5,Shader 'shaders/hot.kshader': line" @('--assets-select', 'materials/hot.mat')
try {
    Wait-Log 'hot_broken' "Compiled shader 'shaders/hot.kshader' (cache hit)" 60
    Start-Sleep -Seconds 1
    Write-Shader "void fragment()`n{`n    ALBEDO = missingName;`n}`n"
} finally { Stop-Editor $editor2 30 }
$log = Get-Content -Raw "$Out\hot_broken.log"
$expected = "[error] Shader 'shaders/hot.kshader': line 3: error X3004: undeclared identifier 'missingName'; the last good version keeps rendering"
if (-not $log.Contains($expected)) { throw "hot_broken.log lacks: $expected" }
$blue = Count-Color "$Out\hot_broken.png" 0
"Broken edit: $blue blue samples (the last good shader keeps rendering)"
if ($blue -lt 500) { throw "the last good shader isn't on screen after the broken edit (only $blue blue samples)" }
"OK"
