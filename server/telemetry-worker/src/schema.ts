// SPDX-License-Identifier: GPL-3.0-or-later
// One declaration per Analytics Engine point kind (the `index1` value). The
// writer in analytics.ts fills blob and double arrays from these tables, and
// queries ask for columns through col(kind, field) instead of literal blobN.
//
// Slots are positions in stored rows, and rows are read by position for 90
// days. A column never moves; new fields take the next free slot (blobs 13-20,
// doubles 9-20). A retired column leaves its slot empty.

export type FieldType = "blob" | "double";

export type EventRecord = Record<string, unknown>;

/**
 * Pulls one column's raw value out of an event. `item` is the element a
 * fan-out kind iterates over (a panel, a widget, one settings change).
 */
export type Reader = (event: EventRecord, item?: EventRecord) => unknown;

export interface ColumnDef {
  /** Stable logical name used by col(); the client's JSON key where one exists. */
  field: string;
  type: FieldType;
  /** 1-based position in the blobs or doubles array. */
  slot: number;
  /** Accessor for columns that are not a plain `event[field]`. */
  read?: Reader;
}

export interface PointKind {
  /** The `index1` value. */
  kind: string;
  columns: ColumnDef[];
}

export const MAX_SLOT = 20;

// ---------- Readers ----------

const section = (v: unknown): EventRecord => (v ?? {}) as EventRecord;

/** `event[sec][key]`, then each flat key in order; null and undefined fall through. */
const nested =
  (sec: string, key: string, ...flatKeys: string[]): Reader =>
  (e) => {
    const v = section(e[sec])[key];
    if (v !== undefined && v !== null) return v;
    for (const k of flatKeys) {
      if (e[k] !== undefined && e[k] !== null) return e[k];
    }
    return undefined;
  };

const firstOf =
  (...readers: Reader[]): Reader =>
  (e, item) => {
    for (const r of readers) {
      const v = r(e, item);
      if (v !== undefined && v !== null) return v;
    }
    return undefined;
  };

const flat =
  (key: string): Reader =>
  (e) =>
    e[key];

const fromItem =
  (key: string): Reader =>
  (_e, item) =>
    item?.[key];

const appVersion = nested("app", "version", "app_version", "version");
const appPlatform = nested("app", "platform", "app_platform", "platform");
const appBoard = nested("app", "board", "app_board", "board");

// Update events carry two versions, so neither reads the envelope's app_version.
const targetVersion = flat("version");
const fromVersion = flat("from_version");

const themeName: Reader = (e) =>
  firstOf(flat("theme_name"), nested("app", "theme", "theme"))(e) ?? "";

const darkLightTheme: Reader = (e) => {
  if (e.theme !== undefined && e.theme !== null) return e.theme;
  const name = String(themeName(e));
  return name.includes("(Dark)") ? "dark" : name.includes("(Light)") ? "light" : "";
};

/** "1" / "0" / "": a boolean that is "" for clients too old to report it. */
const triState =
  (read: Reader): Reader =>
  (e, item) => {
    const v = read(e, item);
    return v === undefined || v === null ? "" : v ? "1" : "0";
  };

const hardwareSensorCount: Reader = (e) => {
  const sensors = section(e.sensors);
  const sum =
    Number(sensors.filament ?? 0) +
    Number(sensors.temperature_extra ?? 0) +
    Number(sensors.color ?? 0) +
    Number(sensors.accel ?? 0);
  return sum || Number(sensors.count ?? e.sensor_count ?? 0);
};

// Bit order: 0=chamber, 1=accelerometer, 2=firmware_retraction,
// 3=exclude_object, 4=timelapse, 5=klippain_shaketune, 6=speaker, 7=probe,
// 8=led, 9=filament_sensor, 10=multi_extruder, 11=ams, 12=heater_bed.
const CAPABILITY_BITS = [
  "has_chamber",
  "has_accelerometer",
  "has_firmware_retraction",
  "has_exclude_object",
  "has_timelapse",
  "has_klippain_shaketune",
  "has_speaker",
];

const hardwareCapabilitiesBitmask: Reader = (e) => {
  const caps = section(e.capabilities);
  let mask = Number(e.capabilities_bitmask ?? 0);
  if (typeof caps === "object" && caps !== null && Object.keys(caps).length > 0) {
    const probe = section(e.probe);
    const leds = section(e.leds);
    const sensors = section(e.sensors);
    const tools = section(e.tools);
    const extruders = section(e.extruders);
    mask = 0;
    for (let i = 0; i < CAPABILITY_BITS.length; i++) {
      if (caps[CAPABILITY_BITS[i]]) mask |= 1 << i;
    }
    if (probe.has_probe) mask |= 1 << 7;
    if (Number(leds.count ?? 0) > 0) mask |= 1 << 8;
    if (Number(sensors.filament ?? 0) > 0) mask |= 1 << 9;
    if (tools.is_multi_tool || Number(extruders.count ?? 0) > 1) mask |= 1 << 10;
    if (e.ams) mask |= 1 << 11;
    if (extruders.has_heater_bed) mask |= 1 << 12;
  }
  return mask;
};

