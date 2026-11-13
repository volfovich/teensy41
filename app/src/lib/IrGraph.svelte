<script lang="ts">
  // Two pictures of the loaded IR as the unit uses it: the waveform over time and the
  // frequency response (20 Hz to 20 kHz, logarithmic), both computed by the app's core.
  let { wave, response, ms, colour }: { wave: [number, number][]; response: number[]; ms: number; colour: string } = $props();

  const W = 360;
  const H = 120;
  const PAD = 4;

  const wavePath = $derived.by(() => {
    if (!wave.length) return "";
    const x = (c: number) => (c / Math.max(1, wave.length - 1)) * W;
    const y = (v: number) => H / 2 - v * (H / 2 - PAD);
    const top = wave.map(([, hi], c) => `${c ? "L" : "M"}${x(c).toFixed(1)} ${y(hi).toFixed(1)}`).join("");
    const bottom = wave.map((_, i) => `L${x(wave.length - 1 - i).toFixed(1)} ${y(wave[wave.length - 1 - i][0]).toFixed(1)}`).join("");
    return `${top}${bottom}Z`;
  });

  // The response's scale: the top is the next 6 dB line above the peak, 48 dB are shown.
  const SPAN = 48;
  const top = $derived(response.length ? Math.ceil((Math.max(...response) + 1) / 6) * 6 : 0);
  const fx = (hz: number) => (Math.log(hz / 20) / Math.log(1000)) * W;
  const dy = (db: number) => PAD + ((top - db) / SPAN) * (H - 2 * PAD);
  const responsePath = $derived(
    response.map((db, i) => `${i ? "L" : "M"}${((i / (response.length - 1)) * W).toFixed(1)} ${Math.min(H, dy(db)).toFixed(1)}`).join(""),
  );
  const hzLines = [50, 100, 200, 500, 1000, 2000, 5000, 10000];
  const hzLabels: [number, string][] = [[100, "100"], [1000, "1k"], [10000, "10k"]];
  const dbLines = $derived([0, 1, 2, 3].map((i) => top - 12 * i));
</script>

<div class="graphs" style:--c={colour}>
  <figure>
    <svg viewBox="0 0 {W} {H}" preserveAspectRatio="none" role="img" aria-label="The IR's waveform">
      <line class="grid" x1="0" y1={H / 2} x2={W} y2={H / 2} />
      <path class="fill" d={wavePath} />
    </svg>
    <figcaption><span>Waveform</span><span>0 to {ms.toFixed(0)} ms</span></figcaption>
  </figure>
  <figure>
    <div class="plot">
      <svg viewBox="0 0 {W} {H}" preserveAspectRatio="none" role="img" aria-label="The IR's frequency response">
        {#each hzLines as hz}<line class="grid" x1={fx(hz)} y1="0" x2={fx(hz)} y2={H} />{/each}
        {#each dbLines as db}<line class="grid" x1="0" y1={dy(db)} x2={W} y2={dy(db)} />{/each}
        <path class="curve" d={responsePath} />
      </svg>
      {#each dbLines as db}<span class="db" style:top="{(dy(db) / H) * 100}%">{db > 0 ? "+" : ""}{db}</span>{/each}
      {#each hzLabels as [hz, text]}<span class="hz" style:left="{(fx(hz) / W) * 100}%">{text}</span>{/each}
    </div>
    <figcaption><span>Frequency response</span><span>dB, 20 Hz to 20 kHz</span></figcaption>
  </figure>
</div>

<style>
  .graphs {
    display: flex;
    gap: 14px;
    flex-wrap: wrap;
  }
  figure {
    margin: 0;
    flex: 1 1 260px;
    max-width: 440px;
    min-width: 0;
  }
  .plot {
    position: relative;
  }
  svg {
    display: block;
    width: 100%;
    height: 120px;
    background: #14171c;
    border: 1px solid #2a2e37;
    border-radius: 8px;
  }
  .grid {
    stroke: #262a33;
    stroke-width: 1;
    vector-effect: non-scaling-stroke;
  }
  .fill {
    fill: color-mix(in srgb, var(--c) 75%, #fff);
    opacity: 0.9;
  }
  .curve {
    fill: none;
    stroke: color-mix(in srgb, var(--c) 80%, #fff);
    stroke-width: 1.6;
    stroke-linejoin: round;
    vector-effect: non-scaling-stroke;
  }
  .db,
  .hz {
    position: absolute;
    font-size: 9.5px;
    color: #6f7786;
    pointer-events: none;
  }
  .db {
    right: 5px;
    transform: translateY(1px);
  }
  .hz {
    bottom: 3px;
    transform: translateX(3px);
  }
  figcaption {
    display: flex;
    justify-content: space-between;
    margin-top: 5px;
    font-size: 10.5px;
    letter-spacing: 0.06em;
    color: #8a92a0;
  }
  figcaption span:first-child {
    text-transform: uppercase;
  }
</style>
