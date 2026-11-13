<script lang="ts">
  // The main screen (APP.md 3.1): the signal chain as blocks, the drums lane, and the selected
  // block's controls. Everything is built from the device's settings table and the block
  // definitions shared with the unit's own UI (protocol/blocks.json).
  import { onMount } from "svelte";
  import { blockActive, blockOn, blockOnValue, buildBlocks, type Block } from "$lib/blocks";
  import Control from "$lib/controls/Control.svelte";
  import Meter from "$lib/controls/Meter.svelte";
  import { connectDevice, device, meters, preview, setParam } from "$lib/device.svelte";
  import { baseName, formatValue } from "$lib/format";

  onMount(() => {
    let stop = () => {};
    connectDevice().then((s) => (stop = s));
    return () => stop();
  });

  const blocks = $derived(buildBlocks(device.settings));
  const all = $derived([...blocks.chain, blocks.lane, ...(blocks.other ? [blocks.other] : [])]);
  // The address can name the block to open ("#reverb"): handy when working on the UI.
  let selectedId = $state((typeof location !== "undefined" && location.hash.slice(1)) || "amp");
  const selected = $derived(all.find((b) => b.def.id === selectedId) ?? all[0]);

  const value = (id: number, fallback: number) => device.values[id] ?? fallback;
  const isOn = (b: Block) => blockOn(b, device.values);
  const toggle = (b: Block) => b.on && setParam(b.on.id, blockOnValue(b, !isOn(b)));

  const fileOf = (b: Block) => (b.def.file === "model" ? device.files.model : b.def.file === "ir" ? device.files.ir : "");
  const stem = (path: string) => baseName(path).replace(/\.(nam|wav)$/i, "").replaceAll("_", " ");
  function summary(b: Block): string {
    if (b.def.file) return stem(fileOf(b)) || (b.def.file === "model" ? "No model" : "No IR");
    return b.summary ? formatValue(b.summary, value(b.summary.id, b.summary.default)) : "";
  }

  const cpuOf = (b: Block): number | null =>
    ({ amp: meters.cpuAmp, doubler: meters.cpuDoubler, delay: meters.cpuDelay, reverb: meters.cpuReverb, drums: meters.cpuDrums })[b.def.id] ?? null;

  // The clip count only grows: "clipped" lights for 2 s after it last went up.
  let clipSeen = 0;
  let clipAt = $state(-1e9);
  let now = $state(0);
  $effect(() => {
    now = performance.now();
    if (meters.clipped > clipSeen) clipAt = now;
    clipSeen = meters.clipped;
  });
  const clipping = $derived(now - clipAt < 2000);

  // Space switches the selected block, unless a control has the keyboard.
  function key(e: KeyboardEvent) {
    const t = e.target as HTMLElement;
    if (e.key !== " " || t.closest("input, select, button, [role=slider]")) return;
    e.preventDefault();
    if (selected) toggle(selected);
  }

  const laneChips = ["bpm", "drum style", "drum kit", "drum song", "bass key"];
  const chip = (key: string) => blocks.lane.sections.flatMap((s) => s.settings).find((s) => s.key === key);
</script>

<svelte:window onkeydown={key} />

