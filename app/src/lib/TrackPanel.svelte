<script lang="ts">
  // The backing-track player's panel, shown in the place of a block's editor: the bar for the
  // whole track with the loop on it, play and stop, the loop's marks and the loop in bars,
  // the tempo, the track's own tempo and beats, the volume and what the file is.
  // It offers what the unit's touch screen offers (docs/SCREEN.md) and what there is only in
  // the shell: a typed tempo, beats in a bar, snapping, a new scan, forgetting the loop.
  import Control from "$lib/controls/Control.svelte";
  import { device, say, setParam } from "$lib/device.svelte";
  import { barBeat, beatSeconds, clock, Cmd, openTrack, playhead, playPause, seek, setTempo, trackCommand, trackTitle, TRACK_COLOUR } from "$lib/track.svelte";
  import { Unit, type Setting } from "$lib/types";

  let { volume, onchoose }: { volume: Setting | null; onchoose: () => void } = $props();

  const t = $derived(device.track);
  const open = $derived(!!t?.path);
  const place = $derived(playhead.place);
  const pct = (seconds: number) => (t && t.length > 0 ? (100 * Math.min(Math.max(seconds, 0), t.length)) / t.length : 0);

  // ---- The bar: click or drag to go there
  let width = $state(0);
  let dragging = $state(false);
  const timeAt = (e: PointerEvent) => {
    const box = (e.currentTarget as HTMLElement).getBoundingClientRect();
    return (Math.min(Math.max((e.clientX - box.left) / box.width, 0), 1)) * (t?.length ?? 0);
  };
  function down(e: PointerEvent) {
    if (e.button !== 0 || !open) return;
    (e.currentTarget as HTMLElement).setPointerCapture(e.pointerId);
    dragging = true;
    seek(timeAt(e));
  }
  function move(e: PointerEvent) {
    if (dragging) seek(timeAt(e));
  }
  function barKey(e: KeyboardEvent) {
    const step = e.shiftKey ? 1 : 5;
    if (e.key === "ArrowLeft") seek(place - step);
    else if (e.key === "ArrowRight") seek(place + step);
    else if (e.key === "Home") seek(0);
    else return;
    e.preventDefault();
    e.stopPropagation();
  }
  // Bar lines, once the tempo is known: every bar, or every 2nd, 4th … so that they stay apart.
  const barLines = $derived.by(() => {
    if (!t || !width || t.length <= 0) return [];
    const bar = beatSeconds(t) * Math.max(1, t.beatsPerBar);
    if (!bar) return [];
    let every = 1;
    while ((width * bar * every) / t.length < 7 && every < 64) every *= 2;
    if ((width * bar * every) / t.length < 7) return [];
    const out: number[] = [];
    for (let s = t.firstBeat; s < t.length && out.length < 400; s += bar * every) out.push(pct(s));
    return out;
  });

  const where = $derived(t ? barBeat(t, place) : null);
  const stateText = $derived(!t || t.playing === 0 ? "No track" : t.playing === 2 ? "Playing" : t.playing === 3 ? "Paused" : "Stopped");
  const facts = $derived.by(() => {
    if (!t || !t.fileKind) return "";
    const kind = t.fileKind === 1 ? "WAV" : t.fileKind === 2 ? "MP3" : "File";
    const ch = t.channels === 1 ? "mono" : t.channels === 2 ? "stereo" : `${t.channels} channels`;
    return [kind, `${t.fileRate / 1000} kHz`, ch, t.kbps ? `${t.kbps} kbit/s` : ""].filter(Boolean).join(" · ");
  });

  // ---- The loop in bars. Every change sets the loop at once, so it is heard while it is moved.
  const per = $derived(Math.max(1, t?.beatsPerBar ?? 4));
  const LENGTHS = [1, 2, 4, 8, 16, 32];
  // A loop that is set shows as its nearest beat and its length in bars.
  const fromLoop = $derived.by(() => {
    const beat = t ? beatSeconds(t) : 0;
    if (!t || !beat || t.loop === 0 || t.loop === 3) return null;
    const k = Math.max(0, Math.round((t.loopFrom - t.firstBeat) / beat));
    return { bar: Math.floor(k / per) + 1, beat: (k % per) + 1, bars: Math.max(1, Math.round((t.loopTo - t.loopFrom) / (beat * per))) };
  });
  // Without a loop: four bars from the bar the track is in, and nothing is set until a value is touched.
  let draft = $state<{ bar: number; beat: number; bars: number } | null>(null);
  let draftFor = "";
  $effect(() => {
    const path = t?.path ?? "";
    if (path !== draftFor) {
      draftFor = path;
      draft = null;
    }
  });
  const bars = $derived(fromLoop ?? draft ?? { bar: where?.bar ?? 1, beat: 1, bars: 4 });
  function loopBars(next: { bar: number; beat: number; bars: number }) {
    draft = next;
    trackCommand(Cmd.LoopBars, next.bar, next.beat, next.bars);
  }
  function moveStart(beats: number) {
    const k = Math.max(0, (bars.bar - 1) * per + (bars.beat - 1) + beats);
    loopBars({ bar: Math.floor(k / per) + 1, beat: (k % per) + 1, bars: bars.bars });
  }
  const barsEnd = $derived.by(() => {
    const k = (bars.bar - 1) * per + (bars.beat - 1) + bars.bars * per;
    return `${Math.floor(k / per) + 1}.${(k % per) + 1}`;
  });

  // ---- The track's own tempo
  let bpmText = $state("");
  function typeBpm() {
    const bpm = parseFloat(bpmText.replace(",", "."));
    if (!Number.isFinite(bpm) || bpm < 30 || bpm > 300) return say("The tempo is 30 to 300 BPM.");
    trackCommand(Cmd.OwnTempo, bpm);
    bpmText = "";
  }
  const scanning = $derived(!!t && t.scan >= 0 && t.scan < 100);
  // Stand-ins for settings, so that the usual controls can be used for values that are not in the unit's table.
  const made = (name: string, label: string, min: number, max: number, unit: number): Setting => ({ id: -1, key: "", name, label, min, max, default: min, step: 1, unit, inPreset: false, choices: [] });
  const snapSetting = made("Loop points go to the nearest beat", "Snap to beats", 0, 1, Unit.OnOff);
  const beatsSetting = made("Beats in a bar", "Beats in a bar", 1, 16, Unit.None);
