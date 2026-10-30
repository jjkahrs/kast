# Map size benchmark (README.md): generates N x N tile maps under build/map_bench/<N>/ at run time and measures the
# editor, the runtime client and the world server on them. Every number goes to build/map_bench/results.log.
# Usage (from anywhere, Release builds of the editor and services/world first):
#   powershell -ExecutionPolicy Bypass -File tools/map_bench/map_bench.ps1 [-Sizes 16,32,64] [-Steps generate,bake,...]
# Steps: generate bake shots editor play player server load. Size 0 is today's default (tests/project/maps, START_MAP
# 1) for the server and load steps only.
param(
    [string[]]$Sizes = @('16', '32', '64'),
    [string[]]$Steps = @('generate', 'bake', 'editor', 'play', 'player', 'server', 'load'),
    [int]$TimeoutSec = 900,     # any one run (editor, player, server); a timed-out run is killed and reported
    [int]$BakeTimeoutSec = 3600, # kast_mapbake: ~0.3 s per tile, serial
    [int]$LoadCount = 500,      # tests/load.sh COUNT: today's default
    [int]$LoadDuration = 120,   # tests/load.sh DURATION, seconds
    [int]$Radius = 2,           # the bench scene's Terrain.radius (Play and kast_player)
    [string]$WindowPos = 'secondary', # KAST_WINDOW_POS for every window: off the main monitor
    [switch]$Force              # regenerate maps that already exist
)
$ErrorActionPreference = 'Stop'
# -File passes "a,b" as one string: split it.
$Sizes = @($Sizes -split ',' | ForEach-Object { [int]$_ })
$Steps = @($Steps -split ',')
$repo = (Resolve-Path "$PSScriptRoot/../..").Path
$out = Join-Path $repo 'build/map_bench'
New-Item -ItemType Directory -Force (Join-Path $out 'screenshots') | Out-Null
$editor = Join-Path $repo 'build/release/kast_editor.exe'
$world = Join-Path $repo 'services/world/build/release'
$bash = 'C:\Program Files\Git\bin\bash.exe'
$results = Join-Path $out 'results.log'
$env:KAST_WINDOW_POS = $WindowPos

function Log([string]$line) {
    $stamped = "$(Get-Date -Format s) $line"
    Write-Host $stamped
    Add-Content -Path $results -Value $stamped
}

function MapId([int]$n) { 200 + $n }
function MapDir([int]$n) { Join-Path $out "$n/project/maps/bench$n" }
function Mb([double]$bytes) { '{0:N0} MB' -f ($bytes / 1MB) }

# The GPU dedicated memory of process id (all adapters), bytes; 0 if the counter can't be read.
function GpuDedicated([int]$id) {
    try {
        $s = (Get-Counter "\GPU Process Memory(pid_${id}_*)\Dedicated Usage" -ErrorAction Stop).CounterSamples
        return ($s | Measure-Object CookedValue -Sum).Sum
    } catch { return 0 }
}

# Runs exe with argLine (one string, pre-quoted) in dir, its output in log(.err), killing it after TimeoutSec. Samples
# its peak working set, peak private bytes and (Gpu) peak GPU dedicated memory meanwhile; Until: seconds from start to
# the first log line matching that regex.
function Measure-Run([string]$exe, [string]$argLine, [string]$dir, [string]$log, [switch]$Gpu, [string]$Until, [int]$PollMs = 250, [int]$Timeout = $TimeoutSec) {
    New-Item -ItemType Directory -Force $dir | Out-Null
    $start = @{ FilePath = $exe; WorkingDirectory = $dir; RedirectStandardOutput = $log; RedirectStandardError = "$log.err" }
    if ($argLine) { $start.ArgumentList = $argLine }
    $p = Start-Process @start -NoNewWindow -PassThru
    $null = $p.Handle # keeps the exit code readable after exit
    $r = [ordered]@{ Exit = $null; Seconds = 0; PeakWs = [double]0; PeakPrivate = [double]0; PeakGpu = [double]0; UntilSec = $null; TimedOut = $false }
    $sw = [Diagnostics.Stopwatch]::StartNew()
    while (-not $p.HasExited) {
        try {
            $p.Refresh()
            $r.PeakWs = [math]::Max($r.PeakWs, $p.PeakWorkingSet64)
            $r.PeakPrivate = [math]::Max($r.PeakPrivate, $p.PrivateMemorySize64)
        } catch {}
        if ($Gpu) { $r.PeakGpu = [math]::Max($r.PeakGpu, (GpuDedicated $p.Id)) }
        if ($Until -and $null -eq $r.UntilSec -and (Test-Path $log) -and
            (Select-String -Path $log, "$log.err" -Pattern $Until -Quiet -ErrorAction SilentlyContinue)) {
            $r.UntilSec = $sw.Elapsed.TotalSeconds
        }
        if ($sw.Elapsed.TotalSeconds -gt $Timeout) {
            Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
            $r.TimedOut = $true
            break
        }
        Start-Sleep -Milliseconds $PollMs
    }
    $p.WaitForExit()
    $r.Seconds = $sw.Elapsed.TotalSeconds
    $r.Exit = $p.ExitCode
    if ($Until -and $null -eq $r.UntilSec -and (Select-String -Path $log, "$log.err" -Pattern $Until -Quiet)) {
        $r.UntilSec = $r.Seconds # it printed and exited between two polls
    }
    return $r
}