// The first seven features get their own double; the rest share a bitmask.
const FEATURE_DOUBLES = [
  "macros",
  "camera",
  "bed_mesh",
  "console_gcode",
  "input_shaper",
  "filament_management",
  "manual_probe",
];

const FEATURE_BITMASK_ORDER = [
  "spoolman",
  "led_control",
  "power_devices",
  "multi_printer",
  "theme_changed",
  "timelapse",
  "favorites",
  "pid_calibration",
  "firmware_retraction",
];

const featureFlag =
  (name: string): Reader =>
  (e) =>
    section(e.features)[name] ? 1 : 0;

const featureBitmask: Reader = (e) => {
  const features = section(e.features);
  let mask = 0;
  for (let i = 0; i < FEATURE_BITMASK_ORDER.length; i++) {
    if (features[FEATURE_BITMASK_ORDER[i]]) mask |= 1 << i;
  }
  return mask;
};

const crashBacktraceDepth: Reader = (e) =>
  e.backtrace_depth ?? (Array.isArray(e.backtrace) ? e.backtrace.length : 0);

// ---------- Column constructors ----------

const blob = (slot: number, field: string, read?: Reader): ColumnDef =>
  read ? { field, type: "blob", slot, read } : { field, type: "blob", slot };

const dbl = (slot: number, field: string, read?: Reader): ColumnDef =>
  read ? { field, type: "double", slot, read } : { field, type: "double", slot };

const device = blob(1, "device_id");
const versionPlatform = [
  blob(2, "app_version", appVersion),
  blob(3, "app_platform", appPlatform),
];

/** Idempotency key parts for delta point kinds: (device_id, boot_id, snapshot_seq). */
const deltaColumns = [
  blob(13, "boot_id"),
  dbl(9, "schema_version"),
  dbl(10, "snapshot_seq"),
];

const kind = (name: string, columns: ColumnDef[]): PointKind => ({ kind: name, columns });

// ---------- The table ----------

