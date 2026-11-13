import { Unit, type Setting } from "./types";

/** A setting's value as the unit shows it: "30 %", "380 ms", "+3.5 dB", a choice's name. */
export function formatValue(s: Setting, v: number): string {
  if (s.choices.length && v >= 0 && v < s.choices.length) return s.choices[v];
  if (s.name === "Loop from" && v === 0) return "off";
  switch (s.unit) {
    case Unit.OnOff:
      return v ? "On" : "Off";
    case Unit.Percent:
      return `${v} %`;
    case Unit.Ms:
      return `${v} ms`;
    case Unit.HalfDb: {
      const db = v / 2;
      return `${db > 0 ? "+" : ""}${db.toFixed(1)} dB`;
    }
    case Unit.Bpm:
      return `${v} BPM`;
    case Unit.Bars:
      return `${v} ${v === 1 ? "bar" : "bars"}`;
    case Unit.Db:
      return `${v} dB`;
    case Unit.CentiHz:
      return `${(v / 100).toFixed(2)} Hz`;
    case Unit.Name:
      // Without names: numbered from 1, as the device's shell shows them. 0 can mean none.
      return s.key === "drum song" ? (v ? String(v) : "off") : String(v + 1);
    default:
      return String(v);
  }
}

/** The file's name without its folder. */
export function baseName(path: string): string {
  return path.slice(path.lastIndexOf("/") + 1);
}

/** A peak (linear, 1.0 = full scale) in dBFS, not below `floor`. */
export function toDb(peak: number, floor = -120): number {
  return peak > 0 ? Math.max(floor, 20 * Math.log10(peak)) : floor;
}

/** What the user typed for a setting ("3.5", "-6 dB", "380") as the setting's raw value, or null. */
export function parseTyped(s: Setting, text: string): number | null {
  const x = parseFloat(text.replace(",", "."));
  if (!Number.isFinite(x)) return null;
  const raw = Math.round(s.unit === Unit.HalfDb ? x * 2 : s.unit === Unit.CentiHz ? x * 100 : x);
  return Math.min(s.max, Math.max(s.min, raw));
}

/** The number to start typing from: the value in the unit shown, without the unit. */
export function typedValue(s: Setting, v: number): string {
  return s.unit === Unit.HalfDb ? (v / 2).toFixed(1) : s.unit === Unit.CentiHz ? (v / 100).toFixed(2) : String(v);
}
