<script lang="ts">
  // A rotary control for one setting. Drag up and down (Shift for fine steps), scroll, or use
  // the arrow keys; double-click to type a value; right-click for the default.
  import { formatValue, parseTyped, typedValue } from "$lib/format";
  import type { Setting } from "$lib/types";

  let { setting, value, colour = "#ffb347", onchange }: { setting: Setting; value: number; colour?: string; onchange: (v: number) => void } = $props();

  const SWEEP = 270; // degrees, from 7:30 round to 4:30
  const DRAG_PIXELS = 180; // a full turn of the range

  const span = $derived(Math.max(1, setting.max - setting.min));
  const frac = $derived(Math.min(1, Math.max(0, (value - setting.min) / span)));
  // A setting that goes below and above zero is drawn from its centre (tone controls, gains).
  const bipolar = $derived(setting.min < 0 && setting.max > 0);
  const zeroFrac = $derived(bipolar ? -setting.min / span : 0);

  const R = 25;
  const point = (f: number) => {
    const a = ((-135 + f * SWEEP) * Math.PI) / 180;
    return [32 + R * Math.sin(a), 32 - R * Math.cos(a)];
  };
  const arc = (from: number, to: number) => {
    const [a, b] = from <= to ? [from, to] : [to, from];
    if (b - a < 0.001) return "";
    const [x0, y0] = point(a);
    const [x1, y1] = point(b);
    return `M${x0.toFixed(2)} ${y0.toFixed(2)} A${R} ${R} 0 ${(b - a) * SWEEP > 180 ? 1 : 0} 1 ${x1.toFixed(2)} ${y1.toFixed(2)}`;
  };

  const set = (v: number) => {
    const c = Math.min(setting.max, Math.max(setting.min, Math.round(v)));
    if (c !== value) onchange(c);
  };

  let dragging = $state(false);
  let startY = 0;
  let startValue = 0;
  let acc = 0;

  function down(e: PointerEvent) {
    if (e.button !== 0 || typing) return;
    (e.currentTarget as HTMLElement).setPointerCapture(e.pointerId);
    dragging = true;
    startY = e.clientY;
    startValue = value;
    acc = 0;
  }
  function move(e: PointerEvent) {
    if (!dragging) return;
    // Accumulated, so that pressing Shift half way does not make the value jump.
    acc += ((startY - e.clientY) * span) / DRAG_PIXELS / (e.shiftKey ? 8 : 1);
    startY = e.clientY;
    set(startValue + acc);
  }
  function up() {
    dragging = false;
  }
  function wheel(e: WheelEvent) {
    e.preventDefault();
    set(value + (e.deltaY < 0 ? 1 : -1) * (e.shiftKey ? 1 : setting.step || 1));
  }
  function key(e: KeyboardEvent) {
    const step = e.shiftKey ? 1 : setting.step || 1;
    if (e.key === "ArrowUp" || e.key === "ArrowRight") set(value + step);
    else if (e.key === "ArrowDown" || e.key === "ArrowLeft") set(value - step);
    else if (e.key === "Enter") startTyping();
    else return;
    e.preventDefault();
  }

  let typing = $state(false);
  let text = $state("");
  function startTyping() {
    text = typedValue(setting, value);
    typing = true;
  }
  function commit() {
    if (!typing) return;
    typing = false;
    const v = parseTyped(setting, text);
    if (v !== null) set(v);
  }
  const focusSelect = (el: HTMLInputElement) => {
    el.focus();
    el.select();
  };
</script>

<div class="knob" class:dragging style:--c={colour}>
  <div
    class="dial"
    role="slider"
    tabindex="0"
    aria-label={setting.name}
    aria-valuemin={setting.min}
    aria-valuemax={setting.max}
    aria-valuenow={value}
    aria-valuetext={formatValue(setting, value)}
    title="{setting.name}: drag, scroll or arrow keys. Shift = fine, double-click = type, right-click = default"
    onpointerdown={down}
    onpointermove={move}
    onpointerup={up}
    onpointercancel={up}
    onwheel={wheel}
    onkeydown={key}
    ondblclick={startTyping}
    oncontextmenu={(e) => {
      e.preventDefault();
      set(setting.default);
    }}
  >
    <svg viewBox="0 0 64 64" aria-hidden="true">
      <path class="track" d={arc(0, 1)} />
      <path class="fill" d={arc(zeroFrac, frac)} />
      <circle class="cap" cx="32" cy="32" r="17.5" />
      <line class="pointer" x1="32" y1="19" x2="32" y2="26" transform="rotate({-135 + frac * SWEEP} 32 32)" />
    </svg>
  </div>
  {#if typing}
    <input class="typed" bind:value={text} use:focusSelect onblur={commit} onkeydown={(e) => (e.key === "Enter" ? commit() : e.key === "Escape" ? (typing = false) : null)} />
  {:else}
    <div class="value">{formatValue(setting, value)}</div>
  {/if}
  <div class="label">{setting.label}</div>
</div>

<style>
  .knob {
    display: flex;
    flex-direction: column;
    align-items: center;
    width: 84px;
    user-select: none;
    -webkit-user-select: none;
  }
  .dial {
    width: 64px;
    height: 64px;
    cursor: ns-resize;
    border-radius: 50%;
    outline: none;
    touch-action: none;
  }
  .dial:focus-visible {
    box-shadow: 0 0 0 2px color-mix(in srgb, var(--c) 55%, transparent);
  }
  svg {
    display: block;
    width: 100%;
    height: 100%;
  }
  path {
    fill: none;
    stroke-width: 4.5;
    stroke-linecap: round;
  }
  .track {
    stroke: #2b303a;
  }
  .fill {
    stroke: var(--c);
  }
  .cap {
    fill: #22262e;
    stroke: #353b47;
    stroke-width: 1;
  }
  .dragging .cap {
    fill: #2a2f39;
  }
  .pointer {
    stroke: #e6e8ec;
    stroke-width: 2.5;
    stroke-linecap: round;
  }
  .value,
  .typed {
    margin-top: 4px;
    height: 18px;
    font-size: 12.5px;
    font-weight: 600;
    font-variant-numeric: tabular-nums;
    color: #e6e8ec;
    white-space: nowrap;
  }
  .typed {
    width: 62px;
    text-align: center;
    background: #101216;
    border: 1px solid var(--c);
    border-radius: 4px;
    outline: none;
    font-family: inherit;
    padding: 0;
  }
  .label {
    font-size: 10.5px;
    letter-spacing: 0.08em;
    text-transform: uppercase;
    color: #8a92a0;
    white-space: nowrap;
  }
</style>
