// Glance3D scene tree — web DOM presenter.
//
// The counterpart of the desktop ImGui tree, and deliberately *only* a presenter: tree shape,
// expansion, filtering, the tri-state visibility roll-up and placeholder numbering are all decided
// by the shared headless view-model inside libf3d (G3DSceneTreeView) and reach us as flat rows.
// Nothing here re-derives any of it, which is what stops the two frontends from drifting the way
// two independent implementations always do.
//
// Rows are fetched one window at a time (`getSceneTreeRows(begin, count)`), so both the DOM work
// and the cost of crossing the wasm boundary stay proportional to what is on screen rather than to
// the size of the model. A 100k-node assembly renders the same ~40 rows a 10-node one does.
//
// Markup and CSS classes match `doc/dev/ui-styleguide.html`'s tree component, whose styles now live
// in the shared `tree.css` that both files load.

const ROW_HEIGHT = 22; // must match .tree.compact { --row-h } in tree.css
const OVERSCAN = 8; // rows rendered beyond the viewport, to hide scroll latency

/** Icon sprite paths, copied from the styleguide's symbol sheet. */
const ICONS = {
  chevron: '<polyline points="9 6 15 12 9 18" />',
  cube:
    '<path d="M12 3l8 4.5v9L12 21l-8-4.5v-9Z" /><path d="M12 21V12" />' +
    '<path d="M12 12l8-4.5" /><path d="M12 12 4 7.5" />',
  folder:
    '<path d="M3 7.5a2 2 0 0 1 2-2h3.6a2 2 0 0 1 1.4.6l1 1a2 2 0 0 0 1.4.6H19a2 2 0 0 1 2 2v6.2a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2Z" />',
  "folder-open":
    '<path d="M4 18.5V6.5a1 1 0 0 1 1-1h3.6a1 1 0 0 1 .7.3l1.2 1.2a1 1 0 0 0 .7.3H18a1 1 0 0 1 1 1V10" />' +
    '<path d="M3.4 18.2 5.6 11a1 1 0 0 1 1-.7h13.2a1 1 0 0 1 1 1.3l-1.8 6a1 1 0 0 1-1 .7H4.4a1 1 0 0 1-1-1.1Z" />',
  layers:
    '<path d="M12 3.5 3.5 8 12 12.5 20.5 8Z" /><path d="M3.7 12.2 12 16.5l8.3-4.3" />',
  camera:
    '<rect x="3" y="7" width="18" height="13" rx="2.5" /><path d="M8 7l1.5-2.5h5L16 7" />' +
    '<circle cx="12" cy="13.5" r="3.2" />',
  light:
    '<path d="M9.5 17.5h5" /><path d="M10 20.5h4" />' +
    '<path d="M12 3.5a6 6 0 0 0-3.8 10.6c.7.6 1.3 1.4 1.3 2.4h5c0-1 .6-1.8 1.3-2.4A6 6 0 0 0 12 3.5Z" />',
  eye: '<path d="M2 12s3.5-6 10-6 10 6 10 6-3.5 6-10 6-10-6-10-6Z" /><circle cx="12" cy="12" r="2.6" />',
  eyeoff:
    '<path d="M2 12s3.5-6 10-6 10 6 10 6-3.5 6-10 6-10-6-10-6Z" /><circle cx="12" cy="12" r="2.6" />' +
    '<line x1="4" y1="4" x2="20" y2="20" />',
  search: '<circle cx="11" cy="11" r="6" /><line x1="20" y1="20" x2="16" y2="16" />',
  // Four diamonds in a diamond: one product, many occurrences. Matches the desktop Component glyph.
  component:
    '<path d="M12 2.6 15.4 6 12 9.4 8.6 6Z" /><path d="M6 8.6 9.4 12 6 15.4 2.6 12Z" />' +
    '<path d="M18 8.6 21.4 12 18 15.4 14.6 12Z" /><path d="M12 14.6 15.4 18 12 21.4 8.6 18Z" />',
};

const svg = (name, className) =>
  `<svg class="${className}" viewBox="0 0 24 24">${ICONS[name] ?? ""}</svg>`;

