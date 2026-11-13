import { Unit, type Setting } from "./types";

/** A setting's value as the unit shows it: "30 %", "380 ms", "+3.5 dB", a choice's name. */
export function formatValue(s: Setting, v: number): string {
  if (s.choices.length && v >= 0 && v < s.choices.length) return s.choices[v];
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
    default:
      return String(v);
  }
}

/** The file's name without its folder. */
export function baseName(path: string): string {
  return path.slice(path.lastIndexOf("/") + 1);
}
