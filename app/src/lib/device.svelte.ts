// The window's copy of the device: filled from the Rust side's snapshot and kept current by
// its events. The device owns the state; the UI shows what it reports.
import { invoke } from "@tauri-apps/api/core";
import { listen } from "@tauri-apps/api/event";
import type { DeviceEvent, Snapshot } from "./types";

const empty: Snapshot = {
  connected: false,
  detail: "Looking for the unit…",
  info: null,
  settings: [],
  values: [],
  files: { model: "", ir: "", preset: "", edited: false, card: false },
};

export const device = $state<Snapshot>({ ...empty });

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
      device.values = e.values;
      device.files = e.files;
      break;
    case "param":
      device.values[e.id] = e.value;
      break;
    case "files":
      device.files = e.files;
      break;
    case "disconnected":
      device.connected = false;
      device.detail = e.reason;
      break;
  }
}

/** Call once when the window opens. Returns a function that stops listening. */
export async function connectDevice(): Promise<() => void> {
  // Listen first, then take the snapshot, so nothing falls between the two.
  const unlisten = await listen<DeviceEvent>("device", (msg) => apply(msg.payload));
  Object.assign(device, await invoke<Snapshot>("snapshot"));
  return unlisten;
}

/** Asks the device to change a setting. The value it applies comes back as an event. */
export function setParam(id: number, value: number) {
  device.values[id] = value; // show it at once; the device's answer corrects it if needed
  void invoke("set_param", { id, value });
}
