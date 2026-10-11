-- Calibration Bridge: run slicer-side filament calibrations at the printer.
-- A companion (an OrcaSlicer plugin) posts wizard context through Moonraker
-- agent events; the screen starts the pre-sliced print, asks which result is
-- best, and posts the answer back. Overlay-only: no home tile, the companion
-- event is the front door and the settings action row is the manual one.
--
-- Contract (the README is the spec):
--   event helixcal.available  {wizard, filament, printer, nozzle_mm, current,
--                              choices (numbers), file (relative to the
--                              printer's gcodes root, .gcode)}
--   event helixcal.cancel     {}   resets to idle; ignored while a print runs
--   POST companion_url        {event="chosen", wizard, value, filament, current}

local t = helix.i18n.t

-- States: 0 idle, 1 available, 2 printing, 3 asking, 4 done.
local state = helix.subject.int("state", 0)
local start_on = helix.subject.int("start_on", 0)
local printing_on = helix.subject.int("printing_on", 0)
local pick_on = helix.subject.int("pick_on", 0)
local result_on = helix.subject.int("result_on", 0)
-- LVGL dropdown selection, 0-based; the first option is pre-selected.
local choice_idx = helix.subject.int("choice_idx", 0)
local state_title = helix.subject.string("state_title", t("No calibration running"))
local ctx_printer = helix.subject.string("ctx_printer", "")
local ctx_filament = helix.subject.string("ctx_filament", "")
local ctx_file = helix.subject.string("ctx_file", "")
local ctx_current = helix.subject.string("ctx_current", "")
local result_value = helix.subject.string("result_value", "")
local result_sub = helix.subject.string("result_sub", "")
local status_line = helix.subject.string("status_line", "")

-- The English source of whatever the status line and result note show, so a
-- language switch can say them again in the new language. nil = raw text or
-- nothing standing. A status may carry a reason after its key ("print failed:
-- <reason>"); the reason is the backend's own text and is not translated.
local status_key, status_detail, result_key

local function status_text()
    if not status_key then
        return ""
    end
    return status_detail and (t(status_key) .. ": " .. status_detail) or t(status_key)
end

local function set_status(key, detail)
    status_key = key
    status_detail = detail and tostring(detail) or nil
    status_line:set(status_text())
end

local function set_result_note(key, raw)
    result_key = key
    result_sub:set(key and t(key) or raw)
end

-- Whole-number wizards carry a fixed format; the others pick the fewest
-- decimals that keep every choice's label distinct and exact (see pick_format).
local WIZARDS = {
    temp_tower = { name = "Temperature tower", fmt = "%d\u{00B0}C", integral = true },
    pa_pattern = { name = "PA pattern" },
    flow_rate = { name = "Flow rate", fmt = "%d%%", integral = true },
}

-- Numbers outside this range are not calibration values, and a whole-number
-- format cannot print them.
local LIMIT = 1e6
local MAX_CHOICES = 64

local function usable(n)
    return n == n and n > -LIMIT and n < LIMIT
end

-- The format that renders every value as a distinct label that reads back as
-- the value it stands for, or nil when none does.
local function pick_format(w, values)
    local candidates = { w.fmt }
    if not w.integral then
        candidates = {}
        for d = 2, 6 do
            candidates[#candidates + 1] = ("%%.%df"):format(d)
        end
    end
    for _, fmt in ipairs(candidates) do
        local seen, ok = {}, true
        for _, v in ipairs(values) do
            local label = fmt:format(v)
            if seen[label] or (not w.integral and math.abs(tonumber(label) - v) > 1e-9) then
                ok = false
                break
            end
            seen[label] = true
        end
        if ok then
            return fmt
        end
    end
    return nil
end

-- A path under the printer's gcodes root: no escapes, no control characters.
local function valid_file(f)
    if #f > 255 or f:find("%c") or f:find("\\", 1, true) or f:find("..", 1, true) then
        return false
    end
    return f:match("^[^/].*%.[gG][cC][oO][dD][eE]$") ~= nil
end

-- The calibration in flight. `choices` holds numbers; the dropdown labels and
-- the posted value come from one formatter, so what the user read is what the
-- companion receives.
local cal = nil
local overlay_handle = nil
local shown_opts = nil -- the dropdown options the showing overlay was opened with
local reopen_pending = false
local start_inflight = false
local cancel_inflight = false

local function set_state(v)
    state:set(v)
    start_on:set(v == 1 and 1 or 0)
    printing_on:set(v == 2 and 1 or 0)
    pick_on:set(v == 3 and 1 or 0)
    result_on:set(v == 4 and 1 or 0)
end

-- The context lines, rebuilt rather than patched so a language switch can
-- re-say them from the same data.
local function refresh_ctx()
    if not cal then
        state_title:set(t("No calibration running"))
        ctx_printer:set("")
        ctx_filament:set("")
        ctx_file:set("")
        ctx_current:set("")
        return
    end
    local w = WIZARDS[cal.wizard]
    state_title:set(t(w.name))
    local nozzle = cal.nozzle_mm and (" \u{00B7} " .. t("nozzle") .. " " .. tostring(cal.nozzle_mm)) or ""
    ctx_printer:set(cal.printer .. nozzle)
    ctx_filament:set(cal.filament)
    ctx_file:set(cal.file)
    ctx_current:set(t("Profile") .. ": " .. cal.fmt:format(cal.current))
end

local show_ask

-- The dropdown's newline-separated labels, from the same formatter that
-- renders the posted value.
local function ask_options()
    local labels = {}
    for i, v in ipairs(cal.choices) do
        labels[i] = cal.fmt:format(v)
    end
    return table.concat(labels, "\n")
end

-- The overlay's dropdown options are fixed when it opens, so new choices need
-- a new overlay. Closing is asynchronous: the old one is gone once its
-- on_close runs, and only then can the new one open.
local function on_ask_closed()
    overlay_handle = nil
    shown_opts = nil
    if reopen_pending then
        reopen_pending = false
        if cal then
            show_ask()
        end
    end
end

function show_ask()
    shown_opts = ask_options()
    overlay_handle = helix.ui.overlay("calibration-bridge__ask", {
        opts = shown_opts,
        on_close = on_ask_closed,
    })
end

local function open_ask()
    if not overlay_handle then
        show_ask()
        return
    end
    if ask_options() ~= shown_opts and not reopen_pending then
        reopen_pending = true
        overlay_handle.close()
    end
end

local function cancel_cal()
    cal = nil
    reopen_pending = false
    set_state(0)
    refresh_ctx()
    choice_idx:set(0)
    set_status(nil)
    set_result_note(nil, "")
end

local function on_available(data)
    if type(data) ~= "table" then
        return
    end
    -- A calibration in flight owns the screen until it ends.
    if start_inflight or state:get() == 2 or state:get() == 3 then
        helix.log.warn("helixcal.available ignored: a calibration is running")
        return
    end
    local w = WIZARDS[data.wizard]
    if not w or type(data.choices) ~= "table" then
        set_status("bad event")
        return
    end
    -- Validate everything before mutating anything: a refused event must not
    -- leave a half-applied calibration behind.
    local choices = {}
    for i, v in ipairs(data.choices) do
        local n = tonumber(v)
        if not n or not usable(n) or (w.integral and n ~= math.floor(n)) then
            set_status("bad event")
            return
        end
        choices[i] = n
    end
    if #choices < 2 or #choices > MAX_CHOICES then
        set_status("bad event")
        return
    end
    local fmt = pick_format(w, choices)
    local current = tonumber(data.current)
    if not fmt or not current or not usable(current) or (w.integral and current ~= math.floor(current)) then
        set_status("bad event")
        return
    end
    for _, field in ipairs({ "filament", "printer", "file" }) do
        local v = data[field]
        if v ~= nil and type(v) ~= "string" and type(v) ~= "number" then
            set_status("bad event")
            return
        end
    end
    local file = tostring(data.file or "")
    if file ~= "" and not valid_file(file) then
        set_status("bad event")
        return
    end
    local nozzle = tonumber(data.nozzle_mm)
    cal = {
        wizard = data.wizard,
        filament = tostring(data.filament or ""),
        printer = tostring(data.printer or ""),
        nozzle_mm = nozzle and usable(nozzle) and nozzle or nil,
        current = current,
        choices = choices,
        fmt = fmt,
        file = file,
    }
    choice_idx:set(0)
    set_status(nil)
    set_state(1)
    refresh_ctx()
    if helix.settings.get("auto_open") ~= false then
        open_ask()
    end
end

local function post_result(value)
    local sent = cal
    local url = helix.settings.get("companion_url") or ""
    if not url:match("^https?://") then
        set_result_note("(no companion URL set)", "")
        return
    end
    local body = helix.json.encode({
        event = "chosen",
        wizard = cal.wizard,
        value = value,
        filament = cal.filament,
        current = cal.current,
    })
    local r, err = helix.http.post(url, {
        body = body,
        headers = { ["Content-Type"] = "application/json" },
    })
    if cal ~= sent then
        return -- the calibration ended while the POST was in flight
    end
    if r and r.status >= 200 and r.status < 300 then
        set_result_note("sent to the companion", "")
    elseif r then
        set_result_note(nil, ("HTTP %d"):format(r.status))
    else
        set_result_note(nil, err or "")
    end
end

-- Printing ends: complete hands over to the picker; a pause is not an end;
-- anything else returns to available with the reason on the status line.
local prev_printing = false
local function on_print_state(v)
    if not cal or state:get() ~= 2 then
        prev_printing = v == "printing"
        return
    end
    if v == "printing" then
        prev_printing = true
        return
    end
    if v == "paused" or not prev_printing then
        return
    end
    prev_printing = false
    if v == "complete" then
        set_state(3)
        if helix.settings.get("auto_ask") ~= false then
            open_ask()
        end
    else
        set_state(1)
        set_status(v == "cancelled" and "print cancelled" or "print failed")
    end
end

helix.printer.watch("print_state", on_print_state)

helix.moonraker.on_agent_event("helixcal.available", function(_, data)
    on_available(data)
end)

helix.moonraker.on_agent_event("helixcal.cancel", function()
    -- The event never abandons a print: stopping one is the on-screen
    -- Cancel button's job, behind a confirmation.
    if start_inflight or state:get() == 2 then
        helix.log.warn("helixcal.cancel ignored: the calibration print is running")
        return
    end
    cancel_cal()
end)

helix.ui.on("open", function()
    if cal then
        open_ask()
    else
        helix.ui.toast(t("No calibration running"))
    end
end)

helix.ui.on("start", function()
    if not cal or state:get() ~= 1 or start_inflight then
        return
    end
    if cal.file == "" then
        set_status("no print file in the event")
        return
    end
    -- Events that would replace or end `cal` are ignored while the start is
    -- in flight, so it is still the calibration that was started when the
    -- reply lands.
    start_inflight = true
    local ok, err = helix.moonraker.call("printer.print.start", { filename = cal.file })
    start_inflight = false
    if not ok then
        set_status("print failed", err)
        return
    end
    set_status(nil)
    set_state(2)
end)

helix.ui.on("accept", function()
    if not cal or state:get() ~= 3 then
        return
    end
    local i = choice_idx:get()
    if i < 0 or i >= #cal.choices then
        return -- out of range: a list shrank between pick and accept
    end
    local value = cal.choices[i + 1]
    result_value:set(cal.fmt:format(value))
    set_state(4)
    post_result(value)
end)

-- Stops the print the calibration started; the calibration resets only once
-- Moonraker has accepted the cancel, so a failed cancel leaves a print the
-- screen still tracks.
local function stop_print_and_reset()
    if cancel_inflight then
        return
    end
    cancel_inflight = true
    local ok, err = helix.moonraker.call("printer.print.cancel", {})
    cancel_inflight = false
    if not ok then
        set_status("could not cancel the print", err)
        helix.ui.toast(t("could not cancel the print"), "error")
        return
    end
    cancel_cal()
end

-- Cancel on an idle, available or asking calibration just resets it; while its
-- print runs, it asks first and then stops the print as well.
helix.ui.on("cancel", function()
    if start_inflight then
        return
    end
    if cal and state:get() == 2 then
        helix.ui.confirm(t("Stop the calibration print?"), t("This cancels the running print."), {
            severity = "warning",
            confirm_text = t("Stop print"),
            on_confirm = stop_print_and_reset,
        })
        return
    end
    cancel_cal()
end)

-- Subject-carried strings re-say themselves on a language switch.
helix.i18n.on_change(function()
    refresh_ctx()
    status_line:set(status_text())
    if result_key then
        result_sub:set(t(result_key))
    end
end)
