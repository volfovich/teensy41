<script lang="ts">
  // A level meter: the peak since the last frame as a bar on a dB scale, with a held peak
  // mark that falls back slowly.
  import { meterStyle } from "$lib/blocks";
  import { toDb } from "$lib/format";

  let { label, peak, clipped = false }: { label: string; peak: number; clipped?: boolean } = $props();

  const floor = meterStyle.floorDb;
  const pos = (db: number) => Math.min(100, Math.max(0, ((db - floor) / -floor) * 100));

  // The bar falls at 30 dB per second and the peak mark holds for 1.2 s, so short notes can be read.
  let shown = $state(floor);
  let hold = $state(floor);
  let holdAt = 0;
  let last = 0;
  $effect(() => {
    const db = toDb(peak, floor);
    const now = performance.now();
    const fallen = shown - (30 * (now - last)) / 1000;
    last = now;
    shown = Math.max(db, fallen, floor);
    if (db >= hold || now - holdAt > 1200) {
      hold = db;
      holdAt = now;
    }
  });
  const colour = $derived(shown >= meterStyle.hotDb ? meterStyle.hot : shown >= meterStyle.warnDb ? meterStyle.warn : meterStyle.ok);
</script>

<div class="meter" title="{label}: {hold <= floor ? 'silent' : hold.toFixed(1) + ' dBFS peak'}">
  <span class="label">{label}</span>
  <div class="bar">
    <div class="level" style:width="{pos(shown)}%" style:background={colour}></div>
    {#if hold > floor}<div class="hold" style:left="{pos(hold)}%"></div>{/if}
    <div class="tick" style:left="{pos(meterStyle.warnDb)}%"></div>
  </div>
  <span class="db" class:clip={clipped}>{clipped ? "CLIP" : hold <= floor ? "–∞" : hold.toFixed(0)}</span>
</div>

<style>
  .meter {
    display: flex;
    align-items: center;
    gap: 7px;
  }
  .label {
    font-size: 10.5px;
    letter-spacing: 0.08em;
    color: #8a92a0;
    width: 24px;
  }
  .bar {
    position: relative;
    width: 110px;
    height: 8px;
    background: #23272f;
    border-radius: 4px;
    overflow: hidden;
  }
  .level {
    height: 100%;
    border-radius: 4px;
  }
  .hold {
    position: absolute;
    top: 0;
    width: 2px;
    height: 100%;
    background: #e6e8ec;
  }
  .tick {
    position: absolute;
    top: 0;
    width: 1px;
    height: 100%;
    background: #101216;
    opacity: 0.7;
  }
  .db {
    width: 30px;
    font-size: 11.5px;
    font-variant-numeric: tabular-nums;
    color: #c3c8d1;
    text-align: right;
  }
  .db.clip {
    color: #ff4d4d;
    font-weight: 700;
  }
</style>
