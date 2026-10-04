-- SPDX-License-Identifier: GPL-3.0-or-later
-- temp-spark: the reference HelixScreen plugin. One heater's temperature as a
-- home tile with a 30-sample sparkline, a detail overlay with min/max/target,
-- and three schema settings. Every HelixScreen API it uses is shown once.

local N = 30 -- samples in the sparkline window

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
local min_text = helix.subject.string("min_text", "--")
local max_text = helix.subject.string("max_text", "--")
local target_text = helix.subject.string("target_text", "--")
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

local function fmt(t)
    if not t then
        return "--"
    end
    return math.floor(t + 0.5) .. "\u{00B0}"
end

-- Points are percent of an absolute-temperature scale, so a reading keeps its
-- height while the window slides. The window fills from the right edge.
local function draw(c, scale, with_target)
    local w, h = c:size()
    if w > 1 and h > 1 and #samples >= 2 then
        local pts = {}
        for i, t in ipairs(samples) do
            pts[#pts + 1] = (N - #samples + i - 1) * (w - 1) / (N - 1)
            pts[#pts + 1] = (h - 1) * (1 - t / scale)
        end
        c:polyline(pts, {color = "primary", width = 2})
        if with_target and target_now and helix.settings.get("show_target") then
            local y = (h - 1) * (1 - target_now / scale)
            c:line(0, y, w - 1, y, {color = "text_muted"})
        end
    end
    c:commit()
end

local function render()
    heater_label:set(selected().label)

    -- The vertical scale is an absolute-temperature range: 10% headroom over
    -- the larger of the target and the window peak, floored at 50 degrees so
    -- an idle printer near ambient does not fill the tile with noise.
    local lo, hi = samples[1], samples[1]
    for _, t in ipairs(samples) do
        lo, hi = math.min(lo, t), math.max(hi, t)
    end
    local scale = math.max(50, hi or 0, target_now or 0) * 1.1

    draw(spark, scale)
    draw(graph, scale, true)

    value_text:set(fmt(samples[#samples]))
    min_text:set(fmt(lo))
    max_text:set(fmt(hi))
    target_text:set(fmt(target_now))
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
