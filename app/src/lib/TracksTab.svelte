<script lang="ts">
  // The library's Tracks tab: the backing tracks on the card, one folder at a time, with a
  // search over all folders, and sending, renaming and deleting. The rows come from the
  // unit's own list (TRACK_LIST, TRACK_FIND); the open track is marked.
  import { onMount } from "svelte";
  import { device, dropTo, say, trackFound, transfer } from "$lib/device.svelte";
  import { cancelTrackOffer, findTracks, folderOf, nameProblem, offerTracks, openTrack, pickTracks, sendTracks, showFolder, trackFile, trackOffer, trackTitle } from "$lib/track.svelte";
  import type { TrackRow } from "$lib/types";

  const t = $derived(device.track);
  const list = $derived(device.trackList);
  const crumbs = $derived(list.folder ? list.folder.split("/") : []);
  const pathOf = (r: TrackRow) => (list.folder ? `${list.folder}/${r.name}` : r.name);

  // Files dropped on the window go into the folder shown while this tab is open.
  onMount(() => {
    dropTo.tracks = (paths) => offerTracks(paths, device.trackList.folder);
    return () => (dropTo.tracks = null);
  });

  // ---- The search: asked a moment after the typing stops
  let search = $state("");
  let timer: ReturnType<typeof setTimeout> | undefined;
  const wanted = $derived(search.trim());
  const hits = $derived(wanted && trackFound.text === wanted ? trackFound.hits : null);
  function typed() {
    clearTimeout(timer);
    timer = setTimeout(() => findTracks(search.trim()), 250);
  }
  function endSearch() {
    clearTimeout(timer);
    search = "";
    findTracks("");
  }
  onMount(() => () => {
    clearTimeout(timer);
    findTracks("");
  });

  // ---- A row being renamed or deleted, and the new folder's name
  let renaming = $state("");
  let deleting = $state("");
  let nameText = $state("");
  let naming = $state(false);
  const ending = (name: string) => name.match(/\.(wav|mp3)$/i)?.[0] ?? "";
  function startRename(r: TrackRow) {
    renaming = r.name;
    deleting = "";
    nameText = r.folder ? r.name : r.name.slice(0, r.name.length - ending(r.name).length);
  }
  function commitRename(r: TrackRow) {
    // A track keeps its ending: the unit tells WAV from MP3 by it.
    const name = nameText.trim() + (r.folder ? "" : ending(r.name));
    renaming = "";
    if (name === r.name || !nameText.trim()) return;
    const problem = nameProblem(name, list.folder);
    if (problem) return say(problem);
    trackFile("rename", pathOf(r), list.folder ? `${list.folder}/${name}` : name);
  }
  function commitFolder() {
    const name = nameText.trim();
    naming = false;
    if (!name) return;
    const problem = nameProblem(name, list.folder);
    if (problem) return say(problem);
    trackFile("new-folder", list.folder ? `${list.folder}/${name}` : name);
  }
  function remove(r: TrackRow) {
    trackFile(r.folder ? "delete-folder" : "delete", pathOf(r));
    deleting = "";
  }
  const focusNow = (el: HTMLInputElement) => {
    el.focus();
    el.select();
  };

  // ---- Sending
  const kb = (bytes: number) => (bytes >= 1048576 ? `${(bytes / 1048576).toFixed(1)} MB` : `${Math.max(1, Math.round(bytes / 1024))} KB`);
  const sending = $derived(!(transfer.done && transfer.index >= transfer.count));
  const share = (part: number, whole: number) => (whole ? (100 * part) / whole : 0);
  // "… is on the unit" stays for a few seconds.
  let now = $state(0);
  onMount(() => {
    const tick = setInterval(() => (now = performance.now()), 500);
    return () => clearInterval(tick);
  });
  const sentShown = $derived(!sending && !transfer.error && transfer.name && /\.(wav|mp3)$/i.test(transfer.name) && now - transfer.at < 4000);
</script>

