<script lang="ts">
  // The backing track's lane under the drums: the open track, play / pause, stop, its place.
  // A click on it opens the player's panel, as a click on a block opens the block's.
  import { device } from "$lib/device.svelte";
  import { clock, Cmd, playhead, playPause, trackCommand, trackTitle, TRACK_COLOUR } from "$lib/track.svelte";

  let { selected, onselect }: { selected: boolean; onselect: () => void } = $props();

  const t = $derived(device.track);
  const open = $derived(!!t?.path);
  const playing = $derived(t?.playing === 2);
</script>

{#if t}
  <section class="lane" class:selected class:playing style:--c={TRACK_COLOUR} aria-label="Backing track">
    <button class="play" disabled={!open} aria-label={playing ? "Pause the track" : "Play the track"} title={playing ? "Pause" : "Play"} onclick={playPause}>
      {#if playing}
        <svg viewBox="0 0 16 16"><rect x="4" y="3.5" width="3" height="9" rx="0.8" /><rect x="9" y="3.5" width="3" height="9" rx="0.8" /></svg>
      {:else}
        <svg viewBox="0 0 16 16"><path d="M5 3.5v9l7.5-4.5z" /></svg>
      {/if}
    </button>
    <button class="play stop" disabled={!open} aria-label="Stop the track" title="Stop: back to the start, or to the loop's start" onclick={() => trackCommand(Cmd.Stop)}>
      <svg viewBox="0 0 16 16"><rect x="4" y="4" width="8" height="8" rx="1" /></svg>
    </button>
    <button class="body" onclick={onselect} aria-pressed={selected}>
      <span class="short">TRACK</span>
      {#if open}
        <span class="name" title={t.path}>{trackTitle(t.path)}</span>
        <span class="chip">{clock(playhead.place)} / {clock(t.length)}</span>
        {#if t.tempo !== 100}<span class="chip"><i>Tempo</i> {t.tempo} %</span>{/if}
        {#if t.loop === 2}<span class="chip"><i>Loop</i> {clock(t.loopFrom)} – {clock(t.loopTo)}</span>{/if}
        {#if t.scan >= 0 && t.scan < 100}<span class="chip"><i>Finding the tempo</i> {t.scan} %</span>{/if}
      {:else}
        <span class="quiet">No track open</span>
      {/if}
    </button>
  </section>
{/if}

<style>
  button {
    font: inherit;
    color: inherit;
    cursor: pointer;
  }
  button:disabled {
    opacity: 0.35;
    cursor: default;
  }
  button:focus-visible {
    outline: 2px solid color-mix(in srgb, var(--c) 70%, #fff);
    outline-offset: 2px;
  }
  svg {
    display: block;
    fill: currentColor;
    stroke: none;
  }
  .lane {
    display: flex;
    align-items: center;
    gap: 8px;
    margin: 8px 18px 0;
    padding: 8px 12px;
    border-radius: 12px;
    background: #181b21;
    border: 1px solid #2a2e37;
  }
  .lane.selected {
    border-color: var(--c);
  }
  .play {
    flex: none;
    width: 32px;
    height: 32px;
    padding: 8px;
    border-radius: 50%;
    border: 1px solid color-mix(in srgb, var(--c) 60%, #2a2e37);
    background: color-mix(in srgb, var(--c) 16%, #181b21);
    color: var(--c);
  }
  .lane.playing .play:not(.stop) {
    background: var(--c);
    color: #101216;
  }
  .body {
    flex: 1;
    display: flex;
    align-items: center;
    gap: 10px;
    margin-left: 4px;
    background: none;
    border: 0;
    padding: 4px 0;
    min-width: 0;
    overflow: hidden;
    text-align: left;
  }
  .short {
    margin-right: 6px;
    font-size: 13px;
    font-weight: 700;
    letter-spacing: 0.1em;
    white-space: nowrap;
    color: color-mix(in srgb, var(--c) 70%, #fff);
  }
  .name {
    min-width: 60px;
    flex: 0 1 auto;
    font-weight: 600;
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }
  .quiet {
    color: #8a92a0;
  }
  .chip {
    flex: none;
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
</style>