const escapeHtml = (text) =>
  String(text).replace(
    /[&<>"']/g,
    (c) =>
      ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" })[c],
  );

/**
 * Display text for a row.
 *
 * Localization is the presenter's job by design: the core reports only *that* a node is an unnamed
 * placeholder and which ordinal it carries. The desktop substitutes translated nouns; the web
 * viewer is English-only, so it substitutes literals — same rule, different wording.
 */
function rowLabel(row) {
  if (!row.placeholder) {
    return row.label;
  }
  // The noun follows the node type, and the plural names the section holding the elements — the
  // same rule the desktop applies, only with English literals instead of a translation lookup.
  let noun = row.hasChildren ? "Group" : "Object";
  if (row.type === "camera") {
    noun = row.hasChildren ? "Cameras" : "Camera";
  } else if (row.type === "light") {
    noun = row.hasChildren ? "Lights" : "Light";
  }
  return row.placeholderOrdinal > 0 ? `${noun} ${row.placeholderOrdinal}` : noun;
}

/** Row icon by what the node *is*, rather than by where it sits in the tree. */
function rowIcon(row) {
  if (row.type === "file") {
    return { name: "layers", variant: "root" };
  }
  if (row.type === "camera") {
    return { name: "camera", variant: "" };
  }
  if (row.type === "light") {
    return { name: "light", variant: "light" };
  }
  // Before the folder rule: an occurrence has children, but it points at a product rather than
  // owning what is under it, and in a CAD assembly that is the distinction worth seeing.
  if (row.type === "instance") {
    return { name: "component", variant: row.hasChildren ? "folder" : "" };
  }
  if (row.hasChildren) {
    return { name: row.expanded ? "folder-open" : "folder", variant: "folder" };
  }
  return { name: "cube", variant: "" };
}

function rowMarkup(row) {
  let rails = "";
  for (let i = 0; i < row.depth; i++) {
    rails += '<span class="tree-rail"></span>';
  }

  const twisty = row.hasChildren
    ? `<span class="tree-twisty" data-hit="twisty">${svg("chevron", "icon")}</span>`
    : '<span class="tree-twisty leaf"></span>';

  const icon = rowIcon(row);
  const ticon = svg(
    icon.name,
    `icon tree-ticon${icon.variant ? ` ${icon.variant}` : ""}`,
  );

  // What an occurrence points at beats its child count: the count is visible from the twisty, while
  // the product name is the only thing on the row that is not already on screen. The view-model
  // decides when the target says more than the label, so both frontends show it in the same places.
  const metaText = row.instanceTarget || (row.hasChildren ? row.childCount : "");
  const meta =
    metaText === ""
      ? ""
      : `<span class="tree-meta">${escapeHtml(metaText)}</span>`;

  // A partially visible group reads as shown, not hidden — the eye carries the mixed state.
  // Cameras get no eye at all: the view-model decides that, so both frontends agree without each
  // re-deriving it from the node type.
  const eye = row.visible || row.partiallyVisible ? "eye" : "eyeoff";
  const actions = row.canToggleVisibility
    ? `<span class="tree-actions"><button class="tree-act vis${row.visible ? "" : " on"}" ` +
      `data-hit="visibility" title="Visibility">${svg(eye, "icon")}</button></span>`
    : "";

  const classes = ["tree-row"];
  if (!row.expanded && row.hasChildren) classes.push("collapsed");
  if (row.selected) classes.push("selected", "focused");
  // Only a fully hidden row is dimmed; a partial group still reads as present.
  if (!row.visible && !row.partiallyVisible) classes.push("hidden");
  if (row.type === "file") classes.push("group");

  // Carried on the element so the selection callback can hand the panel what it already fetched,
  // rather than asking the core a second time for a fact that arrived with the row.
  const target = row.instanceTarget
    ? ` data-instance-target="${escapeHtml(row.instanceTarget)}"`
    : "";

  return (
    `<div class="${classes.join(" ")}" data-depth="${row.depth}"${target} ` +
    `data-path="${escapeHtml(row.path)}" title="${escapeHtml(row.path)}">` +
    `${rails}${twisty}${ticon}` +
    `<span class="tree-label">${escapeHtml(rowLabel(row))}</span>` +
    `${meta}${actions}</div>`
  );
}

/**
 * Mount the scene tree into `hostEl`.
 *
 * @param {HTMLElement} hostEl container the tree owns entirely
 * @param {object} engine the libf3d engine instance (Module.engineInstance)
 * @param {{onSelect?: (path: string, row: {instanceTarget: string}) => void}} [callbacks] notified
 *   when the selection changes, so a sibling panel can follow it without the tree having to know
 *   that panel exists
 * @returns {{refresh: () => void, isSupported: () => boolean}}
 */
export function initG3DSceneTree(hostEl, engine, callbacks = {}) {
  const noop = { refresh: () => {}, isSupported: () => false };
  if (!hostEl || !engine || typeof engine.getScene !== "function") {
    return noop;
  }

  // Guarded the same way the data-info group is: a wasm built before these bindings existed
  // degrades to a hidden tree instead of throwing, and a rebuild activates it with no code change.
  const probe = engine.getScene();
  if (!probe || typeof probe.getSceneTreeInfo !== "function") {
    hostEl.hidden = true;
    return noop;
  }

  hostEl.classList.add("g3d-scene-tree");
  hostEl.innerHTML =
    '<div class="tree-toolbar">' +
    '<span class="ttitle">Scene</span>' +
    `<span class="tsearch">${svg("search", "icon")}` +
    '<input type="search" placeholder="Filter" aria-label="Filter scene tree" /></span>' +
    "</div>" +
    '<div class="tree-scroll" tabindex="0">' +
    '<div class="g3d-scene-tree__sizer"><div class="tree compact"></div></div>' +
    "</div>" +
    '<div class="tree-foot"><span class="count"></span></div>';

  const scrollEl = hostEl.querySelector(".tree-scroll");
  const sizerEl = hostEl.querySelector(".g3d-scene-tree__sizer");
  const listEl = hostEl.querySelector(".tree");
  const footEl = hostEl.querySelector(".tree-foot .count");
  const searchEl = hostEl.querySelector(".tsearch input");

  let rowCount = 0;
  let renderedFirst = -1;
  let renderedLast = -1;

  const scene = () => engine.getScene();

  /**
   * Draw the window of rows the scroll position exposes.
   *
   * The sizer div carries the full height so the scrollbar is honest, while only the visible slice
   * exists in the DOM, offset into place. `force` re-fetches even when the window did not move,
   * which is what a state change (expansion, visibility, filter) needs.
   */
  const renderWindow = (force = false) => {
    const viewportRows = Math.ceil(scrollEl.clientHeight / ROW_HEIGHT);
    const first = Math.max(
      0,
      Math.floor(scrollEl.scrollTop / ROW_HEIGHT) - OVERSCAN,
    );
    const last = Math.min(rowCount, first + viewportRows + OVERSCAN * 2);

    if (!force && first === renderedFirst && last === renderedLast) {
      return;
    }
    renderedFirst = first;
    renderedLast = last;

    const rows = last > first ? scene().getSceneTreeRows(first, last - first) : [];
    listEl.style.transform = `translateY(${first * ROW_HEIGHT}px)`;
    listEl.innerHTML = rows.map(rowMarkup).join("");
  };

  /** Re-read the row count and redraw. Call after anything that can change the tree. */
  const refresh = () => {
    let info = null;
    try {
      info = scene().getSceneTreeInfo();
    } catch {
      // Leaving the previous scene's rows on screen would be worse than showing nothing: they
      // would look live and every path in them would now resolve to nothing.
      info = null;
    }
    rowCount = info ? info.rowCount : 0;
    sizerEl.style.height = `${rowCount * ROW_HEIGHT}px`;
    if (!info) {
      footEl.textContent = "Scene tree unavailable";
    } else if (rowCount === 0) {
      footEl.textContent = "Empty scene";
    } else {
      const rowWord = rowCount === 1 ? "row" : "rows";
      const nodeWord = info.nodeCount === 1 ? "node" : "nodes";
      footEl.textContent = `${rowCount} ${rowWord} · ${info.nodeCount} ${nodeWord}`;
    }
    renderWindow(true);
  };

  scrollEl.addEventListener("scroll", () => renderWindow(), { passive: true });

  // Row interaction, by delegation: rows come and go as the window scrolls, so per-row listeners
  // would have to be rebound constantly.
  listEl.addEventListener("click", (evt) => {
    const rowEl = evt.target.closest(".tree-row");
    if (!rowEl) {
      return;
    }
    const path = rowEl.dataset.path;
    const hitEl = evt.target.closest("[data-hit]");
    const hit = hitEl ? hitEl.dataset.hit : "row";

    if (hit === "twisty") {
      // Expansion is view state: it never touches the scene, so no re-render is needed.
      const expanded = !rowEl.classList.contains("collapsed");
      scene().setSceneTreeExpanded(path, !expanded);
    } else if (hit === "visibility") {
      // Visibility *is* scene data. Partially visible groups turn fully on, as every outliner does.
      const partial = !rowEl.classList.contains("hidden");
      scene().setSceneTreeNodeVisibility(path, !partial);
    } else {
      scene().setSceneTreeSelection(path);
      // A camera node's only purpose is to be looked through, so selecting one activates it — the
      // same single-click behaviour a viewpoint list has. Returns false for anything else, which
      // leaves the click as a plain selection.
      scene().activateSceneTreeNode(path);
      callbacks.onSelect?.(path, {
        instanceTarget: rowEl.dataset.instanceTarget ?? "",
      });
    }
    refresh();
  });

  listEl.addEventListener("dblclick", (evt) => {
    const rowEl = evt.target.closest(".tree-row");
    if (rowEl) {
      scene().focusSceneTreeNode(rowEl.dataset.path);
    }
  });

  let filterTimer = 0;
  searchEl.addEventListener("input", () => {
    // Debounced: each keystroke rebuilds the row list over the whole graph.
    window.clearTimeout(filterTimer);
    filterTimer = window.setTimeout(() => {
      scene().setSceneTreeFilter(searchEl.value, false);
      scrollEl.scrollTop = 0;
      refresh();
    }, 120);
  });

  // The scroll viewport has no intrinsic height until the panel opens, so the first window would
  // otherwise be computed against a height of zero.
  if (typeof ResizeObserver === "function") {
    new ResizeObserver(() => renderWindow(true)).observe(scrollEl);
  }

  window.addEventListener("g3d:scene-loaded", refresh);
  refresh();

  return { refresh, isSupported: () => true };
}
