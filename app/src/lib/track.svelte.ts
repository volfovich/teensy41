// The backing-track player and the tracks on the card, for the window: what the Tracks tab,
// the lane and the panel ask of the unit (protocol/PROTOCOL.md 4.8). The state itself is in
// `device.track` and `device.trackList` (device.svelte.ts), kept current by the unit's events.
//
// In the browser preview there is no unit: a small made-up tree and player stand in for it
// (the second half of this file), enough to look at the tab and the panel and to try them.
import { apply, device, holdPlace, holdTempo, inTauri, say, trackFound, trackSync } from "./device.svelte";
import type { TrackRow, TrackState } from "./types";

/** The colour of the lane and the panel. */
export const TRACK_COLOUR = "#45c4e6";

/** The numbers of `TRACK_COMMAND` (PROTOCOL.md 4.8). */
export const Cmd = {
  Play: 1,
  Pause: 2,
  Stop: 3,
  Seek: 4,
  Tempo: 5,
  OwnTempo: 6,
  OwnTempoHalf: 7,
  OwnTempoDouble: 8,
  BeatsPerBar: 9,
  Snap: 10,
  Scan: 11,
  LoopStart: 12,
  LoopEnd: 13,
  LoopSection: 14,
  LoopOff: 15,
  LoopOn: 16,
  LoopForget: 17,
  LoopBars: 18,
} as const;

const invoke = (command: string, args?: Record<string, unknown>) => import("@tauri-apps/api/core").then((api) => api.invoke(command, args));

// ---- Reading the state

/** The track's place now, in seconds: the unit's last report, moved on at the tempo while it plays. */
export function trackPlace(now: number): number {
  const t = device.track;
  if (!t || !t.path) return 0;
  let p = trackSync.place;
  if (t.playing === 2) {
    p += ((now - trackSync.at) / 1000) * (t.tempo / 100);
    const span = t.loopTo - t.loopFrom;
    if (t.loop === 2 && span > 0 && trackSync.place <= t.loopTo && p >= t.loopTo) p = t.loopFrom + ((p - t.loopTo) % span);
  }
  return Math.min(Math.max(p, 0), t.length);
}

/** The place, renewed on every display frame while a track plays: for the bar and the clocks. */
export const playhead = $state({ place: 0 });
if (typeof requestAnimationFrame !== "undefined") {
  const frame = (now: number) => {
    const p = trackPlace(now);
    if (p !== playhead.place) playhead.place = p;
    requestAnimationFrame(frame);
  };
  requestAnimationFrame(frame);
}

/** Seconds as "1:23", or "1:23.4" with tenths. */
export function clock(seconds: number, tenths = false): string {
  const s = Math.max(0, seconds);
  const whole = tenths ? Math.floor(s * 10) / 10 : Math.floor(s);
  const m = Math.floor(whole / 60);
  const rest = whole - 60 * m;
  return `${m}:${rest < 10 ? "0" : ""}${tenths ? rest.toFixed(1) : rest}`;
}

/** The length of a beat in seconds; 0 while the track's tempo is not known. */
export const beatSeconds = (t: TrackState) => (t.ownTempo > 0 ? 60 / t.ownTempo : 0);

/** The bar and the beat in it (both from 1) at a place, counted from the first beat found; null without a tempo. */
export function barBeat(t: TrackState, place: number): { bar: number; beat: number } | null {
  const beat = beatSeconds(t);
  if (!beat) return null;
  const per = Math.max(1, t.beatsPerBar);
  const k = Math.max(0, Math.floor((place - t.firstBeat) / beat + 1e-3));
  return { bar: Math.floor(k / per) + 1, beat: (k % per) + 1 };
}

/** A track's name as shown: without its folders and its ending, `_` as a space. */
export function trackTitle(path: string): string {
  return path.slice(path.lastIndexOf("/") + 1).replace(/\.(wav|mp3)$/i, "").replaceAll("_", " ");
}

