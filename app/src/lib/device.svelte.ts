// The window's copy of the device: filled from the Rust side's snapshot and kept current by
// its events. The device owns the state; the UI shows what it reports.
//
// In a plain browser (npm run dev, without Tauri) there is no device: the window then shows a
// saved copy of the unit's state with moving meters, for working on the UI.
import type { DeviceEvent, Job, Meters, Snapshot, Transfer, TunerReading } from "./types";

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
  modelInfo: null,
  irInfo: null,
  card: null,
};

const noMeters: Meters = {
  inputPeak: 0, outputPeak: 0, gateGain: 1, clipped: 0,
  cpu: 0, cpuMax: 0, cpuAmp: 0, cpuDoubler: 0, cpuDelay: 0, cpuReverb: 0, cpuDrums: 0,
  overruns: 0, overloadMutes: 0,
  ampRunning: false, delayRinging: false, reverbRinging: false, drumsPlaying: false, muted: false, tunerOpen: false, stagePeaks: [],
};

export const device = $state<Snapshot>({ ...empty });
export const meters = $state<Meters>({ ...noMeters });
/** True when the window runs without Tauri and shows saved data. */
export const preview = $state({ on: false });
/** The last thing the unit refused ("No such preset"), for the screen to show briefly. */
export const notice = $state({ text: "", at: 0 });
/** The file being sent to the unit, or the last one sent (`at`: when it last changed). */
export const transfer = $state<Transfer & { at: number }>({ name: "", sent: 0, total: 0, done: true, error: "", at: 0 });
/** The tuner's last reading. Whether it is open is in `meters.tunerOpen`. */
export const tuner = $state<TunerReading>({ valid: false, hz: 0, note: 0, octave: 0, cents: 0 });
/** The backup or restore under way, or the last one (`title` empty: none yet). */
export const job = $state<Job>({ title: "", detail: "", done: 0, total: 0, finished: true, error: "" });
/** A backup folder the user chose, waiting for the yes to send it back. */
export const restoreOffer = $state<{ folder: string; files: string[] }>({ folder: "", files: [] });
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
    case "tuner":
      Object.assign(tuner, { valid: e.valid, hz: e.hz, note: e.note, octave: e.octave, cents: e.cents });
      break;
    case "job":
      Object.assign(job, { title: e.title, detail: e.detail, done: e.done, total: e.total, finished: e.finished, error: e.error });
      break;
    case "models":
      device.models = e.files;
      break;
    case "irs":
      device.irs = e.files;
      break;
    case "modelInfo":
      device.modelInfo = e.info;
      break;
    case "irInfo":
      device.irInfo = e.info;
      break;
    case "card":
      device.card = e.space;
      break;
    case "transfer":
      Object.assign(transfer, { name: e.name, sent: e.sent, total: e.total, done: e.done, error: e.error, at: performance.now() });
      if (e.error) say(`${e.name}: ${e.error}`);
      break;
    case "presetsChanged":
      break;
    case "meters":
      // The tuner's state just asked for stands until the unit has had time to follow.
      if (performance.now() - tunerAskedAt < 600) e.meters.tunerOpen = meters.tunerOpen;
      Object.assign(meters, e.meters);
      break;
    case "disconnected":
      device.connected = false;
      device.detail = e.reason;
      Object.assign(meters, noMeters);
      break;
  }
}

/** A line for the terminal the app was started from (shown with NAMFORGE_LOG=1). */
export function uiLog(text: string) {
  if (inTauri) void import("@tauri-apps/api/core").then(({ invoke }) => invoke("ui_log", { text }));
}