function LogText([string]$log) { (Get-Content $log, "$log.err" -Raw -ErrorAction SilentlyContinue) -join "`n" }
function Status($r) { if ($r.TimedOut) { "TIMED OUT after $('{0:N0}' -f $r.Seconds) s" } else { "exit $($r.Exit) after $('{0:N1}' -f $r.Seconds) s" } }
function LastError([string]$log) {
    $e = Select-String -Path $log, "$log.err" -Pattern '\[error\]|fatal|error' -ErrorAction SilentlyContinue | Select-Object -Last 1
    if ($e) { " last error: $($e.Line.Trim())" } else { '' }
}
function EditorArgs([string]$work) { "--no-recycle --projects-file `"$work/projects.json`" --settings-file `"$work/editor_settings.json`"" }

# The camera flight script, with the map's extent (refreshed by play and player, so an edit needs no new map).
function CopyMoveScript([int]$n) {
    (Get-Content "$PSScriptRoot/map_bench_move.lua" -Raw).Replace('@EXTENT@', "$($n * 256)") |
        Set-Content -NoNewline -Encoding ascii (Join-Path $out "$n/project/scripts/map_bench_move.lua")
}

# --- generate: project.kast, the grass material and its texture, the move script, a scene, and maps/bench<N> made by
# the editor's own zone flags (heights generated, spawns, saved) from a map.json written here: the New Map cap is
# never involved, so this works the same before and after it changes.
function Generate([int]$n) {
    $root = Join-Path $out "$n/project"
    if ((Test-Path (Join-Path (MapDir $n) 'heights')) -and -not $Force) { Log "N=$n generate: kept $root (-Force regenerates)"; return }
    Remove-Item -Recurse -Force (Join-Path $out $n) -ErrorAction SilentlyContinue
    $src = Join-Path $repo 'tests/project'
    foreach ($d in 'materials', 'textures', 'scripts', 'scenes', "maps/bench$n") { New-Item -ItemType Directory -Force (Join-Path $root $d) | Out-Null }
    Copy-Item "$src/materials/terrain_grass.mat*" (Join-Path $root 'materials')
    Copy-Item "$src/textures/checker.png*" (Join-Path $root 'textures')
    $extent = $n * 256
    CopyMoveScript $n
    Set-Content -Encoding ascii (Join-Path $root 'project.kast') "{`n  `"version`": 1,`n  `"name`": `"Map Bench`",`n  `"startScene`": `"scenes/bench.scene`"`n}"
    $id = MapId $n
    $c = $extent / 2
    Set-Content -Encoding ascii (Join-Path (MapDir $n) 'map.json') @"
{
  "version": 1,
  "id": $id,
  "name": "Bench $n",
  "tiles": [[0, 0, $($n - 1), $($n - 1)]],
  "heightRange": [0, 400],
  "material": "materials/terrain_grass.mat",
  "spawn": {"pos": [$c, 0, $c], "heading": 0}
}
"@
    # The camera looks along +x +z, 30 degrees down (map_bench_move.lua flies it along that diagonal).
    Set-Content -Encoding ascii (Join-Path $root 'scenes/bench.scene') @"
{
  "version": 5,
  "entities": [
    { "id": 1, "name": "Sun", "parent": null, "components": {
        "Transform": { "position": [$c, 500, $c], "rotation": [-0.115425, 0.799192, 0.16269, 0.567011], "scale": [1, 1, 1] },
        "DirectionalLight": { "color": [1, 0.95, 0.9], "intensity": 1 } } },
    { "id": 2, "name": "Terrain", "parent": null, "components": {
        "Transform": { "position": [0, 0, 0], "rotation": [0, 0, 0, 1], "scale": [1, 1, 1] },
        "Terrain": { "map": $id, "radius": $Radius } } },
    { "id": 3, "name": "Main Camera", "parent": null, "components": {
        "Transform": { "position": [128, 450, 128], "rotation": [-0.099046, -0.892399, -0.239118, 0.369643], "scale": [1, 1, 1] },
        "Camera": { "verticalFovDegrees": 60, "nearPlane": 0.5, "farPlane": 3000 } } }
  ],
  "systems": ["scripts/map_bench_move.lua"],
  "lighting": { "ambientColor": [0.6, 0.7, 1], "ambientIntensity": 0.3 }
}
"@
    # Heights with real relief (fBm, 1 km features, 0..400 m), one Bat spawn per 4 x 4 tiles, the map spawn on the ground.
    # ponytail: bench8's centre is on a slope too steep for the navmesh (every load bot move corrected off_mesh), so its
    # spawn moves to a walkable spot found by hand; a size whose spawn lands off the mesh needs an entry here.
    $spawn = if ($n -eq 8) { 600 } else { $c }
    $edits = @('--map', "bench$n", '--generate-heights', '7,1024,6,0.5,2', '--set-spawn', "$spawn,$spawn")
    for ($z = 2; $z -lt $n; $z += 4) { for ($x = 2; $x -lt $n; $x += 4) { $edits += '--add-spawn', "bat_${x}_$z,Bat,$($x * 256 + 128),$($z * 256 + 128)" } }
    $edits += '--save-zone', '--headless'
    $work = Join-Path $out "$n/generate.run"
    $r = Measure-Run $editor "--project `"$root`" $(EditorArgs $work) $($edits -join ' ')" $work (Join-Path $out "$n/generate.log")
    $bytes = (Get-ChildItem (Join-Path (MapDir $n) 'heights') -ErrorAction SilentlyContinue | Measure-Object Length -Sum).Sum
    Log "N=$n generate: $(Status $r), peak RAM $(Mb $r.PeakWs), heights $(Mb $bytes) on disk$(if ($r.Exit -ne 0) { LastError (Join-Path $out "$n/generate.log") })"
}

# --- bake: kast_mapbake on the map (lint + one navtile per tile).
function Bake([int]$n) {
    $log = Join-Path $out "$n/mapbake.log"
    $r = Measure-Run (Join-Path $world 'kast_mapbake.exe') "`"$(MapDir $n)`"" (Join-Path $out $n) $log -Timeout $BakeTimeoutSec
    $nav = Get-ChildItem (Join-Path (MapDir $n) '.server/nav') -Filter *.navtile -ErrorAction SilentlyContinue | Measure-Object Length -Sum
    $polys = (Select-String -Path $log -Pattern '\.navtile: ([0-9]+) polys' -ErrorAction SilentlyContinue | ForEach-Object { [int]$_.Matches[0].Groups[1].Value } | Measure-Object -Maximum).Maximum
    Log "N=$n bake: $(Status $r), $($nav.Count) navtiles, $(Mb $nav.Sum), at most $polys polys per tile, peak RAM $(Mb $r.PeakWs)$(if ($r.Exit -ne 0) { LastError $log })"
}

