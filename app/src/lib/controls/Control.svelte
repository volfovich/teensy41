<script lang="ts">
  // The right control for a setting, chosen from its row in the device's table: a switch, a
  // set of buttons or a list for named choices, a stepper for numbered things, a knob for the rest.
  import { formatValue } from "$lib/format";
  import { Unit, type Setting } from "$lib/types";
  import Knob from "./Knob.svelte";

  // inline: the label beside the control instead of under it, without the height of a dial
  // (for a type or mode selector on its own line).
  let { setting, value, colour, onchange, inline = false }: { setting: Setting; value: number; colour: string; onchange: (v: number) => void; inline?: boolean } = $props();

  const kind = $derived(
    setting.unit === Unit.OnOff ? "switch"
    : setting.choices.length ? (setting.choices.length <= 5 ? "buttons" : "list")
    : setting.unit === Unit.Name || setting.unit === Unit.None ? "stepper"
    : "knob",
  );
  const step = (d: number) => onchange(Math.min(setting.max, Math.max(setting.min, value + d)));
</script>

{#if kind === "knob"}
  <Knob {setting} {value} {colour} {onchange} />
{:else}
  <div class="field" class:inline style:--c={colour}>
    {#if kind === "switch"}
      <button class="switch" class:on={value !== 0} role="switch" aria-checked={value !== 0} aria-label={setting.name} onclick={() => onchange(value ? 0 : 1)}>
        <span></span>
      </button>
    {:else if kind === "buttons"}
      <div class="buttons" role="radiogroup" aria-label={setting.name}>
        {#each setting.choices as c, i}
          <button class:on={value === i} role="radio" aria-checked={value === i} onclick={() => onchange(i)}>{c}</button>
        {/each}
      </div>
    {:else if kind === "list"}
      <select aria-label={setting.name} {value} onchange={(e) => onchange(Number(e.currentTarget.value))}>
        {#each setting.choices as c, i}<option value={i}>{c}</option>{/each}
      </select>
    {:else}
      <div class="stepper">
        <button aria-label="{setting.name} down" disabled={value <= setting.min} onclick={() => step(-1)}>‹</button>
        <b>{formatValue(setting, value)}</b>
        <button aria-label="{setting.name} up" disabled={value >= setting.max} onclick={() => step(1)}>›</button>
      </div>
    {/if}
    <div class="label">{setting.label}</div>
  </div>
{/if}

<style>
  .field {
    display: flex;
    flex-direction: column;
    align-items: center;
    justify-content: flex-end;
    gap: 6px;
    min-width: 84px;
    /* No height of its own: the row lines the labels up along its bottom, so a field stands
       level with a dial's label beside it, and a row without dials stays low. */
  }
  .field.inline {
    flex-direction: row-reverse;
    justify-content: flex-end;
    align-items: center;
    gap: 10px;
    min-width: 0;
    min-height: 0;
  }
  .field.inline > :not(.label) {
    margin-bottom: 0;
  }
  .label {
    font-size: 10.5px;
    letter-spacing: 0.08em;
    text-transform: uppercase;
    color: #8a92a0;
    white-space: nowrap;
  }
  button,
  select {
    font: inherit;
    color: inherit;
    cursor: pointer;
  }
  .switch {
    width: 44px;
    height: 24px;
    margin-bottom: 8px;
    border-radius: 12px;
    border: 1px solid #353b47;
    background: #22262e;
    padding: 0;
    position: relative;
  }
  .switch span {
    position: absolute;
    top: 2px;
    left: 2px;
    width: 18px;
    height: 18px;
    border-radius: 50%;
    background: #6b7280;
    transition: left 0.12s, background 0.12s;
  }
  .switch.on {
    background: color-mix(in srgb, var(--c) 30%, #22262e);
    border-color: var(--c);
  }
  .switch.on span {
    left: 22px;
    background: var(--c);
  }
  .buttons {
    display: flex;
    margin-bottom: 6px;
    border: 1px solid #353b47;
    border-radius: 7px;
    overflow: hidden;
  }
  .buttons button {
    background: #22262e;
    border: 0;
    border-left: 1px solid #353b47;
    padding: 5px 10px;
    font-size: 12px;
    white-space: nowrap;
    color: #aab2bf;
  }
  .buttons button:first-child {
    border-left: 0;
  }
  .buttons button.on {
    background: color-mix(in srgb, var(--c) 28%, #22262e);
    color: #fff;
  }
  select {
    margin-bottom: 6px;
    background: #22262e;
    border: 1px solid #353b47;
    border-radius: 7px;
    padding: 5px 8px;
  }
  .stepper {
    display: flex;
    align-items: center;
    margin-bottom: 6px;
    border: 1px solid #353b47;
    border-radius: 7px;
    background: #22262e;
  }
  .stepper button {
    background: none;
    border: 0;
    width: 26px;
    height: 28px;
    font-size: 17px;
    line-height: 1;
    color: #aab2bf;
  }
  .stepper button:disabled {
    opacity: 0.3;
    cursor: default;
  }
  .stepper b {
    min-width: 38px;
    text-align: center;
    font-variant-numeric: tabular-nums;
  }
  button:focus-visible,
  select:focus-visible {
    outline: 2px solid color-mix(in srgb, var(--c) 60%, transparent);
    outline-offset: 1px;
  }
</style>
