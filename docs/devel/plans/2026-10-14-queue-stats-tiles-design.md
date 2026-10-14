# Job queue and print stats home tiles: redesign

Status: draft for Preston's review (2026-10-14).
Scope: the two home-panel tiles only. The job queue modal and the history dashboard stay as they are.

## What the user said

- Job queue: "glance + start", thought through for how people actually run printers, print farms especially.
- Print stats: lifetime totals and recent activity. No last-print outcome, because the print status tile already covers it. Filament usage only if it's actually wanted (research: wanted, but as a secondary figure).
- Look: one hero number with a supporting line, adding detail only as the tile grows.
- Maintenance counters ("nozzle: 212 h since change") are a separate follow-up, not part of this work. Preston: the maintenance-meter example plugin may be promoted to a widget that ships with the app.

## Research that shapes it

From the UX research done for this design (Bambu, Prusa Connect, SimplyPrint, 3DPrinterOS, Moonraker docs):
- Farm tools model a blocking state: the job finished and the plate isn't confirmed clear yet. A person releases it. Moonraker's `automatic_transition` defaults to false, so the queue already waits after each job, and resuming it is the "bed is clear, start next" action.
- Operators at the printer need to see what's next and press start. Reordering, duplicating and removing jobs happen remotely or in the full queue view.
- Wanted stats: print time per period, filament as a secondary figure, maintenance counters (deferred). Success rate, streaks and cost: no demand found.

## Job queue tile

**The decision is a pure function.** `job_queue_tile_state(inputs) -> JobQueueTileState`, with no LVGL, unit-tested. Inputs:
- queue job count
- Moonraker `queue_state` (ready, loading, starting, paused)
- printer print state (idle, printing, paused, complete, error)
- `automatic_transition`
- whether the job_queue component exists

Outputs:

| State | When | Tile shows | Primary action |
|---|---|---|---|
| Unavailable | Moonraker has no job_queue component | Widget is unavailable, with the reason "Job queue not enabled in Moonraker", like other gated widgets | none |
| Empty | 0 jobs | Quiet "Queue empty" | none |
| WaitingForStart | printer not printing, jobs queued, whatever `automatic_transition` and `queue_state` say | Next job's thumbnail and name, "+N more", estimated time if known | **"Bed clear · Start next"**: one tap starts the job with its saved options, no extra dialog |
| AutoNext | a print is running, jobs queued, auto transition on, queue ready | Hero "N queued" and "Next: name (starts automatically)" | none |
| Printing | a print is running, jobs queued, and the queue will not start the next job by itself | Hero "N queued" and "Next: name" | none |
| Starting | queue_state loading or starting | "Starting name…" | none |

Moonraker sets `queue_state` to "paused" after every job it loads unless automatic transition is on, and starts in "paused", so a post-job wait and a user pause cannot be told apart. Neither needs to be: an idle printer with jobs queued always offers the same start button. "Starts automatically" is only true while a print is running with automatic transition on and the queue ready, because Moonraker transitions only when a job completes.

**Start action:** one tap starts the next queued job through HelixScreen's own print-start path with the pre-print options saved for that job, the same gates and start call print-select's Print button uses for a queued job, with no screen in between. A job with no saved options starts with the file's defaults, as print-select would. On a confirmed start the job leaves Moonraker's queue and its saved options are deleted, with the same bookkeeping print-select uses. The "start a queued job with its saved options" logic is one shared, UI-free piece that print-select and the tile both call. The print status "Up next" row and the print-complete dialog's "Start next" keep their current behaviour (`src/ui/job_queue_start.cpp#handle_up_next_tap`).

**Tapping elsewhere** on the tile opens the existing queue modal.

**Sizes:**
- The minimum span drops from 4x4 to **2x1**.
- 2x1: hero count or the next job's name, with an icon-only start button in WaitingForStart.
- 2x2: thumbnail, name, "+N more" and the full-width button.
- 4x2: thumbnail on the left, then name, estimated time, "+N more" and the button.
- 4x4 and up: also lists the following 2 or 3 jobs, using the existing `ui_xml/components/job_queue_row.xml` in read-only form.