const KINDS: PointKind[] = [
  kind("session", [
    device,
    blob(2, "app_version", appVersion),
    blob(3, "app_platform", appPlatform),
    blob(4, "printer_model", nested("printer", "detected_model", "printer_model")),
    blob(5, "kinematics", nested("printer", "kinematics", "kinematics")),
    blob(6, "display", nested("app", "display", "display")),
    blob(7, "locale", nested("app", "locale", "locale")),
    blob(8, "theme", nested("app", "theme", "theme")),
    blob(9, "arch", nested("host", "arch", "arch")),
    blob(10, "zip_tool", nested("app", "zip_tool", "zip_tool")),
    // Resolved printer board; splits the unified "mips" platform key.
    blob(11, "app_board", nested("app", "board", "board")),
    dbl(1, "ram_total_mb", nested("host", "ram_total_mb", "ram_total_mb")),
    dbl(2, "cpu_cores", nested("host", "cpu_cores", "cpu_cores")),
    dbl(3, "extruder_count", nested("printer", "extruder_count", "extruder_count")),
  ]),

  kind("print_outcome", [
    device,
    blob(2, "outcome"),
    blob(3, "filament_type"),
    blob(4, "app_version", appVersion),
    blob(13, "app_platform", appPlatform),
    blob(14, "app_board", appBoard),
    dbl(1, "duration_sec"),
    dbl(2, "nozzle_temp"),
    dbl(3, "bed_temp"),
    dbl(4, "filament_used_mm"),
    dbl(5, "phases_completed"),
  ]),

  kind("crash", [
    device,
    blob(2, "app_version", appVersion),
    blob(3, "signal_name"),
    blob(4, "app_platform", appPlatform),
    blob(5, "load_base"),
    blob(6, "fault_code_name"),
    blob(7, "fault_addr"),
    blob(8, "app_board", appBoard),
    dbl(1, "uptime_sec"),
    dbl(2, "signal", firstOf(flat("signal"), flat("signal_number"))),
    dbl(3, "backtrace_depth", crashBacktraceDepth),
  ]),

  // blob2 is the version the device was RUNNING (the default version filter
  // reads blob2 for every kind); the release it tried to move to is blob5.
  kind("update_failed", [
    device,
    blob(2, "from_version", fromVersion),
    blob(3, "app_platform", appPlatform),
    blob(4, "reason"),
    blob(5, "version", targetVersion),
    dbl(1, "http_code"),
    dbl(2, "file_size"),
    dbl(3, "exit_code"),
  ]),

  // blob2 is the release the device is running now; blob4 is where it came from.
  kind("update_success", [
    device,
    blob(2, "version", targetVersion),
    blob(3, "app_platform", appPlatform),
    blob(4, "from_version", fromVersion),
  ]),

  kind("memory_snapshot", [
    device,
    ...versionPlatform,
    blob(4, "trigger"),
    dbl(1, "uptime_sec"),
    dbl(2, "rss_kb"),
    dbl(3, "vm_size_kb"),
    dbl(4, "vm_peak_kb"),
    dbl(5, "vm_hwm_kb"),
  ]),

  kind("memory_warning", [
    device,
    ...versionPlatform,
    blob(4, "level"),
    blob(5, "reason"),
    dbl(1, "uptime_sec"),
    dbl(2, "rss_kb"),
    dbl(3, "vm_size_kb"),
    dbl(4, "system_available_mb"),
    dbl(5, "growth_5min_kb"),
    dbl(6, "private_dirty_kb"),
    dbl(7, "pss_kb"),
    dbl(8, "system_total_mb"),
  ]),

  kind("hardware_profile", [
    device,
    ...versionPlatform,
    blob(4, "printer_model", nested("printer", "detected_model", "printer_model")),
    blob(5, "kinematics", nested("printer", "kinematics", "kinematics")),
    blob(6, "mcu_chip", nested("mcus", "primary", "mcu_chip")),
    blob(7, "probe_type", nested("probe", "type", "probe_type")),
    blob(8, "ams_type", nested("ams", "type")),
    // Tri-state strings: a bitmask bit cannot tell false from "client too old".
    blob(9, "has_helix_macros", triState(nested("macros", "has_helix_macros"))),
    blob(10, "moonraker_is_local", triState(flat("moonraker_is_local"))),
    blob(13, "app_board", appBoard),
    dbl(
      1,
      "extruder_count",
      firstOf(
        nested("extruders", "count"),
        nested("printer", "extruder_count", "extruder_count"),
      ),
    ),
    dbl(2, "fan_count", firstOf(nested("fans", "total"), nested("fans", "count", "fan_count"))),
    dbl(3, "sensor_count", hardwareSensorCount),
    dbl(4, "macro_count", firstOf(nested("macros", "total_count"), nested("macros", "count", "macro_count"))),
    dbl(5, "build_vol_x", nested("build_volume", "x_mm", "build_vol_x")),
    dbl(6, "build_vol_y", nested("build_volume", "y_mm", "build_vol_y")),
    dbl(7, "build_vol_z", nested("build_volume", "z_mm", "build_vol_z")),
    dbl(8, "capabilities_bitmask", hardwareCapabilitiesBitmask),
  ]),

  kind("settings_snapshot", [
    device,
    ...versionPlatform,
    blob(4, "theme_name", themeName),
    blob(5, "locale", nested("app", "locale", "locale")),
    blob(6, "auto_update_channel"),
    blob(7, "time_format"),
    blob(8, "theme", darkLightTheme),
    dbl(1, "brightness_pct"),
    dbl(2, "screensaver_timeout_sec"),
    dbl(3, "screen_blank_timeout_sec"),
    dbl(4, "sound_enabled"),
    dbl(5, "animations_enabled"),
  ]),

  // One point per distinct home widget id; `count` is how many times it is placed.
  kind("widget_placement", [
    device,
    ...versionPlatform,
    blob(4, "widget_id", fromItem("widget_id")),
    dbl(1, "count", fromItem("count")),
  ]),

  // One point per panel in the event's panel maps.
  kind("panel_usage", [
    device,
    ...versionPlatform,
    blob(4, "panel_name", fromItem("panel_name")),
    ...deltaColumns,
    dbl(1, "session_duration_sec"),
    dbl(2, "time_sec", fromItem("time_sec")),
    dbl(3, "visits", fromItem("visits")),
    dbl(4, "overlay_open_count"),
  ]),

  kind("widget_interaction", [
    device,
    ...versionPlatform,
    blob(4, "widget_id", fromItem("widget_id")),
    ...deltaColumns,
    dbl(1, "session_duration_sec"),
    dbl(2, "count", fromItem("count")),
  ]),

  kind("overlay_visit", [
    device,
    ...versionPlatform,
    blob(4, "overlay_name", fromItem("overlay_name")),
    ...deltaColumns,
    dbl(1, "session_duration_sec"),
    dbl(2, "count", fromItem("count")),
  ]),

  kind("connection_stability", [
    device,
    ...versionPlatform,
    blob(13, "boot_id"),
    blob(14, "app_board", appBoard),
    dbl(1, "session_duration_sec"),
    dbl(2, "connect_count"),
    dbl(3, "disconnect_count"),
    dbl(4, "total_connected_sec"),
    dbl(5, "total_disconnected_sec"),
    dbl(6, "longest_disconnect_sec"),
    dbl(7, "klippy_error_count"),
    dbl(8, "klippy_shutdown_count"),
    dbl(9, "schema_version"),
    dbl(10, "snapshot_seq"),
  ]),

  kind("print_start_context", [
    device,
    ...versionPlatform,
    blob(4, "slicer"),
    blob(5, "file_size_bucket"),
    blob(6, "estimated_duration_bucket"),
    blob(7, "source"),
    blob(13, "app_board", appBoard),
    dbl(1, "tool_count_used"),
    dbl(2, "has_thumbnail"),
    dbl(3, "ams_active"),
  ]),

  kind("error_encountered", [
    device,
    ...versionPlatform,
    blob(4, "category"),
    blob(5, "code"),
    blob(6, "context"),
    blob(13, "app_board", appBoard),
    dbl(1, "uptime_sec"),
  ]),

  kind("performance_snapshot", [
    device,
    ...versionPlatform,
    blob(4, "worst_panel"),
    dbl(1, "snapshot_seq"),
    dbl(2, "frame_time_p50_ms"),
    dbl(3, "frame_time_p95_ms"),
    dbl(4, "frame_time_p99_ms"),
    dbl(5, "dropped_frame_count"),
    dbl(6, "total_frame_count"),
    dbl(7, "worst_panel_p95_ms"),
    dbl(8, "task_handler_max_ms"),
  ]),

  kind("feature_adoption", [
    device,
    ...versionPlatform,
    ...FEATURE_DOUBLES.map((name, i) => dbl(i + 1, name, featureFlag(name))),
    dbl(8, "feature_bitmask", featureBitmask),
  ]),

  // One point per changed setting; `change_count` is 1 per change and 0 for an
  // event that lists none.
  kind("settings_change", [
    device,
    ...versionPlatform,
    blob(4, "setting", fromItem("setting")),
    blob(5, "old_value", fromItem("old_value")),
    blob(6, "new_value", fromItem("new_value")),
    dbl(1, "change_count", (_e, item) => (item ? 1 : 0)),
  ]),

  // The per-tag breakdown stays in R2 (see R2_ONLY); these are the window totals.
  kind("async_lifetime_skips", [
    device,
    ...versionPlatform,
    dbl(1, "uptime_sec"),
    dbl(2, "total_skips"),
    dbl(3, "other_count"),
    dbl(4, "is_shutdown"),
  ]),
];

