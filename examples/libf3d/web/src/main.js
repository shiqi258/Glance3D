import "bulma/css/bulma.min.css";
import "bulma-switch/dist/css/bulma-switch.min.css";
import {
  installBrowserConsoleLogger,
  recordBrowserLog,
} from "./browser-console-log.js";
import f3d from "f3d";

installBrowserConsoleLogger();

const logViewerState = (message, details = {}) => {
  console.debug("[viewer]", message, details);
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

    const openFile = (file) => {
      const { name, path, stream } = file;
      document.getElementById("file-name").innerHTML = name;
      const scene = Module.engineInstance.getScene();
      scene.clear();
      try {
        if (path) {
          scene.add(path);
        } else {
          scene.addBuffer(stream);
        }
        currentFile = file;
        logViewerState("scene loaded", {
          name,
          path,
          bytes: stream?.byteLength ?? stream?.length ?? 0,
        });
      } catch (e) {
        document.getElementById("file-name").innerHTML =
          '<strong class="has-text-danger">Unsupported file</strong>';
        logViewerState("scene load failed", {
          name,
          path,
          error: e?.message ?? String(e),
        });
      }
      Module.engineInstance.getWindow().getCamera().resetToBounds(0.9);
      Module.engineInstance.getWindow().render();
      logViewerState("scene rendered", { name });
    };

    // setup file open event
    const progressEl = document.querySelector("#progressEl");
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
