<script lang="ts">
  // Milestone A1 of APP.md: a plain window that proves the link in both directions. Every
  // setting from the device's own table is listed with a control built from its range and
  // unit. The Quad Cortex style chain view replaces this at A2.
  import { onMount } from "svelte";
  import { device, connectDevice, setParam } from "$lib/device.svelte";
  import { formatValue, baseName } from "$lib/format";
  import { Unit, type Setting } from "$lib/types";

  onMount(() => {
    let stop = () => {};
    connectDevice().then((s) => (stop = s));
    return () => stop();
  });

  // Group the flat table by the first word of each setting's name ("Delay mix" -> "Delay").
  const groups = $derived.by(() => {
    const out: { title: string; settings: Setting[] }[] = [];
    for (const s of device.settings) {
      const title = s.name.split(" ")[0];
      const last = out[out.length - 1];
      if (last && last.title === title) last.settings.push(s);
      else out.push({ title, settings: [s] });
    }
    return out;
  });
</script>

<header>
  <div class="brand">VOLFOVICH <b>NAM FORGE</b></div>
  <div class="status" class:on={device.connected}>
    <span class="dot"></span>
    {#if device.connected && device.info}
      Connected · firmware {device.info.firmware} · {device.info.blockSize}-sample blocks at {device.info.sampleRate / 1000} kHz
    {:else}
      {device.detail}
    {/if}
  </div>
</header>

{#if device.connected}
  <section class="files">
    <div><span>Preset</span><b>{device.files.preset || "none"}{device.files.edited ? " •" : ""}</b></div>
    <div><span>Amp model</span><b>{baseName(device.files.model) || "none"}</b></div>
    <div><span>Cab IR</span><b>{baseName(device.files.ir) || "none"}</b></div>
    <div><span>SD card</span><b>{device.files.card ? "present" : "missing"}</b></div>
  </section>

  <main>
    {#each groups as g (g.title + g.settings[0].id)}
      <section class="group">
        <h2>{g.title}</h2>
        {#each g.settings as s (s.id)}
          {@const v = device.values[s.id] ?? s.default}
          <label class="row">
            <span class="name" title={s.key}>{s.name}</span>
            {#if s.unit === Unit.OnOff}
              <input type="checkbox" checked={v !== 0} onchange={(e) => setParam(s.id, e.currentTarget.checked ? 1 : 0)} />
            {:else if s.choices.length}
              <select value={v} onchange={(e) => setParam(s.id, Number(e.currentTarget.value))}>
                {#each s.choices as c, i}<option value={i}>{c}</option>{/each}
              </select>
            {:else}
              <input type="range" min={s.min} max={s.max} step="1" value={v} oninput={(e) => setParam(s.id, Number(e.currentTarget.value))} />
            {/if}
            <span class="value">{formatValue(s, v)}</span>
          </label>
        {/each}
      </section>
    {/each}
  </main>
{:else}
  <div class="waiting">
    <p>{device.detail}</p>
  </div>
{/if}

<style>
  :global(html, body) {
    margin: 0;
    height: 100%;
    background: #121418;
    color: #e6e8ec;
    font: 13px/1.4 -apple-system, BlinkMacSystemFont, "Helvetica Neue", sans-serif;
  }
  header {
    display: flex;
    align-items: center;
    justify-content: space-between;
    padding: 12px 18px;
    background: #1a1d23;
    border-bottom: 1px solid #2a2e37;
  }
  .brand {
    letter-spacing: 0.18em;
    font-size: 12px;
    color: #9aa1ad;
  }
  .brand b {
    color: #ffb347;
  }
  .status {
    display: flex;
    align-items: center;
    gap: 8px;
    color: #9aa1ad;
  }
  .dot {
    width: 9px;
    height: 9px;
    border-radius: 50%;
    background: #6b7280;
  }
  .status.on .dot {
    background: #3ddc84;
    box-shadow: 0 0 8px #3ddc84;
  }
  .files {
    display: grid;
    grid-template-columns: repeat(4, 1fr);
    gap: 1px;
    background: #2a2e37;
    border-bottom: 1px solid #2a2e37;
  }
  .files div {
    background: #171a20;
    padding: 10px 18px;
    min-width: 0;
  }
  .files span {
    display: block;
    font-size: 11px;
    text-transform: uppercase;
    letter-spacing: 0.08em;
    color: #7c8493;
  }
  .files b {
    display: block;
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
    font-weight: 600;
  }
  main {
    display: grid;
    grid-template-columns: repeat(auto-fill, minmax(340px, 1fr));
    gap: 14px;
    padding: 16px 18px 28px;
  }
  .group {
    background: #1a1d23;
    border: 1px solid #2a2e37;
    border-radius: 10px;
    padding: 10px 14px 12px;
  }
  h2 {
    margin: 0 0 6px;
    font-size: 12px;
    text-transform: uppercase;
    letter-spacing: 0.12em;
    color: #ffb347;
  }
  .row {
    display: grid;
    grid-template-columns: 130px 1fr 78px;
    align-items: center;
    gap: 10px;
    padding: 3px 0;
  }
  .name {
    color: #c3c8d1;
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }
  .value {
    text-align: right;
    font-variant-numeric: tabular-nums;
    color: #e6e8ec;
  }
  input[type="range"] {
    width: 100%;
    accent-color: #ffb347;
  }
  input[type="checkbox"] {
    justify-self: start;
    accent-color: #ffb347;
  }
  select {
    background: #121418;
    color: inherit;
    border: 1px solid #2a2e37;
    border-radius: 6px;
    padding: 3px 6px;
  }
  .waiting {
    display: grid;
    place-items: center;
    height: 70vh;
    color: #9aa1ad;
    font-size: 15px;
    text-align: center;
    padding: 0 40px;
  }
</style>
