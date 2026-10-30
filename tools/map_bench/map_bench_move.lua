-- Map size benchmark (tools/map_bench, copied into each generated project as scripts/map_bench_move.lua): flies the
-- Main Camera STEP m a frame on x and on z along the map's diagonal from the first tile's centre (back and forth), so
-- every run streams the same tiles on the same frames whatever the map size, and logs the frame time (each update's
-- dt: no --fixed-dt, no vsync) over MEASURE frames after WARMUP. EXTENT (the map's edge in metres) is filled in by
-- map_bench.ps1.
local EXTENT, STEP, HEIGHT, WARMUP, MEASURE = @EXTENT@, 0.7, 450, 120, 3000
local frame, camera, ms = 0, nil, {}

return {
    update = function(dt)
        frame = frame + 1
        if not camera then
            for e in kast.query("Camera") do camera = e end
        end
        local span = EXTENT - 256 -- tile centre to tile centre
        local d = (STEP * frame) % (2 * span)
        if d > span then d = 2 * span - d end
        kast.set(camera, "Transform", {position = {x = 128 + d, y = HEIGHT, z = 128 + d}})
        if frame > WARMUP and #ms < MEASURE then
            ms[#ms + 1] = 1000 * dt
            if #ms == MEASURE then
                local sum, max = 0, 0
                for _, v in ipairs(ms) do sum, max = sum + v, math.max(max, v) end
                table.sort(ms)
                kast.log(string.format("map bench: frame time %.3f ms average, %.3f ms 99th percentile, %.3f ms at most " ..
                                       "over %d frames, %.0f m flown", sum / MEASURE, ms[math.ceil(0.99 * MEASURE)], max,
                                       MEASURE, STEP * frame * math.sqrt(2)))
            end
        end
    end,
}