**Data:**
- Thumbnail and estimated time for the next job come from the file metadata the print-select and print-status code already fetch. Reuse that cache and don't add a second fetch path.
- Refresh stays event-driven: the `notify_job_queue_changed` notification, activation, and reconnect.

**Clean-up that comes with it:**
- Rows and empty states become XML: subjects, `<repeat>` or the row component, and bind_flag. No `lv_obj_create`, `lv_label_set_text` or `lv_obj_add_flag` in the widget.
- Every string is translated ("N jobs queued" and so on).
- The current "Ready / Queue empty" shown while loading or when the component is missing goes away.

## Print stats tile

**Hero:** hours printed in the **last 7 days**, e.g. "23 h" with "this week" under it.
- It sums `print_duration` rather than `total_duration`, so pauses don't count.
- The source is PrintHistoryManager's cache, which already covers 7 days through `ensure_covers_since`.

**Supporting line:** lifetime prints and lifetime hours from the server totals (`server.history.totals`). From 4x2 up it adds lifetime filament from the totals' `total_filament_used`, shown as a length in meters (e.g. "412 m"), with no conversion to mass.

**7-day bars:** from 2x2 up, a seven-bar sparkline of hours per day, with today on the right and a quiet baseline.
- It's drawn by an existing chart or bar primitive if one fits; otherwise it's a canvas draw hook, which is a structural exception in the declarative rules.
- No axes and no labels. At 4x4 and up, each bar gets a weekday initial under it.

**Removed:**
- Success rate (the scope mismatch, and nobody asks for it)
- The "Last: name OK/X/!" line (print status covers it)
- The hidden lifetime/weekly tap toggle

**Tap:** opens the history dashboard panel (`ui_panel_history_dashboard.cpp`).

**Sizes:**
- The minimum span goes to **2x1**.
- 2x1: hero and "this week".
- 2x2: hero, the bars and "N prints".
- 4x2: the hero and bars side by side, plus the full supporting line.
- 4x4: bigger bars with weekday initials.

**States:**
- "No prints this week" (hero "0 h") still shows the lifetime line.
- With no history at all: "No prints yet".
- If the totals call fails: the week figures still show, and the lifetime line is hidden rather than undercounted from the 50-job cache.

**Clean-up:** one XML tree driven by size-mode subjects, replacing the four duplicated trees. Translated strings, design tokens, no raw pixel values, and no imperative pressed opacity.

## Shared look

- **Hero:** the tier's heading font, primary text colour.
- **Label:** small, muted.
- **Supporting line:** body small, muted.
- **Colour:** at most one accent colour per tile (the start button, the bars).
- **Spacing:** card padding as on other tiles.

Both tiles stay out of the default layouts (the no-fillers ruling). The redesign is for users who add them.

## Testing

- Unit tests for `job_queue_tile_state` cover every row of the table, plus the edge cases: 1 job, a job being added while printing, and an idle printer with automatic transition on (paused or ready) showing the start button.
- Unit tests for the weekly-hours, per-day bucket and filament formatting functions. They're pure functions over job lists, with day boundaries in local time.
- Widget tests for each state (subjects, visibility, button presence) at 2x1, 2x2, 4x2 and 4x4.
- Content-fits sweep rows for both tiles at every shipping geometry, including 1920x1080.
- Renders of each state at 800x480 and 480x272 for Preston's eye.
- Mutation gate once at the end.

## Out of scope

- Maintenance counters: a separate follow-up that starts from the existing example plugin `examples/plugins/maintenance-meter/`. It already has hour meters that accrue from print time and survive restarts, a ring gauge, a due toast and reset-with-confirm, on a 1x1 to 4x2 adaptive tile. The follow-up decides how it ships: as a bundled Lua plugin, enabled through the catalog, or ported to a native widget.
- Queue modal changes.
- History dashboard changes.
- Reordering or adding to the queue from the tile.
