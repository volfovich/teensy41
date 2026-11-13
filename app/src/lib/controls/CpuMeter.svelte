<script lang="ts">
  // The unit's processing load: how much of each audio block's time the whole chain takes.
  // The bar is the average, the mark the highest block lately (held for 2 s). Above the
  // budget (80 %) a block may come too late, which is heard as a click or a dropout.
  import { meterStyle } from "$lib/blocks";

  let { cpu, cpuMax, overruns = 0 }: { cpu: number; cpuMax: number; overruns?: number } = $props();

  const BUDGET = 80;
  const WARN = 70;
  const pos = (v: number) => Math.min(100, Math.max(0, v));

  let hold = $state(0);
  let holdAt = 0;
  $effect(() => {
    const now = performance.now();
    if (cpuMax >= hold || now - holdAt > 2000) {
      hold = cpuMax;
      holdAt = now;
    }
  });
  const colour = $derived(hold >= BUDGET ? meterStyle.hot : hold >= WARN ? meterStyle.warn : meterStyle.ok);
</script>

<div
  class="meter"
  title="CPU: {cpu.toFixed(0)} % of each audio block's time on average, {hold.toFixed(0)} % at the highest lately. The budget is {BUDGET} %.{overruns ? ` Blocks that came too late so far: ${overruns}.` : ''}"
>
  <span class="label">CPU</span>
  <div class="bar" role="meter" aria-label="CPU load" aria-valuemin="0" aria-valuemax="100" aria-valuenow={Math.round(cpu)}>
    <div class="level" style:width="{pos(cpu)}%" style:background={colour}></div>
    {#if hold > 0}<div class="hold" style:left="{pos(hold)}%"></div>{/if}
    <div class="tick" style:left="{BUDGET}%"></div>
  </div>
  <span class="pct" class:hot={hold >= BUDGET}>{cpu.toFixed(0)}%</span>
</div>

<style>
  .meter {
    display: flex;
    align-items: center;
    gap: 5px;
  }
  .label {
    font-size: 10.5px;
    letter-spacing: 0.08em;
    color: #8a92a0;
  }
  .bar {
    position: relative;
    width: 48px;
    height: 8px;
    border-radius: 4px;
    background: #23272f;
    overflow: hidden;
  }
  .level {
    position: absolute;
    left: 0;
    top: 0;
    bottom: 0;
    border-radius: 4px;
  }
  .hold {
    position: absolute;
    top: 0;
    bottom: 0;
    width: 2px;
    margin-left: -1px;
    background: #e8eaee;
  }
  .tick {
    position: absolute;
    top: 0;
    bottom: 0;
    width: 1px;
    background: #6f7786;
  }
  .pct {
    min-width: 30px;
    text-align: right;
    font-size: 11.5px;
    font-variant-numeric: tabular-nums;
    color: #c3c8d1;
  }
  .pct.hot {
    color: #ff4d4d;
    font-weight: 700;
  }
</style>
