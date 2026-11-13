<script lang="ts">
  // The tuner, over the whole window: the note, how far off it is, and the frequency.
  import { meters, setTuner, tuner } from "$lib/device.svelte";

  const NAMES = ["C", "C♯", "D", "E♭", "E", "F", "F♯", "G", "A♭", "A", "B♭", "B"];
  const IN_TUNE = 3; // cents either side that count as in tune

  const inTune = $derived(tuner.valid && Math.abs(tuner.cents) <= IN_TUNE);
  // The needle: -50 .. +50 cents across the scale.
  const pos = $derived(50 + Math.max(-50, Math.min(50, tuner.cents)));
  const focusNow = (el: HTMLElement) => el.focus();
</script>

{#if meters.tunerOpen}
  <div class="tuner" role="dialog" aria-label="Tuner" tabindex="-1" use:focusNow onkeydown={(e) => e.key === "Escape" && setTuner(false)}>
    <div class="note" class:in={inTune} class:none={!tuner.valid}>
      {#if tuner.valid}{NAMES[tuner.note % 12]}<sub>{tuner.octave}</sub>{:else}–{/if}
    </div>
    <div class="scale" class:in={inTune}>
      <div class="zone"></div>
      {#each [0, 25, 50, 75, 100] as t}<div class="tick" style:left="{t}%"></div>{/each}
      {#if tuner.valid}<div class="needle" style:left="{pos}%"></div>{/if}
    </div>
    <div class="ends"><span>flat</span><span>sharp</span></div>
    <div class="read">
      {#if tuner.valid}
        {tuner.cents > 0 ? "+" : ""}{tuner.cents.toFixed(1)} cents · {tuner.hz.toFixed(2)} Hz
      {:else}
        Play a single string
      {/if}
    </div>
    <p>The guitar is muted while the tuner is open.</p>
    <button onclick={() => setTuner(false)}>Close (Esc)</button>
  </div>
{/if}

<style>
  .tuner {
    position: fixed;
    inset: 0;
    z-index: 10;
    display: flex;
    flex-direction: column;
    align-items: center;
    justify-content: center;
    gap: 14px;
    background: rgba(13, 15, 18, 0.96);
    outline: none;
  }
  .note {
    font-size: 150px;
    font-weight: 700;
    line-height: 1;
    color: #ffb347;
    font-variant-numeric: tabular-nums;
  }
  .note.in {
    color: #3ddc84;
  }
  .note.none {
    color: #4a505c;
  }
  sub {
    font-size: 44px;
    font-weight: 500;
    margin-left: 6px;
    color: #8a92a0;
    vertical-align: baseline;
  }
  .scale {
    position: relative;
    width: min(560px, 80vw);
    height: 44px;
    margin-top: 10px;
    border-radius: 8px;
    background: #1c2027;
  }
  .zone {
    position: absolute;
    left: 47%;
    width: 6%;
    height: 100%;
    background: rgba(61, 220, 132, 0.22);
  }
  .tick {
    position: absolute;
    top: 0;
    width: 1px;
    height: 100%;
    background: #353b47;
  }
  .needle {
    position: absolute;
    top: -6px;
    width: 6px;
    height: 56px;
    margin-left: -3px;
    border-radius: 3px;
    background: #ffb347;
    transition: left 0.1s linear;
  }
  .scale.in .needle {
    background: #3ddc84;
    box-shadow: 0 0 14px #3ddc84;
  }
  .ends {
    display: flex;
    justify-content: space-between;
    width: min(560px, 80vw);
    font-size: 11px;
    letter-spacing: 0.1em;
    text-transform: uppercase;
    color: #8a92a0;
  }
  .read {
    font-size: 17px;
    font-variant-numeric: tabular-nums;
    color: #c3c8d1;
  }
  p {
    margin: 12px 0 0;
    color: #8a92a0;
  }
  button {
    font: inherit;
    color: inherit;
    cursor: pointer;
    border: 1px solid #353b47;
    background: #22262e;
    border-radius: 8px;
    padding: 7px 16px;
  }
</style>
