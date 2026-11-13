// The chain's blocks, from the definitions both UIs share (protocol/blocks.json), joined with
// the device's settings table.
import defs from "../../../protocol/blocks.json";
import type { Meters, Setting } from "./types";

export interface BlockDef {
  id: string;
  name: string;
  short: string;
  colour: string;
  on?: string;
  onInverted?: boolean;
  file?: string;
  summary?: string;
  activity?: string;
  sections: { title?: string; settings: string[] }[];
}

export interface Block {
  def: BlockDef;
  /** The block's switch, if it has one. */
  on: Setting | null;
  /** The setting whose value the block's tile shows. */
  summary: Setting | null;
  sections: { title: string; settings: Setting[] }[];
}

export const meterStyle = defs.meters;

function build(def: BlockDef, find: (ref: string) => Setting | undefined): Block {
  return {
    def,
    on: (def.on && find(def.on)) || null,
    summary: (def.summary && find(def.summary)) || null,
    sections: def.sections
      .map((s) => ({ title: s.title ?? "", settings: s.settings.map(find).filter((x): x is Setting => !!x) }))
      .filter((s) => s.settings.length),
  };
}

/** The chain's blocks in order, the drums lane, and a block for settings no block names. */
export function buildBlocks(settings: Setting[]): { chain: Block[]; lane: Block; other: Block | null } {
  const used = new Set<number>();
  const find = (ref: string) => {
    const s = ref.startsWith("@") ? settings.find((x) => x.name === ref.slice(1)) : settings.find((x) => x.key === ref);
    if (s) used.add(s.id);
    return s;
  };
  const chain = (defs.blocks as BlockDef[]).map((d) => build(d, find));
  const lane = build(defs.lane as BlockDef, find);
  const rest = settings.filter((s) => !used.has(s.id));
  const other: Block | null = rest.length
    ? { def: { id: "other", name: "Other", short: "OTHER", colour: "#aab2bf", sections: [] }, on: null, summary: null, sections: [{ title: "", settings: rest }] }
    : null;
  return { chain, lane, other };
}

/** Whether the block is switched on (a block without a switch always is). */
export function blockOn(b: Block, values: number[]): boolean {
  if (!b.on) return true;
  const v = (values[b.on.id] ?? b.on.default) !== 0;
  return b.def.onInverted ? !v : v;
}

/** The value that switches the block on or off. */
export function blockOnValue(b: Block, on: boolean): number {
  return (b.def.onInverted ? !on : on) ? 1 : 0;
}

/** Whether the device reports the block as working (an effect's tail still ringing, the drums playing). */
export function blockActive(b: Block, m: Meters): boolean {
  return b.def.activity ? !!(m as unknown as Record<string, unknown>)[b.def.activity] : false;
}