<header>
  <div class="brand">VOLFOVICH <b>NAM FORGE</b></div>
  <div class="status" class:on={device.connected}>
    <span class="dot"></span>
    {device.connected ? (preview.on ? "Preview, no unit" : "Connected") : "Not connected"}
  </div>
  {#if device.connected}
    <div class="preset" title="The preset recalled last">
      <span>PRESET</span>
      <b>{device.files.preset || "None"}</b>
      {#if device.files.edited}<i title="Changed since it was recalled">edited</i>{/if}
    </div>
    <div class="meters">
      <Meter label="IN" peak={meters.inputPeak} />
      <Meter label="OUT" peak={meters.outputPeak} clipped={clipping} />
      <div class="cpu" class:warn={meters.cpuMax >= 80} title="Audio processing time as a share of one block: average, and the highest lately">
        <span>CPU</span><b>{meters.cpu.toFixed(0)}%</b>
      </div>
    </div>
  {/if}
</header>

{#if device.connected && selected}
  {#if meters.muted}
    <div class="alert">Overload: the chain could not keep up, so the unit muted its output and is switching effects off.</div>
  {/if}

  <section class="chain" aria-label="Signal chain">
    <div class="jack">IN</div>
    {#each blocks.chain as b (b.def.id)}
      <div class="wire" class:live={meters.inputPeak > 0.003}></div>
      <div class="tile" class:off={!isOn(b)} class:selected={selected.def.id === b.def.id} class:ringing={!isOn(b) && blockActive(b, meters)} style:--c={b.def.colour}>
        <button class="body" onclick={() => (selectedId = b.def.id)} aria-pressed={selected.def.id === b.def.id}>
          <span class="short">{b.def.short}</span>
          <span class="summary">{summary(b)}</span>
          {#if b.def.id === "gate" && isOn(b)}
            <span class="gate" class:open={meters.gateGain > 0.5} title={meters.gateGain > 0.5 ? "Open" : "Closed"}></span>
          {/if}
        </button>
        {#if b.on}
          <button class="power" aria-label="{b.def.name} {isOn(b) ? 'off' : 'on'}" title="Switch {isOn(b) ? 'off' : 'on'}" onclick={() => toggle(b)}>
            <svg viewBox="0 0 16 16"><path d="M8 2v6M4.5 4.6a5 5 0 1 0 7 0" /></svg>
          </button>
        {/if}
      </div>
    {/each}
    <div class="wire" class:live={meters.outputPeak > 0.003}></div>
    <div class="jack">OUT</div>
  </section>

  {@const lane = blocks.lane}
  <section class="lane" class:selected={selected.def.id === lane.def.id} class:playing={isOn(lane)} style:--c={lane.def.colour}>
    <button class="play" aria-label={isOn(lane) ? "Stop the drums" : "Start the drums"} onclick={() => toggle(lane)}>
      {#if isOn(lane)}
        <svg viewBox="0 0 16 16"><rect x="4" y="4" width="8" height="8" rx="1" /></svg>
      {:else}
        <svg viewBox="0 0 16 16"><path d="M5 3.5v9l7.5-4.5z" /></svg>
      {/if}
    </button>
    <button class="body" onclick={() => (selectedId = lane.def.id)} aria-pressed={selected.def.id === lane.def.id}>
      <span class="short">{lane.def.short}</span>
      {#each laneChips as k}
        {@const s = chip(k)}
        {#if s}<span class="chip"><i>{s.label}</i> {formatValue(s, value(s.id, s.default))}</span>{/if}
      {/each}
    </button>
    {#if blocks.other}
      <button class="more" class:selected={selected.def.id === "other"} onclick={() => (selectedId = "other")}>Other settings</button>
    {/if}
  </section>

  <section class="editor" style:--c={selected.def.colour} aria-label="{selected.def.name} settings">
    <div class="head">
      <h2>{selected.def.name}</h2>
      {#if selected.def.file}
        <span class="file" title={fileOf(selected)}>{baseName(fileOf(selected)) || (selected.def.file === "model" ? "No model loaded" : "No IR loaded: the cab passes the sound through")}</span>
      {/if}
      <span class="grow"></span>
      {#if cpuOf(selected) !== null}<span class="stagecpu" title="This block's share of the processing time">{cpuOf(selected)?.toFixed(1)}% CPU</span>{/if}
      {#if selected.on}
        <button class="onoff" class:on={isOn(selected)} onclick={() => toggle(selected)}>
          {selected.def.id === "drums" ? (isOn(selected) ? "Playing" : "Stopped") : isOn(selected) ? "On" : "Off"}
        </button>
      {/if}
    </div>
    {#each selected.sections as sec (sec.title)}
      <div class="section">
        {#if sec.title}<h3>{sec.title}</h3>{/if}
        <div class="controls">
          {#each sec.settings as s (s.id)}
            <Control setting={s} value={value(s.id, s.default)} colour={selected.def.colour} onchange={(v) => setParam(s.id, v)} />
          {/each}
        </div>
      </div>
    {:else}
      <p class="none">This block has no settings of its own.</p>
    {/each}
  </section>

  <footer>
    <span>Firmware {device.info?.firmware}</span>
    <span>{device.info?.blockSize}-sample blocks at {(device.info?.sampleRate ?? 0) / 1000} kHz</span>
    <span>CPU max {meters.cpuMax.toFixed(0)}%</span>
    <span class:bad={meters.overruns > 0}>Overruns {meters.overruns}</span>
    <span class:bad={meters.overloadMutes > 0}>Overload mutes {meters.overloadMutes}</span>
    <span class:bad={meters.clipped > 0}>Clipped samples {meters.clipped}</span>
    <span class:bad={!device.files.card}>SD card {device.files.card ? "in" : "missing"}</span>
  </footer>
{:else}
  <div class="waiting"><p>{device.detail}</p></div>
{/if}

<style>
  :global(html, body) {
    margin: 0;
    height: 100%;
    background: #101216;
    color: #e6e8ec;
    font: 13px/1.4 -apple-system, BlinkMacSystemFont, "Helvetica Neue", sans-serif;
  }
  :global(body > div) {
    display: flex !important;
    flex-direction: column;
    height: 100vh;
  }
  button {
    font: inherit;
    color: inherit;
    cursor: pointer;
  }
  svg {
    display: block;
    fill: none;
    stroke: currentColor;
    stroke-width: 1.8;
    stroke-linecap: round;
    stroke-linejoin: round;
  }

  header {
    display: flex;
    align-items: center;
    gap: 22px;
    padding: 10px 18px;
    background: #181b21;
    border-bottom: 1px solid #2a2e37;
  }
  .brand {
    letter-spacing: 0.18em;
    font-size: 12px;
    color: #8a92a0;
    white-space: nowrap;
  }
  .brand b {
    color: #ffb347;
  }
  .status {
    display: flex;
    align-items: center;
    gap: 7px;
    color: #8a92a0;
    white-space: nowrap;
  }
  .dot {
    width: 8px;
    height: 8px;
    border-radius: 50%;
    background: #6b7280;
  }
  .status.on .dot {
    background: #3ddc84;
    box-shadow: 0 0 8px #3ddc84;
  }
  .preset {
    flex: 1;
    display: flex;
    align-items: baseline;
    justify-content: center;
    gap: 9px;
    min-width: 0;
  }
  .preset span {
    font-size: 10.5px;
    letter-spacing: 0.1em;
    color: #8a92a0;
  }
  .preset b {
    font-size: 17px;
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }
  .preset i {
    font-style: normal;
    font-size: 10.5px;
    letter-spacing: 0.06em;
    text-transform: uppercase;
    color: #ffb347;
    border: 1px solid #6b4d1f;
    border-radius: 4px;
    padding: 0 5px;
  }
  .meters {
    display: flex;
    align-items: center;
    gap: 16px;
  }
  .cpu {
    display: flex;
    align-items: baseline;
    gap: 6px;
  }
  .cpu span {
    font-size: 10.5px;
    letter-spacing: 0.08em;
    color: #8a92a0;
  }
  .cpu b {
    width: 34px;
    font-variant-numeric: tabular-nums;
  }
  .cpu.warn b {
    color: #ff4d4d;
  }
  .alert {
    background: #4a1c1c;
    color: #ffd7d7;
    padding: 8px 18px;
    border-bottom: 1px solid #7a2a2a;
  }

  .chain {
    display: flex;
    align-items: center;
    padding: 26px 18px 18px;
  }
  .jack {
    font-size: 10.5px;
    letter-spacing: 0.1em;
    color: #8a92a0;
    border: 1px solid #353b47;
    border-radius: 12px;
    padding: 3px 9px;
  }
  .wire {
    flex: 1 1 10px;
    min-width: 8px;
    height: 2px;
    background: #353b47;
  }
  .wire.live {
    background: #5a6373;
  }
  .tile {
    position: relative;
    flex: 0 1 138px;
    min-width: 92px;
    height: 84px;
    border-radius: 12px;
    background: linear-gradient(180deg, color-mix(in srgb, var(--c) 26%, #1c1f26), color-mix(in srgb, var(--c) 12%, #181b21));
    border: 1px solid color-mix(in srgb, var(--c) 55%, #2a2e37);
    transition: opacity 0.12s, box-shadow 0.12s;
  }
  .tile.selected {
    box-shadow: 0 0 0 2px var(--c), 0 6px 22px color-mix(in srgb, var(--c) 30%, transparent);
  }
  .tile.off {
    background: #181b21;
    border-color: #2f343e;
  }
  .tile.off .body {
    opacity: 0.5;
  }
  .tile.ringing {
    animation: ring 1.1s ease-in-out infinite;
  }
  @keyframes ring {
    50% {
      border-color: var(--c);
    }
  }
  .tile .body {
    width: 100%;
    height: 100%;
    background: none;
    border: 0;
    border-radius: 12px;
    padding: 12px 12px 10px;
    display: flex;
    flex-direction: column;
    justify-content: space-between;
    align-items: flex-start;
    text-align: left;
    min-width: 0;
  }
  .short {
    font-size: 13px;
    font-weight: 700;
    letter-spacing: 0.1em;
    color: color-mix(in srgb, var(--c) 70%, #fff);
  }
  .tile.off .short {
    color: #aab2bf;
  }
  .summary {
    width: 100%;
    font-size: 11.5px;
    color: #c3c8d1;
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }
  .gate {
    position: absolute;
    right: 12px;
    bottom: 14px;
    width: 7px;
    height: 7px;
    border-radius: 50%;
    background: #4a505c;
  }
  .gate.open {
    background: #3ddc84;
    box-shadow: 0 0 6px #3ddc84;
  }
  .power {
    position: absolute;
    top: 7px;
    right: 7px;
    width: 26px;
    height: 26px;
    padding: 5px;
    border-radius: 50%;
    border: 0;
    background: color-mix(in srgb, var(--c) 30%, #22262e);
    color: #fff;
  }
  .tile.off .power {
    background: #262a33;
    color: #7c8493;
  }
  button:focus-visible {
    outline: 2px solid color-mix(in srgb, var(--c, #ffb347) 70%, #fff);
    outline-offset: 2px;
  }

  .lane {
    display: flex;
    align-items: center;
    gap: 12px;
    margin: 0 18px;
    padding: 8px 12px;
    border-radius: 12px;
    background: #181b21;
    border: 1px solid #2a2e37;
  }
  .lane.selected {
    border-color: var(--c);
  }
  .play {
    width: 32px;
    height: 32px;
    padding: 8px;
    border-radius: 50%;
    border: 1px solid color-mix(in srgb, var(--c) 60%, #2a2e37);
    background: color-mix(in srgb, var(--c) 16%, #181b21);
    color: var(--c);
  }
  .play svg {
    fill: currentColor;
    stroke: none;
  }
  .lane.playing .play {
    background: var(--c);
    color: #101216;
  }
  .lane .body {
    flex: 1;
    display: flex;
    align-items: center;
    gap: 10px;
    background: none;
    border: 0;
    padding: 4px 0;
    min-width: 0;
    overflow: hidden;
  }
  .lane .short {
    margin-right: 6px;
    white-space: nowrap;
  }
  .chip {
    background: #22262e;
    border-radius: 6px;
    padding: 2px 8px;
    white-space: nowrap;
    font-variant-numeric: tabular-nums;
  }
  .chip i {
    font-style: normal;
    font-size: 10.5px;
    letter-spacing: 0.06em;
    text-transform: uppercase;
    color: #8a92a0;
    margin-right: 3px;
  }
  .more {
    background: #22262e;
    border: 1px solid #353b47;
    border-radius: 7px;
    padding: 4px 10px;
  }
  .more.selected {
    border-color: #aab2bf;
  }

  .editor {
    flex: 1;
    min-height: 0;
    overflow-y: auto;
    margin: 16px 18px 14px;
    padding: 14px 18px 18px;
    border-radius: 12px;
    background: #181b21;
    border: 1px solid #2a2e37;
    border-top: 3px solid var(--c);
  }
  .head {
    display: flex;
    align-items: center;
    gap: 14px;
    margin-bottom: 10px;
    min-width: 0;
  }
  h2 {
    margin: 0;
    font-size: 15px;
    letter-spacing: 0.06em;
    text-transform: uppercase;
    color: color-mix(in srgb, var(--c) 70%, #fff);
    white-space: nowrap;
  }
  .file {
    color: #c3c8d1;
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }
  .grow {
    flex: 1;
  }
  .stagecpu {
    color: #8a92a0;
    font-variant-numeric: tabular-nums;
    white-space: nowrap;
  }
  .onoff {
    min-width: 62px;
    padding: 4px 12px;
    border-radius: 14px;
    border: 1px solid #353b47;
    background: #22262e;
    color: #aab2bf;
  }
  .onoff.on {
    border-color: var(--c);
    background: color-mix(in srgb, var(--c) 26%, #22262e);
    color: #fff;
  }
  .section + .section {
    margin-top: 14px;
    padding-top: 12px;
    border-top: 1px solid #262a33;
  }
  h3 {
    margin: 0 0 8px;
    font-size: 10.5px;
    font-weight: 600;
    letter-spacing: 0.1em;
    text-transform: uppercase;
    color: #8a92a0;
  }
  .controls {
    display: flex;
    flex-wrap: wrap;
    align-items: flex-end;
    gap: 14px 18px;
  }
  .none {
    margin: 6px 0 0;
    color: #8a92a0;
  }

  footer {
    display: flex;
    flex-wrap: wrap;
    gap: 4px 20px;
    padding: 7px 18px;
    border-top: 1px solid #2a2e37;
    background: #14171c;
    font-size: 11.5px;
    color: #8a92a0;
    font-variant-numeric: tabular-nums;
  }
  footer .bad {
    color: #ffb347;
  }
  .waiting {
    flex: 1;
    display: grid;
    place-items: center;
    color: #8a92a0;
    font-size: 15px;
    text-align: center;
    padding: 0 40px;
  }
</style>