export const POINT_KINDS: Record<string, PointKind> = Object.fromEntries(
  KINDS.map((k) => [k.kind, k]),
);

/** Event type -> the point kinds it writes, in write order. */
export const EVENT_FANOUT: Record<string, string[]> = {
  session: ["session"],
  print_outcome: ["print_outcome"],
  crash: ["crash"],
  update_failed: ["update_failed"],
  update_success: ["update_success"],
  memory_snapshot: ["memory_snapshot"],
  memory_warning: ["memory_warning"],
  hardware_profile: ["hardware_profile"],
  settings_snapshot: ["settings_snapshot", "widget_placement"],
  panel_usage: ["panel_usage", "widget_interaction", "overlay_visit"],
  connection_stability: ["connection_stability"],
  print_start_context: ["print_start_context"],
  error_encountered: ["error_encountered"],
  performance_snapshot: ["performance_snapshot"],
  feature_adoption: ["feature_adoption"],
  settings_changes: ["settings_change"],
  async_lifetime_skips: ["async_lifetime_skips"],
};

/** Event type -> dotted field paths that are deliberately kept out of Analytics Engine. */
export const R2_ONLY: Record<string, string[]> = {
  async_lifetime_skips: ["tags"],
};

// ---------- Lookup ----------

function hasKind(kindName: string): boolean {
  return Object.prototype.hasOwnProperty.call(POINT_KINDS, kindName);
}

function findColumn(kindName: string, field: string): ColumnDef | undefined {
  return hasKind(kindName) ? POINT_KINDS[kindName].columns.find((c) => c.field === field) : undefined;
}

/** The Analytics Engine column name for a field: "blob7" / "double3". */
export function col(kindName: string, field: string): string {
  if (!hasKind(kindName)) throw new Error(`unknown point kind: ${kindName}`);
  const c = findColumn(kindName, field);
  if (!c) throw new Error(`point kind ${kindName} has no column ${field}`);
  return `${c.type}${c.slot}`;
}

export function hasCol(kindName: string, field: string): boolean {
  return findColumn(kindName, field) !== undefined;
}