<div class="tracks">
  {#if !device.files.card}
    <p class="none">No SD card in the unit.</p>
  {:else}
    <input class="search" type="search" placeholder="Find in all folders" aria-label="Find tracks in all folders" bind:value={search} oninput={typed} onkeydown={(e) => e.key === "Escape" && endSearch()} />
    {#if t?.reading}
      <p class="reading">The unit is reading the card: {t.tracks} tracks so far. Folders fill as they are read, and a search finds only what has been read.</p>
    {/if}

    {#if wanted}
      <div class="crumbs">
        <button onclick={endSearch}>‹ Back to the folder</button>
        <span class="count">{hits ? `${hits.length}${trackFound.more ? "+" : ""} found` : "Searching…"}</span>
      </div>
      {#if hits && !hits.length}
        <p class="none">No track has all these words in its name or in the name of a folder it is in.</p>
      {/if}
      <ul>
        {#each hits ?? [] as hit (hit)}
          <li>
            <button class="rowb hit" class:current={hit === t?.path} aria-current={hit === t?.path} title="/TRACKS/{hit}" onclick={() => hit !== t?.path && openTrack(hit)}>
              <span class="pname">{trackTitle(hit)}</span>
              <span class="where">{folderOf(hit) || "Tracks"}</span>
            </button>
          </li>
        {/each}
      </ul>
    {:else}
      <nav class="crumbs" aria-label="Folder">
        <button disabled={!crumbs.length} onclick={() => showFolder("")}>Tracks</button>
        {#each crumbs as c, i}
          <span>/</span>
          <button disabled={i === crumbs.length - 1} onclick={() => showFolder(crumbs.slice(0, i + 1).join("/"))}>{c}</button>
        {/each}
      </nav>
      {#if !list.rows.length}
        <p class="none">{t?.reading ? "Not read yet." : crumbs.length ? "This folder is empty." : "No backing tracks on the card. Send .wav or .mp3 files."}</p>
      {/if}
      <ul>
        {#each list.rows as r (r.name)}
          <li>
            {#if renaming === r.name}
              <form class="naming" onsubmit={(e) => (e.preventDefault(), commitRename(r))}>
                <input aria-label="New name" bind:value={nameText} use:focusNow onkeydown={(e) => e.key === "Escape" && (renaming = "")} />
                <div class="row">
                  <button type="submit" class="primary">Rename</button>
                  <button type="button" onclick={() => (renaming = "")}>Cancel</button>
                </div>
              </form>
            {:else if deleting === r.name}
              <div class="row confirm">
                {#if r.folder}
                  <span class="ask">Delete the folder "{r.name}" from the card? Everything in it goes with it: {r.rows} {r.rows === 1 ? "row" : "rows"}, and what is in the folders among them.</span>
                {:else}
                  <span class="ask">Delete "{trackTitle(r.name)}" from the card?{pathOf(r) === t?.path ? " It is the open track: it is closed first." : ""}</span>
                {/if}
                <button class="danger" onclick={() => remove(r)}>Delete</button>
                <button onclick={() => (deleting = "")}>Keep</button>
              </div>
            {:else if r.folder}
              <button class="rowb" title="/TRACKS/{pathOf(r)}" onclick={() => showFolder(pathOf(r))}>
                <svg viewBox="0 0 16 16" aria-hidden="true"><path d="M1.8 4.2c0-.6.4-1 1-1h3.1l1.5 1.6h5.8c.6 0 1 .4 1 1v6c0 .6-.4 1-1 1H2.8c-.6 0-1-.4-1-1z" /></svg>
                <span class="pname">{r.name}</span>
                <span class="count">{r.rows}</span>
              </button>
              <button class="act" aria-label="Rename {r.name}" title="Rename" onclick={() => startRename(r)}>✎</button>
              <button class="act" aria-label="Delete {r.name}" title="Delete the folder and everything in it" onclick={() => ((deleting = r.name), (renaming = ""))}>×</button>
            {:else}
              <button class="rowb" class:current={pathOf(r) === t?.path} aria-current={pathOf(r) === t?.path} title="/TRACKS/{pathOf(r)}" onclick={() => pathOf(r) !== t?.path && openTrack(pathOf(r))}>
                <span class="pname">{trackTitle(r.name)}</span>
                <span class="kind">{ending(r.name).slice(1).toLowerCase()}</span>
              </button>
              <button class="act" aria-label="Rename {r.name}" title="Rename" onclick={() => startRename(r)}>✎</button>
              <button class="act" aria-label="Delete {r.name}" title="Delete from the card" onclick={() => ((deleting = r.name), (renaming = ""))}>×</button>
            {/if}
          </li>
        {/each}
      </ul>

      <div class="tools">
        {#if trackOffer.replacing.length}
          <div class="row confirm">
            <span class="ask">This folder already has {trackOffer.replacing.length === 1 ? `"${trackOffer.replacing[0]}"` : `${trackOffer.replacing.length} of these tracks`}. Replace?</span>
            <button class="danger" onclick={() => sendTracks(trackOffer.paths, trackOffer.folder)}>Replace</button>
            {#if trackOffer.paths.length > trackOffer.replacing.length}
              <button onclick={() => sendTracks(trackOffer.paths.filter((p) => !trackOffer.replacing.includes(p.split("/").pop() ?? "")), trackOffer.folder)}>Only the new ones</button>
            {/if}
            <button onclick={cancelTrackOffer}>Cancel</button>
          </div>
        {:else if sending}
          <div class="sending">Sending {transfer.name}{transfer.count > 1 ? ` (${transfer.index} of ${transfer.count})` : ""}<br />{kb(transfer.sent)} of {kb(transfer.total)}</div>
          <div class="progress"><div style:width="{share(transfer.sent, transfer.total)}%"></div></div>
          {#if transfer.count > 1}
            <div class="sending all">All {transfer.count}: {kb(transfer.allSent)} of {kb(transfer.allTotal)}</div>
            <div class="progress all"><div style:width="{share(transfer.allSent, transfer.allTotal)}%"></div></div>
          {/if}
          <p class="hint">About 130 KB a second: a 5 MB track takes 40 s. The unit keeps playing meanwhile.</p>
        {:else if naming}
          <form class="naming" onsubmit={(e) => (e.preventDefault(), commitFolder())}>
            <label for="fname">A new folder in {list.folder || "Tracks"}</label>
            <input id="fname" placeholder="Name" bind:value={nameText} use:focusNow onkeydown={(e) => e.key === "Escape" && (naming = false)} />
            <div class="row">
              <button type="submit" class="primary">Make</button>
              <button type="button" onclick={() => (naming = false)}>Cancel</button>
            </div>
          </form>
        {:else}
          {#if sentShown}<div class="sending">{transfer.name} is on the unit.</div>{/if}
          <div class="row">
            <button onclick={() => pickTracks(list.folder)}>Send tracks…</button>
            <button onclick={() => ((naming = true), (nameText = ""), (renaming = ""))}>New folder</button>
          </div>
          <p class="hint">Or drop .wav and .mp3 files on the window: they go into {list.folder ? `"${crumbs[crumbs.length - 1]}"` : "the top folder"}.</p>
        {/if}
      </div>
    {/if}
  {/if}
</div>

<style>
  .tracks {
    flex: 1;
    min-height: 0;
    display: flex;
    flex-direction: column;
  }
  button {
    font: inherit;
    color: inherit;
    cursor: pointer;
  }
  button:disabled {
    cursor: default;
  }
  button:focus-visible {
    outline: 2px solid #ffcf8a;
    outline-offset: 1px;
  }
  input {
    width: 100%;
    box-sizing: border-box;
    padding: 6px 8px;
    font: inherit;
    color: inherit;
    background: #101216;
    border: 1px solid #353b47;
    border-radius: 7px;
    outline: none;
  }
  input:focus {
    border-color: #ffb347;
  }
  .search {
    margin-bottom: 8px;
  }
  .reading {
    margin: 0 2px 8px;
    padding: 6px 8px;
    border-radius: 7px;
    background: #3a2f17;
    color: #ffe2b0;
    font-size: 11.5px;
  }
  .crumbs {
    display: flex;
    flex-wrap: wrap;
    align-items: baseline;
    gap: 2px 4px;
    margin: 0 2px 6px;
    font-size: 12px;
    color: #8a92a0;
  }
  .crumbs button {
    padding: 2px 5px;
    border: 0;
    border-radius: 5px;
    background: none;
    color: #ffb347;
    overflow-wrap: anywhere;
    text-align: left;
  }
  .crumbs button:hover:not(:disabled) {
    background: #1c2027;
  }
  .crumbs button:disabled {
    color: #e6e8ec;
    font-weight: 600;
  }
  .crumbs .count {
    margin-left: auto;
  }
  ul {
    list-style: none;
    margin: 0;
    padding: 0;
    flex: 1;
  }
  li {
    display: flex;
    align-items: center;
  }
  .rowb {
    flex: 1;
    min-width: 0;
    display: flex;
    align-items: baseline;
    gap: 8px;
    padding: 7px 8px;
    border: 0;
    border-radius: 7px;
    background: none;
    text-align: left;
  }
  .rowb:hover {
    background: #1c2027;
  }
  .rowb.current {
    background: #2a2416;
    color: #ffb347;
  }
  .rowb svg {
    flex: none;
    width: 14px;
    height: 14px;
    align-self: center;
    fill: #6f7786;
    stroke: none;
  }
  .rowb.hit {
    flex-direction: column;
    gap: 1px;
  }
  /* Names wrap instead of being cut: their ends tell them apart. */
  .pname {
    flex: 1;
    min-width: 0;
    overflow-wrap: anywhere;
    line-height: 1.3;
    font-weight: 500;
  }
  .count,
  .kind,
  .where {
    font-size: 11px;
    color: #8a92a0;
    font-variant-numeric: tabular-nums;
  }
  .kind {
    text-transform: uppercase;
    letter-spacing: 0.06em;
    font-size: 9.5px;
  }
  .where {
    overflow-wrap: anywhere;
  }
  .act {
    flex: none;
    width: 22px;
    padding: 4px 0;
    border: 0;
    border-radius: 6px;
    background: none;
    color: #8a92a0;
    opacity: 0;
  }
  .act:hover {
    background: #1c2027;
    color: #e6e8ec;
  }
  li:hover .act,
  .act:focus-visible {
    opacity: 1;
  }
  .none {
    margin: 6px 0 0;
    padding: 0 8px;
    color: #8a92a0;
  }
  .row {
    display: flex;
    flex-wrap: wrap;
    align-items: center;
    gap: 6px;
  }
  .row button {
    border: 1px solid #353b47;
    background: #22262e;
    border-radius: 7px;
    padding: 4px 11px;
    line-height: 18px;
    white-space: nowrap;
  }
  .row button:hover {
    background: #2b303a;
    border-color: #454c5a;
  }
  .row .primary {
    border-color: #ffb347;
    color: #ffb347;
  }
  .row .danger {
    border-color: #ff4d4d;
    color: #ff8d8d;
  }
  .confirm {
    flex: 1;
    padding: 6px 8px;
    background: #1c2027;
    border-radius: 7px;
  }
  .ask {
    width: 100%;
    color: #c3c8d1;
    overflow-wrap: anywhere;
  }
  .naming {
    flex: 1;
    padding: 4px 0;
  }
  .naming label {
    display: block;
    font-size: 11.5px;
    color: #8a92a0;
    margin-bottom: 5px;
  }
  .naming input {
    margin-bottom: 8px;
  }
  .tools {
    margin-top: 10px;
    padding: 10px 6px 0;
    border-top: 1px solid #2a2e37;
  }
  .sending {
    padding: 0 2px 6px;
    font-size: 11.5px;
    color: #c3c8d1;
    overflow-wrap: anywhere;
  }
  .sending.all {
    padding-top: 8px;
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
  .progress.all div {
    background: #6f7786;
  }
  .hint {
    margin: 7px 2px 0;
    font-size: 11.5px;
    color: #8a92a0;
  }
</style>