/** The folder a path below /TRACKS is in; "" for /TRACKS itself. */
export const folderOf = (path: string) => (path.includes("/") ? path.slice(0, path.lastIndexOf("/")) : "");

const joined = (folder: string, name: string) => (folder ? `${folder}/${name}` : name);
const bytes = (text: string) => new TextEncoder().encode(text).length;

/** Why a name cannot be a track's or a folder's on the unit, or "". The limits are files.rs's. */
export function nameProblem(name: string, folder: string): string {
  if (!name) return "A name is needed.";
  if (name.startsWith(".")) return "A name cannot start with a dot.";
  if (/[\/\\:\u0000-\u001f]/.test(name)) return "A name cannot have / \\ or : in it.";
  if (bytes(name) > 127) return "That name is too long for the unit (at most 127 bytes).";
  if (bytes(joined(folder, name)) > 255) return "The path is too long for the unit (at most 255 bytes below /TRACKS).";
  return "";
}

// ---- The player

/** One of the player's commands; the state after it comes back as an event. */
export function trackCommand(command: number, a = 0, b = 0, c = 0) {
  if (inTauri) void invoke("track_command", { command, a, b, c });
  else demoCommand(command, a, b, c);
}

// A dragged bar or slider sends its newest value once per display frame; the device thread
// in turn sends only the newest one that is still waiting.
const waiting = new Map<number, number>();
let flushQueued = false;
function queue(command: number, a: number) {
  waiting.set(command, a);
  if (flushQueued) return;
  flushQueued = true;
  requestAnimationFrame(() => {
    flushQueued = false;
    const batch = [...waiting];
    waiting.clear();
    for (const [command, a] of batch) trackCommand(command, a);
  });
}

/** Goes to a time in the track. The bar shows it at once. */
export function seek(seconds: number) {
  const t = device.track;
  if (!t || !t.path) return;
  const s = Math.min(Math.max(seconds, 0), t.length);
  holdPlace(s);
  playhead.place = s;
  queue(Cmd.Seek, s);
}

/** Sets the tempo in percent of the track's own (50 to 150), at the same pitch. */
export function setTempo(percent: number) {
  const t = device.track;
  if (!t || !t.path) return;
  const p = Math.round(Math.min(150, Math.max(50, percent)));
  if (p === t.tempo) return;
  holdTempo(p, trackPlace(performance.now()));
  queue(Cmd.Tempo, p);
}

export function playPause() {
  const t = device.track;
  if (t?.path) trackCommand(t.playing === 2 ? Cmd.Pause : Cmd.Play);
}

/** Opens a track by its path below /TRACKS; "" closes the open one. */
export function openTrack(path: string) {
  if (inTauri) void invoke("track_open", { path });
  else demoOpen(path);
}

// ---- The tracks on the card

/** Shows a folder below /TRACKS in the Tracks tab ("" is /TRACKS itself). */
export function showFolder(folder: string) {
  if (inTauri) void invoke("track_list", { folder });
  else demoShow(folder);
}

/** Finds tracks in all folders by words; "" ends the search. The hits arrive in `trackFound`. */
export function findTracks(text: string) {
  if (!text) Object.assign(trackFound, { text: "", hits: [], more: false });
  if (inTauri) void invoke("track_find", { text });
  else demoFind(text);
}

export type TrackFileAction = "rename" | "new-folder" | "delete-folder" | "delete";

/** Renames or moves a track or folder (`path` to `to`), makes a folder, or deletes a folder or a track. Paths are below /TRACKS. */
export function trackFile(action: TrackFileAction, path: string, to?: string) {
  if (inTauri) void invoke("track_file", { action, path, to: to ?? null });
  else demoFile(action, path, to ?? "");
}

/** Tracks on the Mac waiting for the user's yes, because some would replace tracks in the folder. */
export const trackOffer = $state<{ paths: string[]; folder: string; replacing: string[] }>({ paths: [], folder: "", replacing: [] });

