// What the Rust side sends (app/crates/namforge-core/src/types.rs and app/src-tauri/src/lib.rs).

export interface DeviceInfo {
  protocol: number;
  firmware: string;
  blockSize: number;
  sampleRate: number;
  maxModels: number;
  maxIrs: number;
  maxPath: number;
  savedSettings: number;
  settings: number;
  schemaHash: number;
  port: string;
}

export interface Setting {
  id: number;
  key: string;
  name: string;
  label: string;
  min: number;
  max: number;
  default: number;
  step: number;
  unit: number;
  inPreset: boolean;
  choices: string[];
}

export interface Files {
  model: string;
  ir: string;
  preset: string;
  edited: boolean;
  card: boolean;
}

/** One meter frame (PROTOCOL.md 4.6). Peaks are linear, 1.0 = full scale. */
export interface Meters {
  inputPeak: number;
  outputPeak: number;
  gateGain: number;
  clipped: number;
  cpu: number;
  cpuMax: number;
  cpuAmp: number;
  cpuDoubler: number;
  cpuDelay: number;
  cpuReverb: number;
  cpuDrums: number;
  overruns: number;
  overloadMutes: number;
  ampRunning: boolean;
  delayRinging: boolean;
  reverbRinging: boolean;
  drumsPlaying: boolean;
  muted: boolean;
}

/** A preset on the unit's SD card: "03 Lead" and its number (0 if the name has none). */
export interface Preset {
  name: string;
  number: number;
}

export interface Snapshot {
  connected: boolean;
  detail: string;
  info: DeviceInfo | null;
  settings: Setting[];
  values: number[];
  files: Files;
  presets: Preset[];
}

export type DeviceEvent =
  | { kind: "searching"; detail: string }
  | { kind: "connected"; info: DeviceInfo }
  | { kind: "schema"; settings: Setting[] }
  | { kind: "state"; values: number[]; files: Files }
  | { kind: "param"; id: number; value: number }
  | { kind: "files"; files: Files }
  | { kind: "meters"; meters: Meters }
  | { kind: "presets"; presets: Preset[] }
  | { kind: "presetsChanged" }
  | { kind: "refused"; message: string }
  | { kind: "disconnected"; reason: string };

// A setting's unit (protocol/PROTOCOL.md section 6).
export const Unit = { None: 0, OnOff: 1, Percent: 2, Ms: 3, HalfDb: 4, Bpm: 5, Bars: 6, Db: 7, Name: 8 } as const;