</script>

<section class="editor" style:--c={TRACK_COLOUR} aria-label="Backing track">
  <div class="head">
    <h2>Backing track</h2>
    <span class="file" title={open && t ? `/TRACKS/${t.path}` : ""}>{open && t ? trackTitle(t.path) : "No track open"}</span>
    <button class="plain" onclick={onchoose}>Choose…</button>
    {#if open}<button class="plain" title="Close the track" onclick={() => openTrack("")}>Close</button>{/if}
    <span class="grow"></span>
    {#if facts}<span class="quiet">{facts}</span>{/if}
    {#if t && t.gaps > 0}
      <span class="quiet bad" title="Blocks of silence because the unit could not read or stretch the track fast enough, since it was switched on">{t.gaps} {t.gaps === 1 ? "gap" : "gaps"}</span>
    {/if}
    <button class="onoff" class:on={t?.playing === 2} disabled={!open} onclick={playPause}>{stateText}</button>
  </div>

  {#if open && t}
    <!-- svelte-ignore a11y_no_noninteractive_tabindex -->
    <div
      class="timeline"
      class:dragging
      role="slider"
      tabindex="0"
      aria-label="Place in the track"
      aria-valuemin="0"
      aria-valuemax={Math.round(t.length)}
      aria-valuenow={Math.round(place)}
      aria-valuetext="{clock(place)} of {clock(t.length)}"
      title="Click or drag to go there; the arrow keys move 5 s (1 s with Shift)"
      bind:clientWidth={width}
      onpointerdown={down}
      onpointermove={move}
      onpointerup={() => (dragging = false)}
      onpointercancel={() => (dragging = false)}
      onkeydown={barKey}
    >
      <div class="played" style:width="{pct(place)}%"></div>
      {#each barLines as x}<i style:left="{x}%"></i>{/each}
      {#if t.loop === 1 || t.loop === 2}
        <div class="loop" class:off={t.loop === 1} style:left="{pct(t.loopFrom)}%" style:width="{pct(t.loopTo) - pct(t.loopFrom)}%"></div>
      {:else if t.loop === 3}
        <div class="loop mark" style:left="{pct(t.loopFrom)}%"></div>
      {/if}
      <div class="cursor" style:left="{pct(place)}%"></div>
    </div>
    <div class="readout">
      <b>{clock(place, true)}</b>
      <span>of {clock(t.length)}</span>
      {#if where}<span class="bb" title="Bar and beat, counted from the first beat the scan found">bar <b>{where.bar}.{where.beat}</b></span>{/if}
      {#if t.loop === 1 || t.loop === 2}
        <span class="quiet">loop {clock(t.loopFrom, true)} to {clock(t.loopTo, true)}{t.loop === 1 ? " (off)" : ""}</span>
      {:else if t.loop === 3}
        <span class="quiet">loop from {clock(t.loopFrom, true)}: mark its end with B</span>
      {/if}
    </div>

    <div class="row transport">
      <button class="wide" class:lit={t.playing === 2} onclick={playPause}>{t.playing === 2 ? "Pause" : "Play"}</button>
      <button title="Back to the start, or to the loop's start while a loop is on" onclick={() => trackCommand(Cmd.Stop)}>Stop</button>
      <span class="gap"></span>
      <button class:lit={t.loop !== 0} title="Mark the loop's start where the track is now{t.snap ? ' (on the nearest beat)' : ''}" onclick={() => trackCommand(Cmd.LoopStart)}>A</button>
      <button class:lit={t.loop === 1 || t.loop === 2} disabled={t.loop === 0} title="Mark the loop's end where the track is now{t.snap ? ' (on the nearest beat)' : ''}" onclick={() => trackCommand(Cmd.LoopEnd)}>B</button>
      <button class:lit={t.loop === 2} disabled={t.loop !== 1 && t.loop !== 2} title="Switch the loop off or on again; its points are kept with the track" onclick={() => trackCommand(t.loop === 2 ? Cmd.LoopOff : Cmd.LoopOn)}>
        Loop {t.loop === 2 ? "on" : "off"}
      </button>
      <button disabled={t.loop === 0} title="Loop off and its points forgotten" onclick={() => trackCommand(Cmd.LoopForget)}>Forget loop</button>
    </div>

    <div class="section">
      <h3>Loop in bars</h3>
      {#if beatSeconds(t)}
        <div class="controls">
          <div class="field">
            <div class="stepper">
              <button aria-label="Start a bar earlier" title="A bar earlier" onclick={() => moveStart(-per)}>«</button>
              <button aria-label="Start a beat earlier" title="A beat earlier" onclick={() => moveStart(-1)}>‹</button>
              <b>{bars.bar}.{bars.beat}</b>
              <button aria-label="Start a beat later" title="A beat later" onclick={() => moveStart(1)}>›</button>
              <button aria-label="Start a bar later" title="A bar later" onclick={() => moveStart(per)}>»</button>
            </div>
            <div class="label">Start (bar.beat)</div>
          </div>
          <div class="field">
            <button class="here" title="Start at the bar the track is in" onclick={() => loopBars({ bar: where?.bar ?? 1, beat: 1, bars: bars.bars })}>Here</button>
            <div class="label">Start</div>
          </div>
          <div class="field">
            <div class="lengths" role="radiogroup" aria-label="Loop length in bars">
              {#each LENGTHS as n}
                {@const on = !!(fromLoop || draft) && bars.bars === n}
                <button class:on role="radio" aria-checked={on} onclick={() => loopBars({ ...bars, bars: n })}>{n}</button>
              {/each}
            </div>
            <div class="label">Length in bars</div>
          </div>
          <span class="quiet span">
            {#if fromLoop || draft}{bars.bar}.{bars.beat} up to {barsEnd}{#if !LENGTHS.includes(bars.bars)}, {bars.bars} bars{/if}{:else}Nothing is set until a value is touched.{/if}
          </span>
        </div>
      {:else}
        <p class="quiet">{scanning ? "Once the tempo is found." : "Needs the track's tempo: none was found. Type it below."}</p>
      {/if}
    </div>

    <div class="section">
      <h3>Tempo</h3>
      <div class="controls mid">
        <input
          class="slider"
          type="range"
          min="50"
          max="150"
          step="1"
          value={t.tempo}
          aria-label="Tempo in percent of the track's own"
          aria-valuetext="{t.tempo} %"
          title="50 to 150 % of the track's own tempo, at the same pitch. Right-click for 100 %."
          oninput={(e) => setTempo(Number(e.currentTarget.value))}
          oncontextmenu={(e) => (e.preventDefault(), setTempo(100))}
        />
        <b class="big">{t.tempo} %</b>
        {#if t.ownTempo > 0}<span class="quiet">{((t.ownTempo * t.tempo) / 100).toFixed(1)} BPM</span>{/if}
        <button class="plain" disabled={t.tempo === 100} onclick={() => setTempo(100)}>100 %</button>
        <span class="quiet">The pitch stays. A change is heard about a third of a second after the slider rests.</span>
      </div>
    </div>

    <div class="section">
      <h3>The track's own tempo</h3>
      <div class="controls">
        <div class="field left">
          <b class="big">
            {#if t.ownTempo > 0}
              {t.ownTempo.toFixed(2)} BPM{#if !t.sure}<span class="unsure" title="The scan was unsure: the beat is not steady, or the tempo is half or double">&nbsp;?</span>{/if}
            {:else if scanning}
              finding the tempo {t.scan} %
            {:else}
              no beat found
            {/if}
          </b>
          <div class="label">
            {#if scanning}{t.playing === 2 ? "The scan waits while the track plays" : "The scan runs while the track is not playing"}{:else if t.ownTempo > 0 && !t.sure}Unsure{:else}Found by the scan, or set here{/if}
          </div>
        </div>
        <div class="field">
          <div class="row">
            <button disabled={!t.ownTempo} title="The tempo was found at double" onclick={() => trackCommand(Cmd.OwnTempoHalf)}>÷2</button>
            <button disabled={!t.ownTempo} title="The tempo was found at half" onclick={() => trackCommand(Cmd.OwnTempoDouble)}>×2</button>
          </div>
          <div class="label">Correct</div>
        </div>
        <div class="field">
          <form class="row" onsubmit={(e) => (e.preventDefault(), typeBpm())}>
            <input class="bpm" inputmode="decimal" placeholder="BPM" aria-label="The track's tempo in BPM" bind:value={bpmText} />
            <button type="submit" disabled={!bpmText.trim()}>Set</button>
          </form>
          <div class="label">By hand (30 to 300)</div>
        </div>
        <Control setting={beatsSetting} value={t.beatsPerBar} colour={TRACK_COLOUR} onchange={(v) => trackCommand(Cmd.BeatsPerBar, v)} />
        <Control setting={snapSetting} value={t.snap ? 1 : 0} colour={TRACK_COLOUR} onchange={(v) => trackCommand(Cmd.Snap, v)} />
        <div class="field">
          <div class="row"><button title="Find the tempo and the beats again. It runs while the track is not playing." onclick={() => trackCommand(Cmd.Scan)}>Scan again</button></div>
          <div class="label">Tempo and beats</div>
        </div>
      </div>
    </div>

    {#if volume}
      <div class="section">
        <h3>Level</h3>
        <div class="controls">
          <Control setting={volume} value={device.values[volume.id] ?? volume.default} colour={TRACK_COLOUR} onchange={(v) => setParam(volume.id, v)} />
          <span class="quiet span">The track joins the output beside the drums, after the effects and before the master volume. One setting for all tracks, not in presets.</span>
        </div>
      </div>
    {/if}
  {:else}
    <p class="quiet">No track is open. "Choose…" shows the tracks on the card; "Send tracks…" there puts .wav and .mp3 files on it.</p>
  {/if}
</section>

<style>
  button,
  input {
    font: inherit;
    color: inherit;
  }
  button {
    cursor: pointer;
  }
  button:disabled {
    opacity: 0.35;
    cursor: default;
  }
  button:focus-visible,
  .timeline:focus-visible {
    outline: 2px solid color-mix(in srgb, var(--c) 70%, #fff);
    outline-offset: 2px;
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
    margin-bottom: 12px;
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
  .quiet {
    color: #8a92a0;
    font-variant-numeric: tabular-nums;
  }
  .head .quiet {
    white-space: nowrap;
  }
  .quiet.bad {
    color: #ffb347;
  }
  p.quiet {
    margin: 6px 0 0;
  }
  .plain,
  .row button,
  .here {
    flex: none;
    border: 1px solid #353b47;
    background: #22262e;
    border-radius: 7px;
    padding: 4px 11px;
    line-height: 18px;
    white-space: nowrap;
  }
  .plain:hover:not(:disabled),
  .row button:hover:not(:disabled),
  .here:hover {
    background: #2b303a;
    border-color: #454c5a;
  }
  .row button.lit {
    border-color: var(--c);
    background: color-mix(in srgb, var(--c) 26%, #22262e);
    color: #fff;
  }
  .onoff {
    min-width: 76px;
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

  /* The whole track as a bar: what is played, the bar lines, the loop's section, the place. */
  .timeline {
    position: relative;
    height: 34px;
    border-radius: 7px;
    background: #101216;
    border: 1px solid #2a2e37;
    overflow: hidden;
    cursor: pointer;
    touch-action: none;
    user-select: none;
    -webkit-user-select: none;
  }
  .played {
    position: absolute;
    inset: 0 auto 0 0;
    background: color-mix(in srgb, var(--c) 22%, #101216);
  }
  .timeline i {
    position: absolute;
    top: 0;
    bottom: 0;
    width: 1px;
    background: #2f3540;
  }
  .loop {
    position: absolute;
    top: 0;
    bottom: 0;
    box-sizing: border-box;
    min-width: 2px;
    background: color-mix(in srgb, #ffb347 34%, transparent);
    border-left: 2px solid #ffb347;
    border-right: 2px solid #ffb347;
  }
  /* Switched off, the section stays marked, fainter. */
  .loop.off {
    background: color-mix(in srgb, #ffb347 12%, transparent);
    border-color: color-mix(in srgb, #ffb347 45%, transparent);
  }
  .loop.mark {
    border-right: 0;
    background: none;
  }
  .cursor {
    position: absolute;
    top: 0;
    bottom: 0;
    width: 2px;
    margin-left: -1px;
    background: #fff;
  }
  .readout {
    display: flex;
    flex-wrap: wrap;
    align-items: baseline;
    gap: 4px 12px;
    margin: 8px 2px 12px;
    font-variant-numeric: tabular-nums;
    color: #c3c8d1;
  }
  .readout > b {
    min-width: 62px;
    font-size: 19px;
    color: #e6e8ec;
  }
  .bb b {
    color: #e6e8ec;
  }
  .row {
    display: flex;
    flex-wrap: wrap;
    align-items: center;
    gap: 6px;
  }
  .transport button {
    min-width: 38px;
    padding: 6px 12px;
  }
  .transport .wide {
    min-width: 76px;
  }
  .gap {
    width: 14px;
  }
  .section {
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
    gap: 14px 22px;
  }
  .controls.mid {
    align-items: center;
    gap: 10px 14px;
  }
  .span {
    flex: 1 1 220px;
    max-width: 520px;
    margin-bottom: 4px;
    font-size: 11.5px;
    line-height: 1.5;
  }
  .controls.mid > span.quiet:last-child {
    font-size: 11.5px;
  }
  .field {
    display: flex;
    flex-direction: column;
    align-items: center;
    gap: 6px;
  }
  .field.left {
    align-items: flex-start;
    min-width: 170px;
  }
  .lengths {
    display: flex;
    border: 1px solid #353b47;
    border-radius: 7px;
    overflow: hidden;
  }
  .lengths button {
    min-width: 34px;
    background: #22262e;
    border: 0;
    border-left: 1px solid #353b47;
    padding: 5px 8px;
    font-size: 12px;
    color: #aab2bf;
  }
  .lengths button:first-child {
    border-left: 0;
  }
  .lengths button.on {
    background: color-mix(in srgb, var(--c) 28%, #22262e);
    color: #fff;
  }
  .field > .row,
  .field > .lengths,
  .field > .stepper,
  .field > .here,
  .field > .big {
    margin-bottom: 6px;
  }
  .label {
    font-size: 10.5px;
    letter-spacing: 0.08em;
    text-transform: uppercase;
    color: #8a92a0;
    white-space: nowrap;
  }
  .stepper {
    display: flex;
    align-items: center;
    border: 1px solid #353b47;
    border-radius: 7px;
    background: #22262e;
  }
  .stepper button {
    background: none;
    border: 0;
    width: 24px;
    height: 28px;
    font-size: 16px;
    line-height: 1;
    color: #aab2bf;
  }
  .stepper b {
    min-width: 46px;
    text-align: center;
    font-variant-numeric: tabular-nums;
  }
  .big {
    font-size: 15px;
    font-variant-numeric: tabular-nums;
    white-space: nowrap;
  }
  .controls.mid .big {
    min-width: 52px;
    text-align: right;
  }
  .unsure {
    color: #ffb347;
  }
  .slider {
    width: min(320px, 40%);
    accent-color: var(--c);
  }
  .bpm {
    width: 62px;
    box-sizing: border-box;
    padding: 4px 8px;
    line-height: 18px;
    background: #101216;
    border: 1px solid #353b47;
    border-radius: 7px;
    outline: none;
  }
  .bpm:focus {
    border-color: var(--c);
  }
</style>
