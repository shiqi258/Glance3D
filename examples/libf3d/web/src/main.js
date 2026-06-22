import "bulma/css/bulma.min.css";
import "bulma-switch/dist/css/bulma-switch.min.css";
import {
  installBrowserConsoleLogger,
  recordBrowserLog,
} from "./browser-console-log.js";
import f3d from "f3d";

installBrowserConsoleLogger();

const logViewerState = (message, details = {}) => {
  // Fold `details` into the message string. The CDP DevTools mirror records
  // console args by their RemoteObject `description`, which collapses any plain
  // object to a bare "Object" — so an object passed as a separate arg loses all
  // its fields in g3d_*_web.log. Serializing here keeps the values legible in
  // both log channels (CDP mirror + in-page JSONL hook).
  let detailText = "";
  if (details && Object.keys(details).length > 0) {
    try {
      detailText = " " + JSON.stringify(details);
    } catch {
      detailText = " [unserializable details]";
    }
  }
  console.debug("[viewer]", message + detailText);
};

const mapF3DLogLevel = (Module, level) => {
  switch (level) {
    case Module.LogVerboseLevel.DEBUG:
      return "debug";
    case Module.LogVerboseLevel.INFO:
      return "info";
    case Module.LogVerboseLevel.WARN:
      return "warn";
    case Module.LogVerboseLevel.ERROR:
      return "error";
    default:
      return "log";
  }
};

const installF3DLogForwarding = (Module) => {
  if (!Module.Log?.forward || !Module.LogVerboseLevel) {
    return;
  }

  Module.Log.forward((level, message) => {
    try {
      const mappedLevel = mapF3DLogLevel(Module, level);
      recordBrowserLog(mappedLevel, "f3d", [message], {
        f3dLevel: level,
        f3dLevelName: mappedLevel,
        stack: undefined,
      });
    } catch {
      // Keep F3D's native logging path isolated from the browser log bridge.
    }
  });
};

const settings = {
  canvas: document.getElementById("canvas"),
  setupOptions: (options) => {
    // background must be set to black for proper blending with transparent canvas
    options.setAsString("render.background.color", "#000000");

    // make it look nice
    options.setAsString("model.color.rgb", "0.9,0.32,0.18");
    options.setAsString("model.unlit", "true");
    options.setAsString("render.show_edges", "true");
    options.toggle("render.effect.antialiasing.enable");
    options.toggle("render.effect.tone_mapping");
    options.toggle("render.effect.ambient_occlusion");
    options.toggle("render.hdri.ambient");

    // display widgets
    options.toggle("ui.axis");
    options.toggle("render.grid.enable");

    // default to +Z
    options.setAsString("scene.up_direction", "+Z");
  },
};