# --- shots: the Zone tab's Map section and the Map tab on the map.
function Shots([int]$n) {
    $dir = Join-Path $out 'screenshots'
    $scene = Join-Path $out "$n/project/scenes/bench.scene"
    foreach ($shot in @(@('zone_tab', '--focus-panel Zone --zone-section Map'), @('map_tab', '--focus-panel Map'))) {
        $png = Join-Path $dir "$($shot[0])_${n}x$n.png"
        $work = "$png.run"
        $r = Measure-Run $editor "--scene `"$scene`" $(EditorArgs $work) --map bench$n --size 1600x1000 $($shot[1]) --frames 60 --screenshot `"$png`"" $work "$png.log"
        Log "N=$n shot $($shot[0]): $(Status $r) -> $png"
    }
}

# --- editor: the sculpting benchmark (--sculpt-loop --log-fps --no-vsync), RAM and GPU memory.
function EditorSculpt([int]$n) {
    $work = Join-Path $out "$n/editor.run"
    $log = Join-Path $out "$n/editor.log"
    $scene = Join-Path $out "$n/project/scenes/bench.scene"
    $r = Measure-Run $editor "--scene `"$scene`" $(EditorArgs $work) --map bench$n --sculpt-loop --log-fps --no-vsync --size 1600x900 --frames 1200 --screenshot `"$(Join-Path $out "screenshots/editor_sculpt_${n}x$n.png")`"" $work $log -Gpu
    $fps = [Collections.Generic.List[double[]]]::new() # one [fps, cpu ms, gpu ms] per one-second window
    foreach ($m in Select-String -Path $log, "$log.err" -Pattern 'FPS ([0-9.]+), CPU ([0-9.]+) ms/frame, GPU ([0-9.]+) ms/frame' -ErrorAction SilentlyContinue) {
        $g = $m.Matches[0].Groups
        $fps.Add([double[]]@([double]$g[1].Value, [double]$g[2].Value, [double]$g[3].Value))
    }
    # Steady state: the last 10 one-second windows (the first ones build every tile's mesh at ~1 FPS).
    $steady = if ($fps.Count -gt 10) { $fps.GetRange($fps.Count - 10, 10) } else { $fps }
    $line = "N=$n editor sculpt: $(Status $r)"
    if ($steady.Count) {
        $avg = ($steady | ForEach-Object { $_[0] } | Measure-Object -Average -Minimum)
        $cpu = ($steady | ForEach-Object { $_[1] } | Measure-Object -Average).Average
        $gpu = ($steady | ForEach-Object { $_[2] } | Measure-Object -Average).Average
        $slow = 0
        while ($slow -lt $fps.Count -and $fps[$slow][0] -lt $avg.Average / 2) { $slow++ }
        $line += ", FPS $('{0:N1}' -f $avg.Average) average, $('{0:N1}' -f $avg.Minimum) lowest over the last $($steady.Count) s, CPU $('{0:N2}' -f $cpu) ms, GPU $('{0:N2}' -f $gpu) ms/frame; $slow s at under half that FPS after opening"
    } else {
        $line += ', no FPS logged'
    }
    $draws = Select-String -Path $log -Pattern 'Scene view: Tris \S+\s+Draws ([0-9]+)' -ErrorAction SilentlyContinue | Select-Object -Last 1
    if ($draws) { $line += ", Scene view $($draws.Matches[0].Groups[1].Value) draws" }
    Log "$line, peak RAM $(Mb $r.PeakWs) (private $(Mb $r.PeakPrivate)), peak GPU dedicated $(Mb $r.PeakGpu)$(if ($r.Exit -ne 0) { LastError $log })"
}

