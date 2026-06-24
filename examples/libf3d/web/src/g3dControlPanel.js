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
  const controlsEl = document.querySelector("#g3d-controls");

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

  // Appearance / Material control groups — the web counterpart of the desktop inspector groups.
  // Controls read the current option value and write changes straight to the shared libf3d options
  // (getAsString/setAsString/toggle), then request a render so the change applies immediately.
  const getStr = (name) => {
    try {
      return options.getAsString(name);
    } catch {
      return null; // unset-optional / unknown
    }
  };
  const getBool = (name, fallback) => {
    const v = getStr(name);
    return v === null ? fallback : v === "true" || v === "1";
  };
  const getFloat = (name, fallback) => {
    const v = parseFloat(getStr(name));
    return Number.isFinite(v) ? v : fallback;
  };
  const rgbToHex = (name, fallback) => {
    const v = getStr(name);
    if (!v) return fallback;
    const p = v.split(",").map((x) => parseFloat(x));
    if (p.length < 3 || p.some((x) => !Number.isFinite(x))) return fallback;
    return (
      "#" +
      p
        .slice(0, 3)
        .map((x) =>
          Math.round(Math.max(0, Math.min(1, x)) * 255)
            .toString(16)
            .padStart(2, "0"),
        )
        .join("")
    );
  };
  const hexToRgb = (hex) => {
    const m = /^#?([0-9a-f]{6})$/i.exec(hex);
    if (!m) return "0,0,0";
    const n = parseInt(m[1], 16);
    return [(n >> 16) & 255, (n >> 8) & 255, n & 255]
      .map((c) => Number((c / 255).toFixed(4)))
      .join(",");
  };
  const setOpt = (name, value) => {
    try {
      options.setAsString(name, value);
      engine.getWindow().render();
    } catch {
      /* ignore unknown options on stale wasm */
    }
  };

  let ctlUid = 0;
  const toggleRow = (label, name) => {
    const id = `g3d-ctl-${ctlUid++}`;
    const row = el("div", "g3d-controls__row");
    const input = el("input");
    input.type = "checkbox";
    input.id = id;
    input.className = "switch is-rounded is-small";
    input.checked = getBool(name, false);
    input.addEventListener("change", () => setOpt(name, input.checked ? "true" : "false"));
    const lab = el("label", "g3d-controls__label", label);
    lab.setAttribute("for", id);
    row.append(input, lab);
    return row;
  };
  const sliderRow = (label, name, fallback) => {
    const row = el("div", "g3d-controls__row g3d-controls__slider");
    const input = el("input");
    input.type = "range";
    input.min = "0";
    input.max = "1";
    input.step = "0.01";
    input.value = String(getFloat(name, fallback));
    const valEl = el("span", "g3d-controls__value", Number(input.value).toFixed(2));
    input.addEventListener("input", () => {
      valEl.textContent = Number(input.value).toFixed(2);
      setOpt(name, input.value);
    });
    row.append(el("span", "g3d-controls__label", label), input, valEl);
    return row;
  };
  const colorRow = (label, name, fallback) => {
    const row = el("div", "g3d-controls__row g3d-controls__color");
    const input = el("input");
    input.type = "color";
    input.value = rgbToHex(name, fallback);
    input.addEventListener("input", () => setOpt(name, hexToRgb(input.value)));
    row.append(input, el("span", "g3d-controls__label", label));
    return row;
  };

  const renderControls = () => {
    if (!controlsEl) {
      return;
    }
    controlsEl.replaceChildren();
    controlsEl.append(el("p", "g3d-controls__section", "Appearance"));
    controlsEl.append(toggleRow("Show edges", "render.show_edges"));
    controlsEl.append(toggleRow("Grid", "render.grid.enable"));
    controlsEl.append(toggleRow("Ambient occlusion", "render.effect.ambient_occlusion"));
    controlsEl.append(toggleRow("Anti-aliasing", "render.effect.antialiasing.enable"));
    controlsEl.append(toggleRow("Tone mapping", "render.effect.tone_mapping"));
    controlsEl.append(colorRow("Background", "render.background.color", "#333333"));
    controlsEl.append(el("p", "g3d-controls__section", "Material"));
    controlsEl.append(sliderRow("Metallic", "model.material.metallic", 0));
    controlsEl.append(sliderRow("Roughness", "model.material.roughness", 0.3));
    controlsEl.append(sliderRow("Opacity", "model.color.opacity", 1));
    controlsEl.append(colorRow("Base color", "model.color.rgb", "#ffffff"));
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
      renderControls(); // and reflect current option values into the controls
    }
  };

  // Refresh the inspector when a new file finishes loading, if the panel is open (main.js dispatches
  // this after a successful load). Decoupled via a window event so the presenter stays standalone.
  window.addEventListener("g3d:scene-loaded", () => {
    if (isOpen()) {
      renderDataInfo();
      renderControls();
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
