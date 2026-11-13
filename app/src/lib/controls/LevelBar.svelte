<script lang="ts">
  // A block's output level: a thin bar for the block's tile, or the held peak as text for its
  // editor. The same scale and colours as the IN and OUT meters; over full scale (above 0 dBFS)
  // is red: fine between blocks, but it would clip at the output.
  import { meterStyle } from "$lib/blocks";
  import { toDb } from "$lib/format";

  let { peak, text = false }: { peak: number; text?: boolean } = $props();

  const floor = meterStyle.floorDb;
  const pos = (db: number) => Math.min(100, Math.max(0, ((db - floor) / -floor) * 100));

  // The bar falls at 30 dB per second; the held peak stays for 1.5 s.
  let shown = $state(floor);
  let hold = $state(floor);
  let holdAt = 0;
  let last = 0;
  $effect(() => {
    const db = toDb(peak, floor);
    const now = performance.now();
    const before = shown;
    shown = Math.max(db, before - (30 * (now - last)) / 1000, floor);
    last = now;
    if (db >= hold || now - holdAt > 1500) {
      hold = db;
      holdAt = now;
    }
  });
  const colourOf = (db: number) => (db > 0 ? meterStyle.hot : db >= meterStyle.warnDb ? meterStyle.warn : meterStyle.ok);
</script>

{#if text}
  <span class="text" class:over={hold > 0} title="The highest level at this block's output lately. Above 0 dBFS is over full scale: fine between blocks, but it would clip at the output.">
    {hold <= floor ? "silent" : `${hold > 0 ? "+" : ""}${hold.toFixed(1)} dBFS`}
  </span>
{:else}
  <div class="bar" aria-hidden="true">
    <div class="level" style:width="{pos(shown)}%" style:background={colourOf(shown)}></div>
    {#if hold > floor}<div class="hold" style:left="{pos(hold)}%" style:background={colourOf(hold)}></div>{/if}
  </div>
{/if}

<style>
  .bar {
    position: absolute;
    left: 10px;
    right: 10px;
    bottom: 5px;
    height: 3px;
    border-radius: 2px;
    background: rgba(0, 0, 0, 0.35);
    overflow: hidden;
    pointer-events: none;
  }
  .level {
    height: 100%;
  }
  .hold {
    position: absolute;
    top: 0;
    width: 2px;
    margin-left: -2px;
    height: 100%;
  }
  .text {
    color: #8a92a0;
    font-variant-numeric: tabular-nums;
    white-space: nowrap;
  }
  .text.over {
    color: #ff4d4d;
    font-weight: 600;
  }
</style>
