-- SPDX-License-Identifier: GPL-3.0-or-later
-- temp-spark: the reference HelixScreen plugin. One heater's temperature as a
-- home tile with a 30-sample sparkline, a detail overlay with gridlines and
-- stats, and three schema settings. Every HelixScreen API it uses is shown once.

local N = 30         -- samples in the sparkline window
local MIN_SPAN = 10  -- smallest plotted temperature span, in degrees
local PAD = 0.6      -- half-height of the plot as a share of the span (20% padding)
local FILL_OPA = 15  -- area-fill opacity under the line, in percent

-- One heater choice maps to Moonraker's temperature-store key, the stable
-- helix.printer field names for live values, and a short display label. The
-- chamber store key is the conventional Klipper object name; a printer whose
-- chamber is configured under another name simply backfills empty.
local HEATERS = {
    extruder = {
        store = "extruder",
        temp = "extruder_temp",
        target = "extruder_target",
        label = "Extruder",
    },
    heater_bed = {
        store = "heater_bed",
        temp = "bed_temp",
        target = "bed_target",
        label = "Bed",
    },
    chamber = {
        store = "heater_generic chamber",
        temp = "chamber_temp",
        target = "chamber_target",
        label = "Chamber",
    },
}

local function selected()
    return HEATERS[helix.settings.get("heater")] or HEATERS.extruder
end

-- Subjects carry the plugin's text output: XML binds to them, Lua only sets
-- them. A name given here registers as temp-spark__<name>.
local value_text = helix.subject.string("value", "--")
local target_beside = helix.subject.string("target_beside", "")
local min_text = helix.subject.string("min_text", "--")
local max_text = helix.subject.string("max_text", "--")
local avg_text = helix.subject.string("avg_text", "--")
local target_text = helix.subject.string("target_text", "--")
local window_text = helix.subject.string("window_text", "")
local heater_label = helix.subject.string("heater_label", "Extruder")
local show_target = helix.subject.int("show_target", 1)

-- Each canvas is a retained drawing: draw() rebuilds its list from the window
-- and commits it, and the widget replays it until the next commit.
local spark = helix.canvas("spark")
local graph = helix.canvas("graph")

-- The window holds the sampled temperatures, oldest first. `latest` and
-- `target_now` are the live readings waiting for the next sample tick.
local samples = {}
local latest = nil
local target_now = nil
local timer = nil

-- Bare whole degrees: the built-in temp tiles print "220 / 220" with one
-- muted degree unit carried by the XML, so the tile matches them.
local function num(t)
    if not t then
        return "--"
    end
    return math.floor(t + 0.5)
end

local function fmt(t)
    if not t then
        return "--"
    end
    return num(t) .. "\u{00B0}"
end

-- A target of 0 means "off"; every target display (the beside text, the dashed
-- line, the stat chip) keys off this one rule.
local function target_on()
    return target_now and target_now > 0 and helix.settings.get("show_target")
end

-- Min, max and mean of the window; all nil when the window is empty.
local function stats()
    if #samples == 0 then
        return nil
    end
    local lo, hi, sum = samples[1], samples[1], 0
    for _, t in ipairs(samples) do
        lo, hi = math.min(lo, t), math.max(hi, t)
        sum = sum + t
    end
    return lo, hi, sum / #samples
end

-- The plot's vertical range: the window's own span widened to at least
-- MIN_SPAN, then padded 20% so the line never touches an edge. A drawn target
-- joins the range so the dashed line always lands inside the plot.
local function range(lo, hi)
    if target_on() then
        lo = math.min(lo, target_now)
        hi = math.max(hi, target_now)
    end
    local half = math.max(hi - lo, MIN_SPAN) * PAD
    local mid = (lo + hi) / 2
    return mid - half, mid + half
end