$moveRegex = 'map bench: frame time ([0-9.]+) ms average, ([0-9.]+) ms 99th percentile, ([0-9.]+) ms at most over ([0-9]+) frames, ([0-9]+) m flown'
function MoveResult([string]$log) {
    $m = Select-String -Path $log, "$log.err" -Pattern $moveRegex -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($m) { $g = $m.Matches[0].Groups; "frame $($g[1].Value) ms average, $($g[2].Value) ms p99, $($g[3].Value) ms max over $($g[4].Value) frames ($($g[5].Value) m flown)" } else { 'no frame time logged' }
}

# --- play: editor Play of the bench scene, the camera flying across the map.
function Play([int]$n) {
    CopyMoveScript $n
    $work = Join-Path $out "$n/play.run"
    $log = Join-Path $out "$n/play.log"
    $scene = Join-Path $out "$n/project/scenes/bench.scene"
    $r = Measure-Run $editor "--scene `"$scene`" $(EditorArgs $work) --play --no-vsync --no-audio-device --size 1600x900 --frames 3200 --screenshot `"$(Join-Path $out "screenshots/play_${n}x$n.png")`"" $work $log -Gpu
    Log "N=$n play (editor, Terrain.radius $Radius): $(Status $r), $(MoveResult $log), peak RAM $(Mb $r.PeakWs), peak GPU dedicated $(Mb $r.PeakGpu)$(if ($r.Exit -ne 0) { LastError $log })"
}