const localName = (path: string) => path.slice(path.lastIndexOf("/") + 1);

/** Sends tracks from the Mac into a folder below /TRACKS; asks first if any would replace one that is there. */
export function offerTracks(paths: string[], folder: string) {
  const tracks = paths.filter((p) => /\.(wav|mp3)$/i.test(p) && !localName(p).startsWith("."));
  if (tracks.length < paths.length) say("Only .wav and .mp3 files can be sent as backing tracks.");
  if (!tracks.length) return;
  // Only the folder shown is known here; its names do not tell capitals from small letters.
  const there = new Set(device.trackList.folder === folder ? device.trackList.rows.filter((r) => !r.folder).map((r) => r.name.toLowerCase()) : []);
  const replacing = tracks.map(localName).filter((n) => there.has(n.toLowerCase()));
  if (replacing.length) Object.assign(trackOffer, { paths: tracks, folder, replacing });
  else sendTracks(tracks, folder);
}

/** Sends tracks without asking (the answer to `trackOffer`). */
export function sendTracks(paths: string[], folder: string) {
  cancelTrackOffer();
  if (!paths.length) return;
  if (inTauri) void invoke("send_tracks", { paths, folder });
  else demoSend(paths, folder);
}

export function cancelTrackOffer() {
  Object.assign(trackOffer, { paths: [], folder: "", replacing: [] });
}

/** Opens the file dialog; the .wav and .mp3 files picked are sent into `folder`. */
export function pickTracks(folder: string) {
  if (inTauri) void invoke("pick_tracks").then((picked) => offerTracks(picked as string[], folder));
  else offerTracks(["/Users/me/Music/Backing/Slow blues in G.mp3", "/Users/me/Music/Backing/New jam in D.mp3"], folder);
}

// ---- The preview's made-up unit

let demoPaths = [
  "Backing in A minor.mp3",
  "Click 120.wav",
  "Blues/Shuffle in E.mp3",
  "Blues/Slow blues in G.mp3",
  "Blues/Texas shuffle in A.mp3",
  "Iron Maiden/Aces High.mp3",
  "Iron Maiden/The Trooper.mp3",
  "Iron Maiden/Live/Fear of the Dark.mp3",
  "Rock/Back_in_Black.mp3",
  "Rock/Iron man.mp3",
  "Rock/Smoke on the Water.wav",
  "Rock/A song whose name is long enough to need a second line in the list.mp3",
];
let demoFolders = ["Blues", "Iron Maiden", "Iron Maiden/Live", "Practice", "Rock"];
let demoFound = "";
let demoListNumber = 1;

const byName = (a: string, b: string) => a.localeCompare(b, undefined, { sensitivity: "base" });

function demoRows(folder: string): TrackRow[] {
  const inside = (list: string[], f: string) => list.filter((p) => folderOf(p) === f);
  const name = (p: string) => p.slice(p.lastIndexOf("/") + 1);
  return [
    ...inside(demoFolders, folder).map((p) => ({ name: name(p), folder: true, rows: inside(demoFolders, p).length + inside(demoPaths, p).length })).sort((a, b) => byName(a.name, b.name)),
    ...inside(demoPaths, folder).map((p) => ({ name: name(p), folder: false, rows: 0 })).sort((a, b) => byName(a.name, b.name)),
  ];
}

function demoShow(folder: string) {
  if (folder && !demoFolders.includes(folder)) folder = "";
  apply({ kind: "trackList", folder, rows: demoRows(folder) });
}

function demoFind(text: string) {
  demoFound = text;
  if (!text) return;
  const words = text.toLowerCase().replaceAll("_", " ").split(/\s+/).filter(Boolean);
  const hits = demoPaths.filter((p) => words.every((w) => p.toLowerCase().replaceAll("_", " ").includes(w))).sort(byName);
  apply({ kind: "trackFound", text, hits, more: false });
}