-- One drawing for both surfaces: the tile sparkline and the detail plot show
-- the same window, so draw() takes the canvas plus a detail flag for the
-- gridlines and the target trace. X is the sample's slot in the N-slot window,
-- so the line hugs the right edge while the window is still filling.
local function draw(c, detail)
    local w, h = c:size()
    if w > 4 and h > 4 and #samples >= 2 then
        local lo, hi = stats()
        lo, hi = range(lo, hi)
        -- The newest dot needs its radius clear of the right edge, so the line
        -- stops a little short of it instead of touching the canvas border.
        local xmax = w - 1 - (detail and 4 or 3)
        local pts = {}
        for i, t in ipairs(samples) do
            pts[#pts + 1] = (N - #samples + i - 1) * xmax / (N - 1)
            pts[#pts + 1] = (h - 1) * (1 - (t - lo) / (hi - lo))
        end

        -- The area fill is one 1px column per sample at low opacity; the
        -- polyline drawn on top at full strength gives the fill its top edge.
        for i = 1, #samples - 1 do
            local x0, y0 = pts[i * 2 - 1], pts[i * 2]
            local w0 = math.max(pts[i * 2 + 1] - x0, 1)
            if y0 < h - 1 then
                c:rect(x0, y0, w0, h - 1 - y0, {fill = "primary", opa = FILL_OPA})
            end
        end

        if detail then
            -- Gridlines at a whole-degree step (1, 2, 5, 10, 25, 50) labelled
            -- at the left edge; the built-in graph picks its step the same way.
            local step = 50
            for _, s in ipairs({1, 2, 5, 10, 25, 50}) do
                if s >= (hi - lo) / 4 then
                    step = s
                    break
                end
            end
            local v = math.ceil(lo / step) * step
            while v <= hi do
                local y = (h - 1) * (1 - (v - lo) / (hi - lo))
                c:line(0, y, w - 1, y, {color = "border"})
                if y >= 14 then
                    -- math.ceil yields a float, so the label goes through %d
                    -- to read "100°" instead of "100.0°".
                    c:text(2, y - 13, ("%d\u{00B0}"):format(v), {font = "xs", color = "text_muted"})
                end
                v = v + step
            end
        end

        c:polyline(pts, {color = "primary", width = 2})

        -- A dot marks the newest sample.
        c:circle(pts[#pts - 1], pts[#pts], detail and 3 or 2, {fill = "primary"})

        if detail and target_on() then
            -- Dashes 6 on, 4 off, like the built-in graph's target trace.
            local y = (h - 1) * (1 - (target_now - lo) / (hi - lo))
            local x = 0
            while x < w - 1 do
                c:line(x, y, math.min(x + 6, w - 1), y, {color = "text_muted"})
                x = x + 10
            end
            -- Labelled at the right end, clear of the degree labels on the
            -- left and of the newest dot; near the top it goes below the line.
            local ty = y >= 15 and y - 15 or y + 4
            c:text(w - 56, ty, "Target", {font = "xs", color = "text_muted"})
        end
    end
    c:commit()
end

local function render()
    heater_label:set(selected().label)

    draw(spark, false)
    draw(graph, true)

    local lo, hi, avg = stats()
    value_text:set(num(samples[#samples]))
    target_beside:set(target_on() and ("/ " .. num(target_now)) or "")
    min_text:set(fmt(lo))
    max_text:set(fmt(hi))
    avg_text:set(fmt(avg))
    target_text:set(fmt(target_now))
    local secs = N * helix.settings.get("interval_s")
    local mins = secs / 60
    -- Lua stringifies a whole float as "1.0", so minutes go through %d.
    if mins == math.floor(mins) then
        window_text:set(("last %d min"):format(mins))
    else
        window_text:set(("last %d s"):format(secs))
    end
    show_target:set(helix.settings.get("show_target") and 1 or 0)
end

local function backfill()
    -- Moonraker keeps its own temperature history; the last N readings of the
    -- selected heater seed the window so the tile is useful the moment it
    -- loads. Any failure leaves the window empty and live sampling fills it.
    local store, err =
        helix.moonraker.call("server.temperature_store", {include_monitors = false})
    if not store then
        helix.log.warn("temperature store unavailable: " .. tostring(err))
        return
    end
    local series = store[selected().store]
    samples = {}
    if series and series.temperatures then
        local temps = series.temperatures
        for i = math.max(1, #temps - N + 1), #temps do
            samples[#samples + 1] = temps[i]
        end
    end
    render()
end

-- Reading a heater's live values needs no permission: watch delivers changes,
-- get reads the current value. There is no unwatch, so all three heaters are
-- watched for the plugin's whole lifetime and each reading is ignored unless
-- it belongs to the selected heater.
for _, h in pairs(HEATERS) do
    local ok, e = pcall(helix.printer.watch, h.temp, function(v)
        if selected().temp == h.temp then
            latest = v
        end
    end)
    if not ok then
        helix.log.warn("cannot watch " .. h.temp .. ": " .. tostring(e))
    end
end

-- Sampling happens on the timer, not on every status change: the sparkline
-- shows one reading per interval, and the tile text follows the window.
local function tick()
    local h = selected()
    target_now = helix.printer.get(h.target)
    if latest ~= nil then
        samples[#samples + 1] = latest
        if #samples > N then
            table.remove(samples, 1)
        end
    end
    render()
end

local function arm_timer()
    if timer then
        timer:cancel()
    end
    timer = helix.timer.every(helix.settings.get("interval_s") * 1000, tick)
end

-- A heater switch is a fresh series: clear the window, adopt the new live
-- readings and pull a new backfill.
local function adopt_heater()
    local h = selected()
    samples = {}
    latest = helix.printer.get(h.temp)
    target_now = helix.printer.get(h.target)
    render()
    backfill()
end

helix.settings.on_change("heater", adopt_heater)
helix.settings.on_change("interval_s", arm_timer)
helix.settings.on_change("show_target", render)

-- A canvas reports its size once laid out and again on every resize; the
-- drawing is rebuilt for the new size.
spark:on_size(render)
graph:on_size(render)

-- The tile's only event opens the detail overlay. The XML addresses it as
-- plugin_event with user_data temp-spark__open; unload closes it.
helix.ui.on("open", function()
    helix.ui.overlay("temp-spark__detail")
end)

arm_timer()
adopt_heater()
