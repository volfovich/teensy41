<script lang="ts">
  // The main screen (APP.md 3.1): the signal chain as blocks, the drums lane, and the selected
  // block's controls. Everything is built from the device's settings table and the block
  // definitions shared with the unit's own UI (protocol/blocks.json).
  import { onMount } from "svelte";
  import { blockActive, blockOn, blockOnValue, blockPeak, buildBlocks, type Block } from "$lib/blocks";
  import LevelBar from "$lib/controls/LevelBar.svelte";
  import Control from "$lib/controls/Control.svelte";
  import Meter from "$lib/controls/Meter.svelte";
  import { cancelOffer, cardFile, connectDevice, device, dropping, meters, notice, offered, pickFiles, presetAction, preview, sendFiles, setParam, setTuner, transfer } from "$lib/device.svelte";
  import Tuner from "$lib/Tuner.svelte";
  import { baseName, formatValue } from "$lib/format";
  import type { Setting } from "$lib/types";

  onMount(() => {
    let stop = () => {};
    connectDevice().then((s) => {
      stop = s;
      if (preview.on && hashTuner === "tuner") setTuner(true);
      if (preview.on && hashAsk === "recall") recall(device.presets[0].name);
      if (preview.on && hashAsk === "files") pickFiles();
    });
    return () => stop();
  });

  const blocks = $derived(buildBlocks(device.settings));
  const lane = $derived(blocks.lane);
  const all = $derived([...blocks.chain, blocks.lane, ...(blocks.other ? [blocks.other] : [])]);
  // The address can name the block and the library tab to open ("#reverb", "#amp/models", and in the preview "#amp//tuner", "#amp///recall", "#amp///files"):
  // handy when working on the UI.
  const [hashBlock, hashTab, hashTuner, hashAsk] = (typeof location !== "undefined" ? location.hash.slice(1) : "").split("/");
  let selectedId = $state(hashBlock || "amp");
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

  // The master volume's lowest step is silence.
  const masterText = (m: Setting, v: number) => (v <= m.min ? "Silent" : formatValue(m, v));

  // The loaded model's and IR's details, as short facts for the editor.
  const signed = (db: number) => `${db > 0 ? "+" : ""}${db.toFixed(1)} dB`;
  const words = (text: string) => text.replaceAll("_", " ");
  const modelFacts = $derived.by(() => {
    const m = device.modelInfo;
    if (!m) return [];
    const gear = m.gearMake === m.gearModel ? m.gearMake : [m.gearMake, m.gearModel].filter(Boolean).join(" ");
    const out: [string, string, string][] = [];
    if (m.name) out.push(["Name", m.name, ""]);
    if (gear) out.push(["Gear", gear, "What was captured, as the file says"]);
    if (m.gearType || m.toneType) out.push(["Type", [m.gearType, m.toneType].filter(Boolean).map(words).join(", "), ""]);
    out.push([
      "Loudness",
      m.loudness === null ? "not in the file" : `${m.loudness.toFixed(1)} dB, levelled ${signed(m.levelDb)}`,
      "The model's own loudness, and the gain the unit adds at the amp's output so that every model is about as loud",
    ]);
    out.push([
      "Input",
      m.calibrationDb === null
        ? "calibration off"
        : `${m.inputLevelDbu === null ? "not calibrated, taken as +12 dBu" : `captured at ${m.inputLevelDbu.toFixed(1)} dBu`}, ${signed(m.calibrationDb)}`,
      "The level the capture was made at, and the gain the unit adds at the amp's input so the model is driven as the real amp was",
    ]);
    if (Math.abs(m.sampleRate - 48000) > 1) out.push(["Rate", `${m.sampleRate.toFixed(0)} Hz: the unit runs at 48000 Hz, so its tone is shifted`, ""]);
    return out;
  });
  const irFacts = $derived.by(() => {
    const r = device.irInfo;
    if (!r) return [];
    const rate = device.info?.sampleRate ?? 48000;
    const out: [string, string, string][] = [
      ["File", `${r.sampleRate / 1000} kHz, ${r.channels === 1 ? "mono" : r.channels === 2 ? "stereo" : `${r.channels} channels`}, ${r.bits}-bit, ${((1000 * r.frames) / r.sampleRate).toFixed(0)} ms`, ""],
      ["In use", `${r.length} samples (${((1000 * r.length) / rate).toFixed(0)} ms)`, "What the unit convolves with, after removing silence at the start and the inaudible end"],
    ];
    const done = [r.resampled ? `resampled to ${rate / 1000} kHz` : "", r.channels > 1 ? "first channel only" : "", r.cut ? "cut at the unit's longest length" : ""].filter(Boolean);
    if (done.length) out.push(["Changed", done.join(", "), ""]);
    out.push(["Level", signed(20 * Math.log10(Math.max(r.gain, 1e-6))), "The gain the unit applies so that all IRs are about as loud"]);
    return out;
  });
  const factsOf = (b: Block) => (b.def.file === "model" ? modelFacts : b.def.file === "ir" ? irFacts : []);

  // The level at a block's output. The Output block's is the unit's output itself.
  const peakOf = (b: Block): number | null => (b.def.id === "output" ? meters.outputPeak : blockPeak(b, meters));
  // The level bars on the tiles can be switched off; the choice is remembered.
  let showLevels = $state(typeof localStorage === "undefined" || localStorage.getItem("levels") !== "off");
  function toggleLevels() {
    showLevels = !showLevels;
    localStorage.setItem("levels", showLevels ? "on" : "off");
  }

  const cpuOf = (b: Block): number | null =>
    ({ amp: meters.cpuAmp, doubler: meters.cpuDoubler, delay: meters.cpuDelay, reverb: meters.cpuReverb, drums: meters.cpuDrums })[b.def.id] ?? null;

  // The clip count only grows: "clipped" lights for 2 s after it last went up.
  // `now` is a clock that ticks, so that things shown "for 2 s" also go away again.
  let clipSeen = -1; // -1: no frame yet (the count at connection is old news)
  let clipAt = $state(-1e9);
  let now = $state(0);
  onMount(() => {
    const tick = setInterval(() => (now = performance.now()), 250);
    return () => clearInterval(tick);
  });
  $effect(() => {
    const count = meters.clipped;
    if (clipSeen >= 0 && count > clipSeen) clipAt = performance.now();
    clipSeen = device.connected ? count : -1;
  });
  const clipping = $derived(now - clipAt < 2000);

  // ---- The library on the unit's card: presets, amp models, IRs
  let tab = $state<"presets" | "models" | "irs">(hashTab === "models" || hashTab === "irs" ? hashTab : "presets");
  const cardList = $derived(tab === "models" ? device.models : device.irs);
  const loadedPath = $derived(tab === "models" ? device.files.model : device.files.ir);
  let deleting = $state("");
  const kb = (bytes: number) => (bytes >= 1048576 ? `${(bytes / 1048576).toFixed(1)} MB` : `${Math.max(1, Math.round(bytes / 1024))} KB`);
  const sending = $derived(!transfer.done);
  const sentShown = $derived(transfer.done && !transfer.error && transfer.name && now - transfer.at < 4000);

  // ---- Presets
  const current = $derived(device.files.preset);
  // The name after the number: "03 Lead" -> "Lead".
  const bare = (name: string) => name.replace(/^\d\d /, "");
  function stepPreset(d: number) {
    const list = device.presets;
    if (!list.length) return;
    const i = list.findIndex((p) => p.name === current);
    const next = list[i < 0 ? 0 : (i + d + list.length) % list.length];
    if (next.name !== current) recall(next.name);
  }
  // Recalling a preset throws away unsaved edits: ask first.
  let pendingRecall = $state("");
  function recall(name: string) {
    if (name === current && !device.files.edited) return;
    if (device.files.edited) pendingRecall = name;
    else presetAction("recall", name);
  }
  function answerRecall(yes: boolean) {
    if (yes) presetAction("recall", pendingRecall);
    pendingRecall = "";
  }
  const asking = $derived(!!pendingRecall || offered.replacing.length > 0);
  const focusEl = (el: HTMLElement) => el.focus();
  // The name field under the list: for "save as" or for renaming the recalled preset.
  let naming = $state<"" | "new" | "rename">("");
  let nameText = $state("");
  let confirmDelete = $state(false);
  function startNaming(mode: "new" | "rename") {
    naming = mode;
    nameText = mode === "rename" ? bare(current) : "";
    confirmDelete = false;
  }
  function commitName() {
    const text = nameText.trim();
    if (naming === "new") presetAction("new", text);
    else if (naming === "rename" && text && text !== bare(current)) presetAction("rename", text);
    naming = "";
  }
  const focusNow = (el: HTMLInputElement) => {
    el.focus();
    el.select();
  };
  const noticeShown = $derived(notice.text && now - notice.at < 5000);

  // Space switches the selected block, the arrows step through the presets, Cmd-S saves:
  // unless a control has the keyboard.
  function key(e: KeyboardEvent) {
    const t = e.target as HTMLElement;
    if (e.key === "s" && e.metaKey) {
      e.preventDefault();
      if (current && device.files.edited) presetAction("save");
      else if (!current) startNaming("new");
      return;
    }
    if (asking) {
      if (e.key === "Escape") (answerRecall(false), cancelOffer());
      return; // a question is open: it has the keyboard
    }
    if (t.closest("input, select, button, [role=slider]")) return;
    if (e.key === "l" || e.key === "L") {
      toggleLevels();
    } else if (e.key === "t" || e.key === "T") {
      if (device.connected) setTuner(!meters.tunerOpen);
    } else if (meters.tunerOpen) {
      return; // the tuner has the keyboard
    } else if (e.key === " ") {
      if (selected) toggle(selected);
    } else if (e.key >= "1" && e.key <= "9" && !e.metaKey && !e.ctrlKey) {
      // 1 .. 7 open the chain's blocks, the next number the drums.
      const b = [...blocks.chain, blocks.lane][Number(e.key) - 1];
      if (!b) return;
      selectedId = b.def.id;
    } else if (e.key === "ArrowLeft" || e.key === "ArrowUp") stepPreset(-1);
    else if (e.key === "ArrowRight" || e.key === "ArrowDown") stepPreset(1);
    else return;
    e.preventDefault();
  }

  const laneChips = ["bpm", "drum style", "drum kit", "drum song", "bass key"];
  const chip = (key: string) => blocks.lane.sections.flatMap((s) => s.settings).find((s) => s.key === key);