const noTrack: TrackState = {
  path: "", playing: 0, place: 0, length: 0, tempo: 100, ownTempo: 0, sure: false, scan: -1, firstBeat: 0, beatsPerBar: 4, snap: true,
  loop: 0, loopFrom: 0, loopTo: 0, gaps: 0, fileKind: 0, fileRate: 0, channels: 0, kbps: 0, tracks: 0, reading: false, listNumber: 1,
};
let demo: TrackState = { ...noTrack };

function demoReport() {
  demo.tracks = demoPaths.length;
  demo.listNumber = demoListNumber;
  apply({ kind: "track", state: { ...demo } });
}

// A tempo that differs from track to track, found when the made-up scan ends.
const demoTempo = (path: string) => 84 + ([...path].reduce((sum, ch) => sum + ch.charCodeAt(0), 0) % 80);

function demoOpen(path: string) {
  if (path && !demoPaths.includes(path)) return say("No such track");
  const mp3 = /\.mp3$/i.test(path);
  demo = path
    ? { ...noTrack, path, playing: 1, length: 150 + (demoTempo(path) % 7) * 21.3, scan: 0, fileKind: mp3 ? 2 : 1, fileRate: mp3 ? 44100 : 48000, channels: 2, kbps: mp3 ? 192 : 0 }
    : { ...noTrack };
  demoReport();
}

function demoCommand(command: number, a: number, b: number, c: number) {
  const t = demo;
  if (!t.path) return say("No track is open");
  const beat = beatSeconds(t);
  const snapped = (s: number) => (t.snap && beat ? t.firstBeat + Math.round((s - t.firstBeat) / beat) * beat : s);
  const loopTo = (from: number, to: number) => {
    if (to - from < 0.25 || from < 0 || to > t.length) return say("A loop is 0.25 s or longer, inside the track");
    Object.assign(t, { loop: 2, loopFrom: from, loopTo: to });
    if (t.place < from || t.place >= to) t.place = from;
  };
  switch (command) {
    case Cmd.Play:
      t.playing = 2;
      break;
    case Cmd.Pause:
      if (t.playing === 2) t.playing = 3;
      break;
    case Cmd.Stop:
      Object.assign(t, { playing: 1, place: t.loop === 2 ? t.loopFrom : 0 });
      break;
    case Cmd.Seek:
      t.place = Math.min(Math.max(a, 0), t.length);
      break;
    case Cmd.Tempo:
      t.tempo = Math.round(Math.min(150, Math.max(50, a)));
      break;
    case Cmd.OwnTempo:
      if (a < 30 || a > 300) return say("The tempo is 30 to 300 BPM");
      Object.assign(t, { ownTempo: a, sure: true, scan: 100 });
      break;
    case Cmd.OwnTempoHalf:
    case Cmd.OwnTempoDouble:
      if (!beat) return say("The track's tempo is not known");
      t.ownTempo *= command === Cmd.OwnTempoHalf ? 0.5 : 2;
      break;
    case Cmd.BeatsPerBar:
      t.beatsPerBar = Math.min(16, Math.max(1, Math.round(a)));
      break;
    case Cmd.Snap:
      t.snap = a !== 0;
      break;
    case Cmd.Scan:
      Object.assign(t, { scan: 0, ownTempo: 0, sure: false });
      break;
    case Cmd.LoopStart:
      Object.assign(t, { loop: 3, loopFrom: snapped(t.place), loopTo: 0 });
      break;
    case Cmd.LoopEnd:
      if (t.loop === 0) return say("Mark the loop's start first");
      loopTo(t.loopFrom, snapped(t.place));
      break;
    case Cmd.LoopSection:
      loopTo(a, b);
      break;
    case Cmd.LoopOff:
      if (t.loop === 2) t.loop = 1;
      break;
    case Cmd.LoopOn:
      if (t.loop === 1) t.loop = 2;
      break;
    case Cmd.LoopForget:
      Object.assign(t, { loop: 0, loopFrom: 0, loopTo: 0 });
      break;
    case Cmd.LoopBars: {
      if (!beat) return say("The track's tempo is not known");
      const from = t.firstBeat + ((a - 1) * t.beatsPerBar + (b - 1)) * beat;
      loopTo(from, from + c * t.beatsPerBar * beat);
      break;
    }
    default:
      return say("Unknown command");
  }
  demoReport();
}