/** Call once when the window opens. Returns a function that stops listening. */
export async function connectDevice(): Promise<() => void> {
  if (!inTauri) return startPreview();
  const { listen } = await import("@tauri-apps/api/event");
  const { invoke } = await import("@tauri-apps/api/core");
  // Listen first, then take the snapshot, so nothing falls between the two.
  window.addEventListener("error", (e) => uiLog(`script error: ${e.message} (${e.filename}:${e.lineno})`));
  window.addEventListener("unhandledrejection", (e) => uiLog(`unhandled rejection: ${e.reason}`));
  const unlisten = await listen<DeviceEvent>("device", (msg) => apply(msg.payload));
  Object.assign(device, await invoke<Snapshot>("snapshot"));
  // Files dropped on the window from Finder go to the unit.
  const { getCurrentWebview } = await import("@tauri-apps/api/webview");
  const undrop = await getCurrentWebview().onDragDropEvent((e) => {
    dropping.on = e.payload.type === "enter" || e.payload.type === "over";
    if (e.payload.type === "drop" && e.payload.paths.length) offerFiles(e.payload.paths);
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
  if (inTauri) void import("@tauri-apps/api/core").then(({ invoke }) => invoke<string[]>("pick_files")).then(offerFiles);
  else offerFiles(["/Users/me/Downloads/New amp.nam", "/Users/me/Downloads/Greenback 2x12.wav"]);
}

/** Files on the Mac waiting for the user's yes, because some would replace files on the card. */
export const offered = $state<{ paths: string[]; replacing: string[] }>({ paths: [], replacing: [] });

// Where a file would go on the card, as the Rust side decides it (files.rs): by its extension.
function cardPath(local: string): string {
  const name = local.split("/").pop() ?? "";
  return /\.nam$/i.test(name) ? `/AMP/${name}` : /\.wav$/i.test(name) ? `/IR/${name}` : "";
}

/** Sends files to the unit; asks first if any would replace a file that is on the card. */
export function offerFiles(paths: string[]) {
  if (!paths.length) return;
  // The card's file names do not tell capitals from small letters.
  const onCard = new Set([...device.models, ...device.irs].map((f) => f.path.toLowerCase()));
  const replacing = paths.filter((p) => onCard.has(cardPath(p).toLowerCase())).map((p) => p.split("/").pop() ?? p);
  if (replacing.length) Object.assign(offered, { paths, replacing });
  else sendFiles(paths);
}

/** Sends files without asking (the answer to `offered`). */
export function sendFiles(paths: string[]) {
  Object.assign(offered, { paths: [], replacing: [] });
  if (inTauri) void import("@tauri-apps/api/core").then(({ invoke }) => invoke("send_files", { paths }));
  else say("Sending files needs the unit: this is the preview.");
}

export function cancelOffer() {
  Object.assign(offered, { paths: [], replacing: [] });
}

/** Opens or closes the tuner. The guitar is muted while it is open. */
let tunerAskedAt = -1e9;

export function setTuner(open: boolean) {
  tunerAskedAt = performance.now();
  meters.tunerOpen = open; // at once; the unit's meter frames confirm it
  tuner.valid = false;
  if (inTauri) void import("@tauri-apps/api/core").then(({ invoke }) => invoke("tuner", { open }));
}

/** Copies the card to a new folder in ~/Music/NAM Forge/Backups, named by date and time. */
export function backupCard() {
  const d = new Date();
  const two = (n: number) => String(n).padStart(2, "0");
  const name = `${d.getFullYear()}-${two(d.getMonth() + 1)}-${two(d.getDate())} ${two(d.getHours())}.${two(d.getMinutes())}`;
  if (!inTauri) {
    Object.assign(job, { title: "Backup", detail: `24 files, 11.3 MB, in ~/Music/NAM Forge/Backups/${name}`, done: 24, total: 24, finished: true, error: "" });
    return;
  }
  Object.assign(job, { title: "Backup", detail: "Starting", done: 0, total: 0, finished: false, error: "" });
  void import("@tauri-apps/api/core")
    .then(({ invoke }) => invoke<string>("backup", { name }))
    .catch((why) => Object.assign(job, { finished: true, error: String(why) }));
}

/** Lets the user choose a backup folder; the screen then asks before anything is sent. */
export function chooseBackup() {
  if (!inTauri) {
    Object.assign(restoreOffer, { folder: "~/Music/NAM Forge/Backups/2026-11-13 14.30", files: ["/AMP/a.nam", "/AMP/b.nam", "/IR/c.wav", "/PRESETS/01 Recto.txt"] });
    return;
  }
  void import("@tauri-apps/api/core")
    .then(({ invoke }) => invoke<[string, string[]] | null>("pick_backup"))
    .then((picked) => {
      if (!picked) return;
      if (picked[1].length) Object.assign(restoreOffer, { folder: picked[0], files: picked[1] });
      else say("That folder is not a backup: it has no AMP, IR, PRESETS or DRUMS folder with files.");
    });
}

/** Sends the chosen backup back to the card (the yes to `restoreOffer`), or drops the offer. */
export function answerRestore(yes: boolean) {
  const folder = restoreOffer.folder;
  Object.assign(restoreOffer, { folder: "", files: [] });
  if (!yes || !folder) return;
  if (!inTauri) return say("Restoring needs the unit: this is the preview.");
  Object.assign(job, { title: "Restore", detail: "Starting", done: 0, total: 0, finished: false, error: "" });
  void import("@tauri-apps/api/core").then(({ invoke }) => invoke("restore", { folder }));
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
  device.modelInfo = { name: "", gearMake: "Marshall", gearModel: "JCM 800 2203", gearType: "amp", toneType: "hi_gain", sampleRate: 48000, loudness: -20.6, inputLevelDbu: 12, outputLevelDbu: null, calibrationDb: -9.3, levelDb: 8.6 };
  // A made-up cab: a few decaying resonances for the picture, a speaker-like curve for the response.
  const wave: [number, number][] = Array.from({ length: 400 }, (_, c) => {
    const t = c / 400;
    const v = Math.exp(-9 * t) * (Math.sin(260 * t) + 0.5 * Math.sin(95 * t + 1) + 0.25 * Math.sin(700 * t));
    const a = Math.min(1, Math.abs(v) / 1.2 + 0.02 * Math.exp(-4 * t));
    return [-a * (0.7 + 0.3 * Math.sin(40 * t)), a];
  });
  const response = Array.from({ length: 120 }, (_, i) => {
    const oct = Math.log2((20 * 1000 ** (i / 119)) / 1000); // octaves from 1 kHz
    const low = -18 * Math.max(0, -3.2 - oct) ** 1.3;
    const high = -30 * Math.max(0, oct - 2.2) ** 1.2;
    return low + high + 5 * Math.exp(-((oct + 3) ** 2) * 2) + 4 * Math.exp(-((oct - 1.5) ** 2) * 3) - 3 * Math.exp(-((oct - 0.2) ** 2) * 6) + Math.sin(i * 1.7);
  });
  device.irInfo = { sampleRate: 96000, channels: 2, bits: 24, frames: 19968, length: 3992, resampled: true, cut: true, gain: 0.44, wave, response };
  device.card = { totalKb: 31154688, freeKb: 31020032 };
  device.presets = ["01 Recto", device.files.preset || "02 Crunch", "03 Vintage clean"].map((name) => ({ name, number: Number(name.slice(0, 2)) }));
  preview.on = true;
  const t0 = performance.now();
  const timer = setInterval(() => {
    const t = (performance.now() - t0) / 1000;
    // A note every 1.6 s that dies away.
    const env = Math.exp(-3 * (t % 1.6));
    const find = (key: string) => device.values[device.settings.find((s) => s.key === key)?.id ?? -1] ?? 0;
    // A low E that drifts from flat to in tune.
    if (meters.tunerOpen) Object.assign(tuner, { valid: env > 0.05, hz: 82.41 * 2 ** ((-18 * env) / 1200), note: 4, octave: 2, cents: -18 * env });
    Object.assign(meters, {
      inputPeak: 0.25 * env + 0.0001,
      outputPeak: 0.5 * Math.sqrt(env) + 0.0001,
      gateGain: env > 0.02 ? 1 : 0,
      cpu: 56.2 + Math.sin(t) * 0.4, cpuMax: 57.4,
      cpuAmp: find("amp") ? 33.7 : 0, cpuDoubler: find("dbl") ? 1.1 : 0, cpuDelay: find("dl") ? 6.2 : 0,
      cpuReverb: find("rv") ? 9.4 : 0, cpuDrums: find("drum") ? 4.0 : 0,
      // amp, gate, cab, doubler, delay, reverb: the amp adds level, the reverb goes over full scale.
      stagePeaks: [0.9, 0.9, 0.45, 0.45, 0.5, 1.3].map((p) => p * Math.sqrt(env) + 0.0001),
      ampRunning: !!find("amp"), delayRinging: !!find("dl"), reverbRinging: !!find("rv"), drumsPlaying: !!find("drum"),
    });
  }, 33);
  return () => clearInterval(timer);
}
