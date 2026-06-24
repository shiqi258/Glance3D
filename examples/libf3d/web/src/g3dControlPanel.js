// Glance3D control panel — web DOM presenter.
//
// This mirrors the desktop architecture: the *state* ("is the panel open") is the shared, headless
// libf3d option `ui.control_panel`; each frontend owns a swappable presenter. The web build is
// F3D_MODULE_UI=OFF (no ImGui drawn on the canvas), so this DOM/CSS presenter owns the visuals.
//
// A floating toggle button (FAB) and the `` ` `` (backtick) key both flip the state; the FAB
// auto-hides after the viewport goes idle (presentation only, not part of the shared state).

const PANEL_OPTION = "ui.control_panel";
const IDLE_HIDE_MS = 2500;

/**
 * Wire up the control panel DOM presenter.
 * @param {object} engine the libf3d engine instance (Module.engineInstance)
 */
export function initG3DControlPanel(engine) {
  const fab = document.querySelector("#g3d-control-fab");
  const panel = document.querySelector("#g3d-control-panel");
  const collapseBtn = document.querySelector("#g3d-control-collapse");
  const host = document.querySelector("#main");
  if (!fab || !panel || !host) {
    return;
  }

  const options = engine.getOptions();
  const dataInfoEl = document.querySelector("#g3d-data-info");

  // Read-only data-info group, sourced from the shared `scene.getG3DDataInfo()` API (same data the
  // desktop inspector shows). Guarded so a wasm built before this binding existed degrades to an
  // empty group instead of throwing; a rebuild (`npm run build`) activates it with no code change.
  const fmt = (x) =>
    Number.isFinite(x) ? Number(x.toPrecision(4)).toString() : String(x);
  const el = (tag, className, text) => {
    const node = document.createElement(tag);
    if (className) node.className = className;
    if (text != null) node.textContent = text;
    return node;
  };
  const infoRow = (key, value) => {
    const row = el("div", "g3d-data-info__row");
    row.append(el("span", "g3d-data-info__key", key), el("span", "g3d-data-info__val", value));
    return row;
  };
  const renderDataInfo = () => {
    if (!dataInfoEl || typeof engine.getScene !== "function") {
      return;
    }
    const scene = engine.getScene();
    if (!scene || typeof scene.getG3DDataInfo !== "function") {
      return; // stale wasm: leave the group empty
    }
    let info;
    try {
      info = scene.getG3DDataInfo();
    } catch {
      return;
    }
    dataInfoEl.replaceChildren();
    dataInfoEl.append(el("p", "g3d-data-info__section", "Data info"));
    dataInfoEl.append(infoRow("Points", String(info.points)));
    dataInfoEl.append(infoRow("Cells", String(info.cells)));
    dataInfoEl.append(infoRow("Actors", String(info.actors)));
    if (info.files > 1) {
      dataInfoEl.append(infoRow("Files", String(info.files)));
    }
    if (info.hasBounds && info.bounds) {
      const b = info.bounds; // {xmin,xmax,ymin,ymax,zmin,zmax}
      dataInfoEl.append(
        infoRow("Size", `${fmt(b[1] - b[0])} x ${fmt(b[3] - b[2])} x ${fmt(b[5] - b[4])}`),
      );
    }
    const arrays = info.arrays || [];
    if (arrays.length === 0) {
      dataInfoEl.append(el("p", "g3d-data-info__empty", "No data arrays"));
    } else {
      dataInfoEl.append(el("p", "g3d-data-info__section", "Arrays"));
      for (const array of arrays) {
        const wrap = el("div", "g3d-data-info__array");
        wrap.append(el("div", "g3d-data-info__array-name", array.name));
        wrap.append(
          el(
            "div",
            "g3d-data-info__array-meta",
            `${array.association} · ${array.components}c  [${fmt(array.range[0])}, ${fmt(array.range[1])}]`,
          ),
        );
        dataInfoEl.append(wrap);
      }
    }
  };

  // Prefer the shared, headless libf3d option as the single source of truth (matches desktop). The
  // bundled wasm may predate this option (built before it existed); detect that up front by listing
  // the known option names — which never touches the missing option — and fall back to a DOM-only
  // open state so a stale wasm can never break the viewer. Rebuilding the wasm (`npm run build`)
  // makes the shared option authoritative automatically, with no code change here.
  let useSharedOption = false;
  try {
    const names = options.getNames();
    useSharedOption = Array.isArray(names) && names.includes(PANEL_OPTION);
  } catch {
    useSharedOption = false;
  }

  let domOpen = false; // used only when the shared option is unavailable

  const isOpen = () =>
    useSharedOption ? options.getAsString(PANEL_OPTION) === "true" : domOpen;

  // Reflect the current state into the DOM.
  const sync = () => {
    const open = isOpen();
    panel.classList.toggle("is-open", open);
    // Expand the right grid column on #main: this physically pushes (shrinks) the canvas, and the
    // ResizeObserver on the canvas (main.js) re-sizes the WebGL render so the 3D view re-fits as it
    // narrows — the web counterpart of the desktop viewport "push".
    host.classList.toggle("g3d-pushing", open);
    fab.classList.toggle("is-active", open);
    fab.setAttribute("aria-pressed", String(open));
    if (open) {
      // Never auto-hide the FAB while the panel is open.
      fab.classList.remove("is-idle");
      renderDataInfo(); // refresh the read-only data each time the panel is shown
    }
  };

  // Refresh the data-info when a new file finishes loading, if the panel is open (main.js dispatches
  // this after a successful load). Decoupled via a window event so the presenter stays standalone.
  window.addEventListener("g3d:scene-loaded", () => {
    if (isOpen()) {
      renderDataInfo();
    }
  });

  const toggle = () => {
    if (useSharedOption) {
      options.toggle(PANEL_OPTION);
    } else {
      domOpen = !domOpen;
    }
    sync();
  };

  fab.addEventListener("click", toggle);
  if (collapseBtn) {
    collapseBtn.addEventListener("click", toggle);
  }

  // Hotkey: backtick toggles the panel. Capture phase + stop propagation so the key never reaches
  // the canvas / wasm VTK interactor (which also binds `grave` -> `toggle ui.control_panel`).
  // Without this the shared option would be flipped twice per press (here + in wasm), cancelling out.
  // Same focus-scoped contract as the desktop IME handling: a shortcut must not fire while a text
  // field is focused or an IME is composing, so the key falls through to text input. The browser
  // scopes the IME per element for free; we only owe this guard. (No text fields exist in the panel
  // yet, so this is forward-looking — harmless today.)
  const isEditableTarget = (el) =>
    el instanceof HTMLElement &&
    (el.isContentEditable ||
      el.tagName === "INPUT" ||
      el.tagName === "TEXTAREA" ||
      el.tagName === "SELECT");
  window.addEventListener(
    "keydown",
    (evt) => {
      if (evt.key === "`" && !evt.repeat && !evt.isComposing && !isEditableTarget(evt.target)) {
        evt.preventDefault();
        evt.stopImmediatePropagation();
        evt.stopPropagation();
        toggle();
      }
    },
    true,
  );

  // Idle auto-hide for the FAB (presentation only).
  let idleTimer = null;
  const markActive = () => {
    fab.classList.remove("is-idle");
    if (idleTimer) {
      clearTimeout(idleTimer);
    }
    idleTimer = setTimeout(() => {
      if (!isOpen()) {
        fab.classList.add("is-idle");
      }
    }, IDLE_HIDE_MS);
  };
  host.addEventListener("pointermove", markActive);

  sync();
  markActive();
}