function demoFile(action: TrackFileAction, path: string, to: string) {
  const under = (p: string, folder: string) => p === folder || p.startsWith(`${folder}/`);
  const moved = (p: string) => (under(p, path) ? to + p.slice(path.length) : p);
  if (action === "new-folder") {
    if (!demoFolders.includes(path)) demoFolders.push(path);
  } else if (action === "rename") {
    if (demoFolders.includes(to) || demoPaths.includes(to)) return say("That name is taken");
    if (under(demo.path, path)) demo = { ...noTrack };
    demoFolders = demoFolders.map(moved);
    demoPaths = demoPaths.map(moved);
  } else {
    if (under(demo.path, path)) demo = { ...noTrack };
    demoFolders = demoFolders.filter((p) => !under(p, path));
    demoPaths = demoPaths.filter((p) => !under(p, path));
  }
  demoChanged();
}

// The card changed: as the unit does, a new list number, and the lists shown are made anew.
function demoChanged() {
  demoListNumber++;
  demoReport();
  demoShow(device.trackList.folder);
  demoFind(demoFound);
}

// Sending in the preview: the progress runs through in a few seconds and the tracks join the list.
function demoSend(paths: string[], folder: string) {
  const size = 3_400_000;
  let step = 0;
  const steps = 12;
  const timer = setInterval(() => {
    const index = Math.floor(step / steps);
    const sent = Math.round(((step % steps) + 1) * (size / steps));
    const done = (step + 1) % steps === 0;
    const name = localName(paths[index]);
    apply({ kind: "transfer", name, sent, total: size, done, error: "", index: index + 1, count: paths.length, allSent: index * size + sent, allTotal: paths.length * size });
    if (done) {
      const path = joined(folder, name);
      if (!demoPaths.some((p) => p.toLowerCase() === path.toLowerCase())) demoPaths.push(path);
      demoChanged();
    }
    if (++step >= steps * paths.length) clearInterval(timer);
  }, 180);
}

/** Starts the preview's player: a track open and playing a loop at 80 %. Returns a function that stops it. */
export function startTrackDemo(): () => void {
  const beat = 60 / 124.56;
  demo = {
    ...noTrack, path: "Rock/Iron man.mp3", playing: 2, place: 96.4, length: 215.4, tempo: 80, ownTempo: 124.56, sure: true, scan: 100, firstBeat: 0.76,
    loop: 2, loopFrom: 0.76 + 192 * beat, loopTo: 0.76 + 224 * beat, fileKind: 2, fileRate: 44100, channels: 2, kbps: 192,
  };
  demoReport();
  demoShow("Rock");
  // Four reports a second, as the unit sends them; the window moves the place in between.
  const timer = setInterval(() => {
    const t = demo;
    if (t.playing === 2) {
      t.place += 0.25 * (t.tempo / 100);
      if (t.loop === 2 && t.place >= t.loopTo) t.place = t.loopFrom + (t.place - t.loopTo);
      if (t.place >= t.length) Object.assign(t, { playing: 1, place: 0 });
      demoReport();
    } else if (t.path && t.scan >= 0 && t.scan < 100) {
      // The scan runs only while the track does not play.
      t.scan = Math.min(100, t.scan + 4);
      if (t.scan === 100) Object.assign(t, { ownTempo: demoTempo(t.path), sure: demoTempo(t.path) % 5 !== 0, firstBeat: 0.31 });
      demoReport();
    }
  }, 250);
  return () => clearInterval(timer);
}