</script>

<svelte:window onkeydown={key} />

<Tuner />

{#if pendingRecall}
  <div class="veil">
    <div class="ask-box" role="alertdialog" aria-label="Discard the changes?">
      <h2>Discard the changes?</h2>
      <p>
        {#if pendingRecall === current}
          "{bare(current)}" has changes that are not saved. Recalling it again goes back to the saved sound.
        {:else}
          "{bare(current)}" has changes that are not saved. Recalling "{bare(pendingRecall)}" discards them.
        {/if}
      </p>
      <div class="row">
        <button class="danger" onclick={() => answerRecall(true)}>Discard and recall</button>
        <button onclick={() => (presetAction("save"), answerRecall(true))}>Save, then recall</button>
        <button use:focusEl onclick={() => answerRecall(false)}>Cancel</button>
      </div>
    </div>
  </div>
{:else if offered.replacing.length}
  <div class="veil">
    <div class="ask-box" role="alertdialog" aria-label="Replace files on the unit?">
      <h2>Replace {offered.replacing.length === 1 ? "this file" : "these files"} on the unit?</h2>
      <p>The card already has {offered.replacing.length === 1 ? "a file" : "files"} with the same name:</p>
      <ul>{#each offered.replacing as name}<li>{name}</li>{/each}</ul>
      <div class="row">
        <button class="danger" onclick={() => sendFiles(offered.paths)}>Replace</button>
        {#if offered.paths.length > offered.replacing.length}
          <button onclick={() => sendFiles(offered.paths.filter((p) => !offered.replacing.includes(p.split("/").pop() ?? "")))}>Send only the new ones</button>
        {/if}
        <button use:focusEl onclick={cancelOffer}>Cancel</button>
      </div>
    </div>
  </div>
{/if}

<header>
  <!-- Connected, the name is at the top of the library column: the header needs the room. -->
  {#if !device.connected}<div class="brand">VOLFOVICH <b>NAM FORGE</b></div>{/if}
  <div class="status" class:on={device.connected} title={device.connected ? (preview.on ? "Preview with saved data, no unit" : `Connected on ${device.info?.port}`) : "Not connected"}>
    <span class="dot"></span>
    {device.connected ? (preview.on ? "Preview" : "") : "Not connected"}
  </div>
  {#if device.connected}
    <div class="preset">
      <button class="step" aria-label="Previous preset" title="Previous preset (left arrow)" disabled={device.presets.length < 2} onclick={() => stepPreset(-1)}>‹</button>
      <b title="The preset recalled last">{current || "No preset"}</b>
      {#if device.files.edited}<i title="Changed since it was recalled">edited</i>{/if}
      <button class="step" aria-label="Next preset" title="Next preset (right arrow)" disabled={device.presets.length < 2} onclick={() => stepPreset(1)}>›</button>
      <button class="save" disabled={!current || !device.files.edited} title="Write the sound over this preset (Cmd-S)" onclick={() => presetAction("save")}>Save</button>
      <button class="save" disabled={!device.files.card} title="Save the sound as a new preset" onclick={() => startNaming("new")}>Save as…</button>
      <button class="save" title="Tuner (T): mutes the guitar while it is open" onclick={() => setTuner(true)}>Tuner</button>
      <button class="save" class:lit={showLevels} aria-pressed={showLevels} title="Show each block's output level on its tile (L)" onclick={toggleLevels}>Levels</button>
    </div>
    <div class="meters">
      <Meter label="IN" peak={meters.inputPeak} />
      <Meter label="OUT" peak={meters.outputPeak} clipped={clipping} />
      {#if blocks.master}
        {@const m = blocks.master}
        {@const v = value(m.id, m.default)}
        <label class="master" title="Master volume: from silent to 0 dB, the unit's full output level. One setting for all presets, kept by the unit and not saved in presets. Right-click for the default.">
          <span>MASTER</span>
          <input
            type="range"
            min={m.min}
            max={m.max}
            step="1"
            value={v}
            aria-valuetext={masterText(m, v)}
            oninput={(e) => setParam(m.id, Number(e.currentTarget.value))}
            oncontextmenu={(e) => (e.preventDefault(), setParam(m.id, m.default))}
          />
          <b>{masterText(m, v)}</b>
        </label>
      {/if}
    </div>
  {/if}
</header>

{#if device.connected && selected}
  {#if meters.muted}
    <div class="alert">Overload: the chain could not keep up, so the unit muted its output and is switching effects off.</div>
  {/if}

  {#if noticeShown}
    <div class="alert soft">{notice.text}</div>
  {/if}

  <div class="columns">
  <aside aria-label="Library" class:dropping={dropping.on}>
    <div class="brand">VOLFOVICH <b>NAM FORGE</b></div>
    <div class="tabs" role="tablist">
      <button role="tab" aria-selected={tab === "presets"} onclick={() => (tab = "presets")}>Presets</button>
      <button role="tab" aria-selected={tab === "models"} onclick={() => (tab = "models")}>Amps</button>
      <button role="tab" aria-selected={tab === "irs"} onclick={() => (tab = "irs")}>IRs</button>
    </div>
    {#if tab !== "presets"}
      {#if !device.files.card}
        <p class="none">No SD card in the unit.</p>
      {:else if !cardList.length}
        <p class="none">{tab === "models" ? "No amp models on the card. Add NAM A2 files (.nam)." : "No IRs on the card. Add .wav files."}</p>
      {/if}
      <ul>
        {#each cardList as f (f.path)}
          <li class="file">
            {#if deleting === f.path}
              <div class="row confirm">
                <span class="ask">Delete "{stem(f.path)}" from the card?</span>
                <button class="danger" onclick={() => (cardFile("delete", f.path), (deleting = ""))}>Delete</button>
                <button onclick={() => (deleting = "")}>Keep</button>
              </div>
            {:else}
              <button class:current={f.path === loadedPath} aria-current={f.path === loadedPath} title="{f.path} · {kb(f.size)}" onclick={() => f.path !== loadedPath && cardFile(tab === "models" ? "model" : "ir", f.path)}>
                <span class="pname">{stem(f.path)}</span>
              </button>
              <button class="del" aria-label="Delete {stem(f.path)}" title="Delete from the card" onclick={() => (deleting = f.path)}>×</button>
            {/if}
          </li>
        {/each}
      </ul>
      <div class="tools">
        {#if sending}
          <div class="sending">Sending {transfer.name}<br />{kb(transfer.sent)} of {kb(transfer.total)}</div>
          <div class="progress"><div style:width="{transfer.total ? (100 * transfer.sent) / transfer.total : 0}%"></div></div>
        {:else}
          {#if sentShown}<div class="sending">{transfer.name} is on the unit.</div>{/if}
          <div class="row"><button disabled={!device.files.card} onclick={pickFiles}>Add files…</button></div>
          <p class="hint">Or drop .nam and .wav files on the window.</p>
        {/if}
      </div>
    {:else}
    {#if !device.files.card}
      <p class="none">No SD card in the unit.</p>
    {:else if !device.presets.length}
      <p class="none">None yet. "Save as…" stores the current sound as the first.</p>
    {/if}
    <ul>
      {#each device.presets as p (p.name)}
        <li>
          <button class:current={p.name === current} aria-current={p.name === current} onclick={() => recall(p.name)}>
            <span class="num">{p.number ? String(p.number).padStart(2, "0") : "–"}</span>
            <span class="pname">{bare(p.name)}</span>
            {#if p.name === current && device.files.edited}<span class="mark" title="Edited">●</span>{/if}
          </button>
        </li>
      {/each}
    </ul>
    {#if naming}
      <form class="naming" onsubmit={(e) => (e.preventDefault(), commitName())}>
        <label for="pname">{naming === "new" ? "Save as a new preset" : "Rename the preset"}</label>
        <input id="pname" maxlength="24" placeholder="Name" bind:value={nameText} use:focusNow onkeydown={(e) => e.key === "Escape" && (naming = "")} />
        <div class="row">
          <button type="submit" class="primary">{naming === "new" ? "Save" : "Rename"}</button>
          <button type="button" onclick={() => (naming = "")}>Cancel</button>
        </div>
      </form>
    {:else if current}
      <div class="row tools">
        {#if confirmDelete}
          <span class="ask">Delete "{bare(current)}"?</span>
          <button class="danger" onclick={() => (presetAction("delete", current), (confirmDelete = false))}>Delete</button>
          <button onclick={() => (confirmDelete = false)}>Keep</button>
        {:else}
          <button onclick={() => startNaming("rename")}>Rename</button>
          <button onclick={() => (confirmDelete = true)}>Delete</button>
        {/if}
      </div>
    {/if}
    {/if}
  </aside>
  <div class="main">

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
        {#if showLevels && peakOf(b) !== null}<LevelBar peak={peakOf(b) ?? 0} />{/if}
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
      {#if selected.def.file}
        <button class="choose" onclick={() => (tab = selected.def.file === "model" ? "models" : "irs")}>Choose…</button>
      {/if}
      <span class="grow"></span>
      {#if peakOf(selected) !== null}<span class="stagecpu">Out</span><LevelBar text peak={peakOf(selected) ?? 0} />{/if}
      {#if cpuOf(selected) !== null}<span class="stagecpu" title="This block's share of the processing time">{cpuOf(selected)?.toFixed(1)}% CPU</span>{/if}
      {#if selected.on}
        <button class="onoff" class:on={isOn(selected)} onclick={() => toggle(selected)}>
          {selected.def.id === "drums" ? (isOn(selected) ? "Playing" : "Stopped") : isOn(selected) ? "On" : "Off"}
        </button>
      {/if}
    </div>
    {#if factsOf(selected).length && fileOf(selected)}
      <dl class="facts">
        {#each factsOf(selected) as [label, text, hint]}
          <div title={hint}><dt>{label}</dt><dd>{text}</dd></div>
        {/each}
      </dl>
    {/if}
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
      {#if !selected.def.file}<p class="none">This block has no settings of its own.</p>{/if}
    {/each}
  </section>

  </div>
  </div>

  <footer>
    <span>Firmware {device.info?.firmware}</span>
    <span>{device.info?.blockSize}-sample blocks at {(device.info?.sampleRate ?? 0) / 1000} kHz</span>
    <span class:bad={meters.cpuMax >= 80} title="Audio processing time as a share of one block: average, and the highest lately">CPU {meters.cpu.toFixed(0)}% (max {meters.cpuMax.toFixed(0)}%)</span>
    <span class:bad={meters.overruns > 0}>Overruns {meters.overruns}</span>
    <span class:bad={meters.overloadMutes > 0}>Overload mutes {meters.overloadMutes}</span>
    <span class:bad={meters.clipped > 0}>Clipped samples {meters.clipped}</span>
    <span class:bad={!device.files.card}>SD card {device.files.card ? "in" : "missing"}</span>
  </footer>
{:else}
  <div class="waiting">
    <div class="pulse"></div>
    <h2>Looking for the unit</h2>
    <p>{device.detail}</p>
    <ul>
      <li>Connect the unit to this Mac with a USB cable that carries data.</li>
      <li>Only one program can talk to it: close the serial monitor, and wait for a firmware upload to finish.</li>
      <li>The app connects by itself as soon as the unit answers.</li>
    </ul>
  </div>
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
    gap: 16px;
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
  .preset {
    align-items: center;
  }
  .step {
    width: 26px;
    height: 26px;
    border-radius: 50%;
    border: 1px solid #353b47;
    background: #22262e;
    font-size: 17px;
    line-height: 1;
    padding: 0 0 2px;
  }
  .save {
    border: 1px solid #353b47;
    background: #22262e;
    border-radius: 7px;
    padding: 4px 11px;
    white-space: nowrap;
  }
  .save:not(:disabled):first-of-type,
  .primary {
    border-color: #ffb347;
    color: #ffb347;
  }
  button:disabled {
    opacity: 0.35;
    cursor: default;
  }
  .veil {
    position: fixed;
    inset: 0;
    z-index: 20;
    display: grid;
    place-items: center;
    background: rgba(13, 15, 18, 0.7);
  }
  .ask-box {
    width: min(440px, 86vw);
    padding: 20px 22px 18px;
    border-radius: 12px;
    background: #1c2027;
    border: 1px solid #353b47;
    box-shadow: 0 18px 60px rgba(0, 0, 0, 0.5);
  }
  .ask-box h2 {
    color: #e6e8ec;
    text-transform: none;
    letter-spacing: 0;
    white-space: normal;
    margin-bottom: 8px;
  }
  .ask-box p {
    margin: 0 0 10px;
    color: #c3c8d1;
  }
  .ask-box ul {
    margin: 0 0 14px;
    padding-left: 18px;
    color: #e6e8ec;
    max-height: 120px;
    overflow-y: auto;
  }
  .ask-box .row {
    gap: 8px;
    margin-top: 14px;
  }
  .ask-box .row button {
    padding: 6px 12px;
  }
  .alert.soft {
    background: #3a2f17;
    color: #ffe2b0;
    border-color: #6b4d1f;
  }
  .columns {
    flex: 1;
    min-height: 0;
    display: flex;
  }
  .main {
    flex: 1;
    min-width: 0;
    display: flex;
    flex-direction: column;
  }
  aside {
    width: 208px;
    flex: none;
    display: flex;
    flex-direction: column;
    padding: 16px 12px 12px;
    background: #14171c;
    border-right: 1px solid #2a2e37;
    overflow-y: auto;
  }
  aside .brand {
    padding: 0 8px 14px;
  }
  aside.dropping {
    outline: 2px dashed #ffb347;
    outline-offset: -6px;
  }
  .tabs {
    display: flex;
    gap: 2px;
    margin-bottom: 10px;
    padding: 3px;
    background: #101216;
    border-radius: 9px;
  }
  .tabs button {
    flex: 1;
    padding: 5px 0;
    border: 0;
    border-radius: 7px;
    background: none;
    font-size: 12px;
    color: #8a92a0;
  }
  .tabs button[aria-selected="true"] {
    background: #262a33;
    color: #e6e8ec;
  }
  li.file {
    display: flex;
    align-items: center;
  }
  li.file > button:first-child {
    flex: 1;
    min-width: 0;
  }
  li.file .pname {
    font-weight: 500;
  }
  aside li .del {
    width: 22px;
    flex: none;
    padding: 4px 0;
    justify-content: center;
    color: #8a92a0;
    opacity: 0;
  }
  li.file:hover .del,
  .del:focus-visible {
    opacity: 1;
  }
  .confirm {
    padding: 6px 8px;
    background: #1c2027;
    border-radius: 7px;
  }
  .sending {
    padding: 0 2px 6px;
    font-size: 11.5px;
    color: #c3c8d1;
    overflow: hidden;
    text-overflow: ellipsis;
  }
  .progress {
    height: 5px;
    background: #23272f;
    border-radius: 3px;
    overflow: hidden;
  }
  .progress div {
    height: 100%;
    background: #ffb347;
  }
  .hint {
    margin: 7px 2px 0;
    font-size: 11.5px;
    color: #8a92a0;
  }
  aside ul {
    list-style: none;
    margin: 0;
    padding: 0;
    flex: 1;
  }
  aside li button {
    width: 100%;
    display: flex;
    align-items: baseline;
    gap: 9px;
    padding: 7px 8px;
    border: 0;
    border-radius: 7px;
    background: none;
    text-align: left;
  }
  aside li button:hover {
    background: #1c2027;
  }
  aside li button.current {
    background: #2a2416;
    color: #ffb347;
  }
  .num {
    font-variant-numeric: tabular-nums;
    color: #8a92a0;
    font-size: 11.5px;
  }
  .pname {
    flex: 1;
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
    font-weight: 600;
  }
  .mark {
    font-size: 8px;
  }
  aside .none {
    padding: 0 8px;
  }
  .row {
    display: flex;
    flex-wrap: wrap;
    align-items: center;
    gap: 6px;
  }
  .tools,
  .naming {
    margin-top: 10px;
    padding: 10px 6px 0;
    border-top: 1px solid #2a2e37;
  }
  .row button {
    border: 1px solid #353b47;
    background: #22262e;
    border-radius: 7px;
    padding: 4px 10px;
  }
  .row .danger {
    border-color: #ff4d4d;
    color: #ff8d8d;
  }
  .ask {
    width: 100%;
    color: #c3c8d1;
  }
  .naming label {
    display: block;
    font-size: 11.5px;
    color: #8a92a0;
    margin-bottom: 5px;
  }
  .naming input {
    width: 100%;
    box-sizing: border-box;
    margin-bottom: 8px;
    padding: 6px 8px;
    font: inherit;
    color: inherit;
    background: #101216;
    border: 1px solid #353b47;
    border-radius: 7px;
    outline: none;
  }
  .naming input:focus {
    border-color: #ffb347;
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
    gap: 12px;
  }
  .master {
    display: flex;
    align-items: center;
    gap: 7px;
    padding-left: 12px;
    border-left: 1px solid #2a2e37;
  }
  .master span {
    font-size: 10.5px;
    letter-spacing: 0.08em;
    color: #8a92a0;
  }
  .master input {
    width: 96px;
    accent-color: #e6e8ec;
  }
  .master b {
    width: 58px;
    text-align: right;
    font-variant-numeric: tabular-nums;
    white-space: nowrap;
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
    padding: 12px 12px 13px;
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
  .save.lit {
    background: #2d323c;
    color: #fff;
  }
  .gate {
    position: absolute;
    right: 12px;
    bottom: 16px;
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
  .choose {
    flex: none;
    border: 1px solid #353b47;
    background: #22262e;
    border-radius: 7px;
    padding: 3px 10px;
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
  .facts {
    display: flex;
    flex-wrap: wrap;
    gap: 6px 26px;
    margin: 0 0 16px;
    padding: 9px 12px;
    border-radius: 8px;
    background: #14171c;
  }
  .facts div {
    display: flex;
    gap: 7px;
    align-items: baseline;
  }
  .facts dt {
    font-size: 10.5px;
    letter-spacing: 0.08em;
    text-transform: uppercase;
    color: #8a92a0;
  }
  .facts dd {
    margin: 0;
    color: #e6e8ec;
    font-variant-numeric: tabular-nums;
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
    display: flex;
    flex-direction: column;
    align-items: center;
    justify-content: center;
    gap: 10px;
    color: #8a92a0;
    padding: 0 40px;
  }
  .waiting h2 {
    color: #e6e8ec;
    text-transform: none;
    letter-spacing: 0;
    font-size: 19px;
  }
  .waiting p {
    margin: 0;
    font-size: 14px;
    color: #ffb347;
    text-align: center;
  }
  .waiting ul {
    margin: 10px 0 0;
    padding-left: 18px;
    max-width: 480px;
    line-height: 1.7;
  }
  .pulse {
    width: 14px;
    height: 14px;
    border-radius: 50%;
    background: #ffb347;
    animation: pulse 1.4s ease-in-out infinite;
  }
  @keyframes pulse {
    50% {
      opacity: 0.25;
      transform: scale(0.7);
    }
  }
  @media (prefers-reduced-motion: reduce) {
    .pulse,
    .tile.ringing {
      animation: none;
    }
  }
</style>
