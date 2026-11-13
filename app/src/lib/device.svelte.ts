// The window's copy of the device: filled from the Rust side's snapshot and kept current by
// its events. The device owns the state; the UI shows what it reports.
//
// In a plain browser (npm run dev, without Tauri) there is no device: the window then shows a
// saved copy of the unit's state with moving meters, for working on the UI.
import type { DeviceEvent, Meters, Snapshot, Transfer } from "./types";

const empty: Snapshot = {
  connected: false,
  detail: "Looking for the unit…",
  info: null,
  settings: [],
  values: [],
  files: { model: "", ir: "", preset: "", edited: false, card: false },
  presets: [],
  models: [],
  irs: [],
};

const noMeters: Meters = {
  inputPeak: 0, outputPeak: 0, gateGain: 1, clipped: 0,
  cpu: 0, cpuMax: 0, cpuAmp: 0, cpuDoubler: 0, cpuDelay: 0, cpuReverb: 0, cpuDrums: 0,
  overruns: 0, overloadMutes: 0,
  ampRunning: false, delayRinging: false, reverbRinging: false, drumsPlaying: false, muted: false,
};

export const device = $state<Snapshot>({ ...empty });
export const meters = $state<Meters>({ ...noMeters });
/** True when the window runs without Tauri and shows saved data. */
export const preview = $state({ on: false });
/** The last thing the unit refused ("No such preset"), for the screen to show briefly. */
export const notice = $state({ text: "", at: 0 });
/** The file being sent to the unit, or the last one sent (`at`: when it last changed). */
export const transfer = $state<Transfer & { at: number }>({ name: "", sent: 0, total: 0, done: true, error: "", at: 0 });
/** True while files are dragged over the window. */
export const dropping = $state({ on: false });

function say(text: string) {
  notice.text = text;
  notice.at = performance.now();
}

const inTauri = typeof window !== "undefined" && "__TAURI_INTERNALS__" in window;

// A control that is being moved shows the user's value, not the device's slightly older
// answers: for this long after the last local change, reports for that setting are ignored.
// The periodic full state corrects anything that was missed (a value the device clamped).
const HOLD_MS = 400;
const touched = new Map<number, number>();
const held = (id: number) => performance.now() - (touched.get(id) ?? -1e9) < HOLD_MS;

function apply(e: DeviceEvent) {
  switch (e.kind) {
    case "searching":
      device.connected = false;
      device.detail = e.detail;
      break;
    case "connected":
      device.connected = true;
      device.detail = "";
      device.info = e.info;
      break;
    case "schema":
      device.settings = e.settings;
      break;
    case "state":
      device.values = e.values.map((v, id) => (held(id) ? (device.values[id] ?? v) : v));
      device.files = e.files;
      break;
    case "param":
      if (!held(e.id)) device.values[e.id] = e.value;
      break;
    case "files":
      device.files = e.files;
      break;
    case "presets":
      device.presets = e.presets;
      break;
    case "refused":
      say(e.message);
      break;
    case "models":
      device.models = e.files;
      break;
    case "irs":
      device.irs = e.files;
      break;
    case "transfer":
      Object.assign(transfer, { name: e.name, sent: e.sent, total: e.total, done: e.done, error: e.error, at: performance.now() });
      if (e.error) say(`${e.name}: ${e.error}`);
      break;
    case "presetsChanged":
      break;
    case "meters":
      Object.assign(meters, e.meters);
      break;
    case "disconnected":
      device.connected = false;
      device.detail = e.reason;
      Object.assign(meters, noMeters);
      break;
  }
}

/** Call once when the window opens. Returns a function that stops listening. */
export async function connectDevice(): Promise<() => void> {
  if (!inTauri) return startPreview();
  const { listen } = await import("@tauri-apps/api/event");
  const { invoke } = await import("@tauri-apps/api/core");
  // Listen first, then take the snapshot, so nothing falls between the two.
  const unlisten = await listen<DeviceEvent>("device", (msg) => apply(msg.payload));
  Object.assign(device, await invoke<Snapshot>("snapshot"));
  // Files dropped on the window from Finder go to the unit.
  const { getCurrentWebview } = await import("@tauri-apps/api/webview");
  const undrop = await getCurrentWebview().onDragDropEvent((e) => {
    dropping.on = e.payload.type === "enter" || e.payload.type === "over";
    if (e.payload.type === "drop" && e.payload.paths.length) void invoke("send_files", { paths: e.payload.paths });
  });
  return () => {
    unlisten();
    undrop();
  };
}

// Values waiting to go out: the newest per setting, sent once per display frame (about 60
// times a second), so a dragged knob does not queue hundreds of messages.
const pending = new Map<number, number>();
let flushQueued = false;

function flush() {
  flushQueued = false;
  const batch = [...pending];
  pending.clear();
  if (!inTauri) return;
  void import("@tauri-apps/api/core").then(({ invoke }) => {
    for (const [id, value] of batch) void invoke("set_param", { id, value });
  });
}

