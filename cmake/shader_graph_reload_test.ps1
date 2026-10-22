# A .kshadergraph changed on disk while the Shader Graph panels have it open (docs/editor/shader-graph.html). In a fresh
# copy of the test project:
# 1. Clean: the graph reloads (logged), and the screenshot shows the new node.
# 2. Dirty (an unsaved --graph-add): nothing reloads; Reload / Keep mine is asked (the screenshot shows the prompt).
# Usage: powershell -File shader_graph_reload_test.ps1 -Editor <kast_editor.exe> -Project <tests/project> -Out <dir>
param([string]$Editor, [string]$Project, [string]$Out)
$ErrorActionPreference = 'Stop'

if (Test-Path $Out) { Remove-Item -Recurse -Force $Out }
New-Item -ItemType Directory $Out | Out-Null
$proj = Join-Path $Out 'project'
Copy-Item -Recurse $Project $proj
$graph = Join-Path $proj 'shaders\graph_demo.kshadergraph'
$original = [IO.File]::ReadAllText($graph)
# The same graph with a Fresnel node (id 50) added, as another program would save it.
$changed = $original -replace '"nodes": \[', "`"nodes`": [`n    {`"id`": 50, `"type`": `"fresnel`", `"position`": [560.0, 330.0]},"

# Starts the editor on the graph; the screenshot comes 10 frames after the log shows onLog. Returns the process.
function Start-Editor([string]$name, [string]$onLog, [string[]]$extra) {
    $work = Join-Path $Out "$name.run"
    New-Item -ItemType Directory $work | Out-Null
    $arguments = @('--project', "`"$proj`"", '--open-graph', 'shaders/graph_demo.kshadergraph', '--frames', '100000',
                   '--screenshot', "`"$Out\$name.png`"", '--screenshot-on-log', "`"10,$onLog`"", '--size', '1600x900',
                   '--projects-file', "`"$work\projects.json`"", '--settings-file', "`"$work\settings.json`"") + $extra
    return Start-Process -FilePath $Editor -ArgumentList $arguments -WorkingDirectory $work -PassThru -NoNewWindow `
        -RedirectStandardOutput "$Out\$name.log" -RedirectStandardError "$Out\$name.err.log"
}

function Wait-Log([string]$name, [string]$text, [int]$seconds) {
    $deadline = (Get-Date).AddSeconds($seconds)
    while ((Get-Date) -lt $deadline) {
        if ((Test-Path "$Out\$name.log") -and (Get-Content -Raw "$Out\$name.log") -match [regex]::Escape($text)) { return }
        Start-Sleep -Milliseconds 50
    }
    throw "$name.log never said '$text'"
}

function Stop-Editor($process, [int]$seconds) {
    if (-not $process.WaitForExit($seconds * 1000)) {
        $process.Kill()
        throw "the editor did not exit within $seconds s"
    }
}

# 1. Clean: reloaded.
$editor1 = Start-Editor 'reload_clean' 'changed on disk: reloaded' @()
try {
    Wait-Log 'reload_clean' 'ImGui initialized' 60
    Start-Sleep -Seconds 2 # past the editor's first frames (its graph steps run at frame 17)
    [IO.File]::WriteAllText($graph, $changed)
} finally { Stop-Editor $editor1 60 }
$log = Get-Content -Raw "$Out\reload_clean.log"
if ($log -notmatch "changed on disk: reloaded") { throw 'reload_clean.log: the clean graph did not reload' }
if ($log -match 'Reload / Keep mine') { throw 'reload_clean.log: a clean graph asked Reload / Keep mine' }
"Clean: reloaded"

# 2. Dirty: asked, not reloaded.
[IO.File]::WriteAllText($graph, $original)
$editor2 = Start-Editor 'reload_conflict' 'Reload / Keep mine' @('--graph-add', 'multiply,560,330')
try {
    Wait-Log 'reload_conflict' '--graph-add multiply,560,330: node 12' 60
    Start-Sleep -Seconds 1
    [IO.File]::WriteAllText($graph, $changed)
} finally { Stop-Editor $editor2 60 }
$log = Get-Content -Raw "$Out\reload_conflict.log"
if ($log -notmatch 'changed on disk while it has unsaved changes: Reload / Keep mine') { throw 'reload_conflict.log: no prompt' }
if ($log -match 'changed on disk: reloaded') { throw 'reload_conflict.log: a dirty graph reloaded without asking' }
if (-not (Test-Path "$Out\reload_conflict.png")) { throw 'reload_conflict.png was not written' }
"Dirty: Reload / Keep mine asked"
"OK"