# --- player: Build the project, then the same flight in kast_player.
function Player([int]$n) {
    CopyMoveScript $n
    $game = Join-Path $out "$n/game"
    $root = Join-Path $out "$n/project"
    Remove-Item -Recurse -Force $game -ErrorAction SilentlyContinue
    $r = Measure-Run $editor "--project `"$root`" --build `"$game`"" (Join-Path $out $n) (Join-Path $out "$n/build.log")
    $pak = (Get-Item (Join-Path $game 'game.pak') -ErrorAction SilentlyContinue).Length
    Log "N=$n player build: $(Status $r), game.pak $(Mb $pak)"
    if ($r.Exit -ne 0) { return }
    $log = Join-Path $out "$n/player.log"
    $r = Measure-Run (Join-Path $game 'Map Bench.exe') "--no-vsync --no-audio-device --size 1600x900 --frames 3200 --screenshot `"$(Join-Path $out "screenshots/player_${n}x$n.png")`"" $game $log -Gpu
    Log "N=$n player (Terrain.radius $Radius): $(Status $r), $(MoveResult $log), peak RAM $(Mb $r.PeakWs), peak GPU dedicated $(Mb $r.PeakGpu)$(if ($r.Exit -ne 0) { LastError $log })"
}

# --- server: kast_world (Release, Windows) on the map until "maps loaded" (no database: it stops right after, as
# cmake/zone_test.cmake runs it): startup time and peak RSS.
function Server([int]$n) {
    $maps = if ($n -eq 0) { Join-Path $repo 'tests/project/maps' } else { Join-Path $out "$n/project/maps" }
    $vars = @{ MAP_ROOT = $maps; START_MAP = $(if ($n -eq 0) { '1' } else { "$(MapId $n)" }); AUTH_JWKS_URL = 'http://127.0.0.1:9/jwks'
        DB_HOST = '127.0.0.1'; DB_PORT = '9'; DB_USER = 'x'; DB_PASSWORD = 'x'; DB_NAME = 'x'; REALM_ID = '1'
        DB_CONNECT_TIMEOUT_SECONDS = '1'; LUA_ROOT = (Join-Path $repo 'services/world/lua') }
    foreach ($k in $vars.Keys) { Set-Item "env:$k" $vars[$k] }
    $log = Join-Path $out "server_$n.log"
    try { $r = Measure-Run (Join-Path $world 'kast_world.exe') '' $out $log -Until '"maps loaded"' -PollMs 20 }
    finally { foreach ($k in $vars.Keys) { Remove-Item "env:$k" } }
    $what = if ($n -eq 0) { 'N=0 (tests/project/maps)' } else { "N=$n" }
    $loaded = if ($null -ne $r.UntilSec) { "maps loaded after $('{0:N2}' -f $r.UntilSec) s" } else { "maps NOT loaded$(LastError $log)" }
    Log "$what server startup: $loaded, peak RSS $(Mb $r.PeakWs) ($(Status $r))"
}

# --- load: services/world/tests/load.sh against the compose stack (docker compose up -d first) on the map, bots
# wandering over the whole map (ZONE) at its ground height (TERRAIN). Wanderers don't follow the navmesh, so a step
# onto a slope too steep for it is corrected (off_mesh): load.sh's bot summary FAILs on those; the tick p99 is the
# number to compare.
function Load([int]$n) {
    $log = Join-Path $out "load_$n.log"
    $env:COUNT = "$LoadCount"; $env:DURATION = "$LoadDuration"; $env:RUN_ID = "mapbench${n}x$([DateTimeOffset]::Now.ToUnixTimeSeconds())"
    if ($n -ne 0) { $env:MAP_DIR = (Join-Path $out "$n/project/maps") -replace '\\', '/'; $env:START_MAP = "$(MapId $n)"; $env:ZONE = "$($n * 256)"; $env:TERRAIN = "bench$n" }
    try { $null = Measure-Run $bash "`"$(Join-Path $repo 'services/world/tests/load.sh')`"" $out $log -Timeout ($LoadDuration + 900) }
    finally { foreach ($k in 'COUNT', 'DURATION', 'RUN_ID', 'MAP_DIR', 'START_MAP', 'ZONE', 'TERRAIN') { Remove-Item "env:$k" -ErrorAction SilentlyContinue } }
    $t = LogText $log
    $pick = { param($label) if ($t -match "$label\s+([^\r\n]*)") { $Matches[1].Trim() } else { '?' } }
    $what = if ($n -eq 0) { 'N=0 (tests/project/maps, START_MAP 1)' } else { "N=$n" }
    Log "$what load ($LoadCount bots, $LoadDuration s): worst window p99 $(& $pick 'worst window tick p99:'), run p99 $(& $pick 'run total tick p99:'), enter p50/p99 $(& $pick 'enter p50 / p99:'), corrections $(if ($t -match '"corrections_recv":([0-9]+)') { $Matches[1] } else { '?' }), $(if ($t -match '(PASS|FAIL) load test') { $Matches[0] } else { 'no result' }) ($log)"
}

Log "map_bench: sizes $($Sizes -join ',') steps $($Steps -join ','), commit $(git -C $repo rev-parse --short HEAD)"
foreach ($n in $Sizes) {
    foreach ($step in $Steps) {
        if ($n -eq 0 -and $step -notin 'server', 'load') { continue }
        switch ($step) {
            'generate' { Generate $n } 'bake' { Bake $n } 'shots' { Shots $n } 'editor' { EditorSculpt $n }
            'play' { Play $n } 'player' { Player $n } 'server' { Server $n } 'load' { Load $n }
            default { throw "unknown step $step" }
        }
    }
}