f3d(settings)
  .then(async (Module) => {
    installF3DLogForwarding(Module);

    logViewerState("runtime initialized", {
      canvasFound: Boolean(settings.canvas),
      devicePixelRatio: window.devicePixelRatio,
      // Cross-origin isolation gates SharedArrayBuffer, which the threaded (pthreads) wasm build
      // needs to run the parse off the main thread. If false here, the threaded load path is skipped.
      crossOriginIsolated: self.crossOriginIsolated === true,
      sharedArrayBuffer: typeof SharedArrayBuffer !== "undefined",
    });

    // write in the filesystem
    const defaultModelName = "f3d.obj";
    const defaultResponse = await fetch(defaultModelName);
    if (!defaultResponse.ok) {
      throw new Error(
        `Failed to fetch default model: ${defaultResponse.status} ${defaultResponse.statusText}`,
      );
    }
    const defaultFile = new Uint8Array(await defaultResponse.arrayBuffer());
    Module.FS.writeFile(defaultModelName, defaultFile);
    logViewerState("default model fetched", {
      bytes: defaultFile.byteLength,
      contentType: defaultResponse.headers.get("content-type"),
    });

    // automatically load all supported file format readers
    Module.Engine.autoloadPlugins();

    Module.engineInstance = Module.Engine.create();

    let currentFile = {
      name: defaultModelName,
      path: defaultModelName,
      stream: defaultFile,
    };

    // ----------------------------------------------------------------------------------------
    // Loading-progress layer — pluggable presenter architecture (see plan: 统一加载进度层)
    //
    // The shared, headless progress MODEL lives in the libf3d core and is identical across
    // frontends: `scene.getAsyncState()` (IDLE/LOADING/READY/FAILED) + `scene.getAsyncProgress()`
    // ([0,1], covers only the CPU parse/build phase). Both desktop (C++) and this web viewer plug
    // into that same model; each owns its own PRESENTER (animation / position / copy are
    // intentionally frontend-specific and swappable).
    //
    // The desktop presenter is a centered ImGui overlay (logo + rotating halo + progress ring).
    // The web build has F3D_MODULE_UI=OFF (no ImGui), so the web presenter must be DOM/CSS/JS.
    //
    // TODO(web-loader): replace the sidebar Bulma bar below with a centered DOM overlay presenter,
    // e.g. `createLoadingOverlay({ host: '#main' })` exposing { show(context), update(progress,
    // phase), hide() }, mounted over #canvas. Drive it from the same `onProgress(fraction)`
    // callback passed to addBufferAsyncThreaded() (derive FINALIZING from fraction >= 1), and call
    // show()/hide() in openFile()'s try/finally. To stay consistent with desktop, the presenter
    // should implement the shared BEHAVIOR SPEC: ~300ms show-delay (no flash on fast loads), map
    // raw[0..1]→display[0..0.9] reserving the last 10% for finalize, monotonic clamp (never
    // decrease), and phase→localized copy ("Loading {file}…" / "Parsing…" / "Almost done…").
    // ----------------------------------------------------------------------------------------

    // Current (interim) presenter: the sidebar Bulma progress bar, driven by the threaded async
    // loader (fraction in [0, 1]). Kept until the centered DOM overlay above is implemented.
    const loadProgressEl = document.querySelector("#load-progress");
    const setLoadProgress = (fraction) => {
      if (!loadProgressEl) {
        return;
      }
      loadProgressEl.hidden = false;
      loadProgressEl.value = Math.round((fraction ?? 0) * 100);
    };
    const hideLoadProgress = () => {
      if (loadProgressEl) {
        loadProgressEl.hidden = true;
      }
    };

    const openFile = async (file) => {
      const { name, path, stream } = file;
      document.getElementById("file-name").innerHTML = name;
      const scene = Module.engineInstance.getScene();
      scene.clear();
      // The threaded wasm build parses on a worker thread so the browser main thread (DOM, progress
      // bar) stays responsive. It needs a cross-origin-isolated context (COOP/COEP) for the pthreads
      // to initialize, so feature-detect both that and the threaded loader before using it.
      const useThreadedLoad =
        !path &&
        self.crossOriginIsolated === true &&
        typeof scene.addBufferAsyncThreaded === "function";
      try {
        if (path) {
          scene.add(path);
        } else if (useThreadedLoad) {
          logViewerState("scene loading (threaded, off-main-thread)", { name });
          await scene.addBufferAsyncThreaded(stream, { fileName: name }, setLoadProgress);
        } else if (typeof scene.addBufferAsync === "function") {
          // Route uploaded buffers through the wrapper's async loader: it writes
          // them to the in-memory filesystem and loads by file name (extension-
          // based reader detection). The raw addBuffer memory path throws
          // "No force reader set ..." on the bundled VTK (< 9.6.20260128).
          await scene.addBufferAsync(stream, { fileName: name });
        } else {
          logViewerState("addBufferAsync unavailable, using raw addBuffer", {
            name,
          });
          scene.addBuffer(stream);
        }
        currentFile = file;
        logViewerState("scene loaded", {
          name,
          path,
          bytes: stream?.byteLength ?? stream?.length ?? 0,
          threaded: useThreadedLoad,
        });
      } catch (e) {
        document.getElementById("file-name").innerHTML =
          '<strong class="has-text-danger">Unsupported file</strong>';
        logViewerState("scene load failed", {
          name,
          path,
          error: e?.message ?? String(e),
        });
      } finally {
        hideLoadProgress();
      }
      Module.engineInstance.getWindow().getCamera().resetToBounds(0.9);
      Module.engineInstance.getWindow().render();
      logViewerState("scene rendered", { name });
    };

    // setup file open event
    const fileSelector = document.querySelector("#file-selector");
    fileSelector.addEventListener("change", (evt) => {
      for (const file of evt.target.files) {
        const reader = new FileReader();
        reader.addEventListener("loadend", (e) => {
          openFile({ name: file.name, stream: new Uint8Array(reader.result) });
        });
        reader.readAsArrayBuffer(file);
      }
    });

    Module.setupOptions(Module.engineInstance.getOptions());

    // Storing DOM element ids to f3d option mappings since also useful for url-param parsing
    const idOptionMappings = [
      ["grid", "render.grid.enable"],
      ["axis", "ui.axis"],
      ["fxaa", "render.effect.antialiasing.enable"],
      ["tone", "render.effect.tone_mapping"],
      ["ssao", "render.effect.ambient_occlusion"],
      ["ambient", "render.hdri.ambient"],
    ];

    // toggle callback
    const mapToggleIdToOption = (id, option) => {
      document.querySelector("#" + id).addEventListener("change", (evt) => {
        Module.engineInstance.getOptions().toggle(option);
        Module.engineInstance.getWindow().render();
      });
    };

    // This assumes all toggles are 'on' before mapping their state to options
    // Ok after f3d(settings) where settings = {..., setupOptions} which toggles some options
    for (let [id, option] of idOptionMappings) {
      mapToggleIdToOption(id, option);
    }

    const switchDark = () => {
      document.documentElement.classList.add("theme-dark");
      document.documentElement.classList.remove("theme-light");
      Module.engineInstance
        .getOptions()
        .setAsString("render.grid.color", "0.25, 0.27, 0.33");
      Module.engineInstance.getWindow().render();
    };

    const switchLight = () => {
      document.documentElement.classList.add("theme-light");
      document.documentElement.classList.remove("theme-dark");
      Module.engineInstance
        .getOptions()
        .setAsString("render.grid.color", "0.67, 0.69, 0.75");
      Module.engineInstance.getWindow().render();
    };

    // theme switch
    document.querySelector("#dark").addEventListener("change", (evt) => {
      if (evt.target.checked) switchDark();
      else switchLight();
    });

    switchDark();

    // up callback
    document.querySelector("#z-up").addEventListener("click", (evt) => {
      Module.engineInstance
        .getOptions()
        .setAsString("scene.up_direction", "+Z");
      document.getElementById("z-up").classList.add("is-active");
      document.getElementById("y-up").classList.remove("is-active");
      openFile(currentFile);
    });

    document.querySelector("#y-up").addEventListener("click", (evt) => {
      Module.engineInstance
        .getOptions()
        .setAsString("scene.up_direction", "+Y");
      document.getElementById("y-up").classList.add("is-active");
      document.getElementById("z-up").classList.remove("is-active");
      openFile(currentFile);
    });

    // setup the window size based on the canvas size
    const main = document.getElementById("main");
    const scale = window.devicePixelRatio;
    Module.engineInstance
      .getWindow()
      .setSize(scale * main.clientWidth, scale * main.clientHeight);
    logViewerState("window sized", {
      mainClientWidth: main.clientWidth,
      mainClientHeight: main.clientHeight,
      renderWidth: scale * main.clientWidth,
      renderHeight: scale * main.clientHeight,
    });

    openFile(currentFile);

    // do a first render and start the interactor
    Module.engineInstance.getWindow().render();
    Module.engineInstance.getInteractor().start();
    logViewerState("interactor started");
  })
  .catch((error) => console.error("Internal exception: " + error));