/** Asks the device to change a setting. The value it applies comes back as an event. */
export function setParam(id: number, value: number) {
  if (device.values[id] === value) return;
  device.values[id] = value; // show it at once
  if (!inTauri && device.files.preset && device.settings[id]?.inPreset) device.files.edited = true;
  touched.set(id, performance.now());
  pending.set(id, value);
  if (!flushQueued) {
    flushQueued = true;
    requestAnimationFrame(flush);
  }
}

/** Loads a model or an IR that is on the card, or deletes a file from it. */
export function cardFile(action: "model" | "ir" | "delete", path: string) {
  if (inTauri) {
    void import("@tauri-apps/api/core").then(({ invoke }) => invoke("card_file", { action, path }));
    return;
  }
  if (action === "model") device.files.model = path;
  else if (action === "ir") device.files.ir = path;
  else {
    device.models = device.models.filter((f) => f.path !== path);
    device.irs = device.irs.filter((f) => f.path !== path);
  }
  if (action !== "delete" && device.files.preset) device.files.edited = true;
}

/** Opens the file dialog; what is picked (.nam, .wav) is sent to the unit. */
export function pickFiles() {
  if (inTauri) void import("@tauri-apps/api/core").then(({ invoke }) => invoke("pick_files"));
  else say("Sending files needs the unit: this is the preview.");
}

export type PresetAction = "recall" | "save" | "new" | "rename" | "delete";

/** Recalls, saves, adds, renames or deletes a preset. What happened comes back as events. */
export function presetAction(action: PresetAction, name?: string) {
  if (inTauri) {
    void import("@tauri-apps/api/core").then(({ invoke }) => invoke("preset", { action, name: name ?? null }));
    return;
  }
  // The preview has no unit: act on the saved list, enough to work on the screen.
  const f = device.files;
  const num = (n: string) => n.slice(0, 2);
  if (action === "recall" && name) Object.assign(f, { preset: name, edited: false });
  else if (action === "save") f.edited = false;
  else if (action === "new") {
    let n = 1;
    while (device.presets.some((p) => p.number === n)) n++;
    const full = `${String(n).padStart(2, "0")} ${name || "Preset"}`;
    device.presets = [...device.presets, { name: full, number: n }].sort((a, b) => a.name.localeCompare(b.name));
    Object.assign(f, { preset: full, edited: false });
  } else if (action === "rename" && name) {
    const full = `${num(f.preset)} ${name}`;
    device.presets = device.presets.map((p) => (p.name === f.preset ? { ...p, name: full } : p));
    f.preset = full;
  } else if (action === "delete" && name) {
    device.presets = device.presets.filter((p) => p.name !== name);
    if (f.preset === name) Object.assign(f, { preset: "", edited: false });
  }
}

async function startPreview(): Promise<() => void> {
  const snapshot = (await import("./demo-snapshot.json")).default as Snapshot;
  Object.assign(device, structuredClone(snapshot));
  device.models = [device.files.model, "/AMP/Recto Modern Red.nam", "/AMP/Deluxe Reverb clean.nam", "/AMP/5150 Block Letter.nam"].filter(Boolean).sort().map((path) => ({ path, size: 665758 }));
  device.irs = [device.files.ir, "/IR/Mesa OS 4x12 V30.wav", "/IR/Greenback 2x12.wav"].filter(Boolean).sort().map((path) => ({ path, size: 96044 }));
  device.presets = ["01 Recto", device.files.preset || "02 Crunch", "03 Vintage clean"].map((name) => ({ name, number: Number(name.slice(0, 2)) }));
  preview.on = true;
  const t0 = performance.now();
  const timer = setInterval(() => {
    const t = (performance.now() - t0) / 1000;
    // A note every 1.6 s that dies away.
    const env = Math.exp(-3 * (t % 1.6));
    const find = (key: string) => device.values[device.settings.find((s) => s.key === key)?.id ?? -1] ?? 0;
    Object.assign(meters, {
      inputPeak: 0.25 * env + 0.0001,
      outputPeak: 0.5 * Math.sqrt(env) + 0.0001,
      gateGain: env > 0.02 ? 1 : 0,
      cpu: 56.2 + Math.sin(t) * 0.4, cpuMax: 57.4,
      cpuAmp: find("amp") ? 33.7 : 0, cpuDoubler: find("dbl") ? 1.1 : 0, cpuDelay: find("dl") ? 6.2 : 0,
      cpuReverb: find("rv") ? 9.4 : 0, cpuDrums: find("drum") ? 4.0 : 0,
      ampRunning: !!find("amp"), delayRinging: !!find("dl"), reverbRinging: !!find("rv"), drumsPlaying: !!find("drum"),
    });
  }, 33);
  return () => clearInterval(timer);
}
