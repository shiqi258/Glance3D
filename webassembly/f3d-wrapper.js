import createCoreModule from "./f3d-core.js";

const GLB_MAGIC = "glTF";
const GLB_JSON_CHUNK_TYPE = "JSON";
const ADVANCED_GLTF_EXTENSIONS = new Set([
  "EXT_meshopt_compression",
  "KHR_texture_basisu",
  "KHR_mesh_quantization",
]);

let capabilityManifestPromise;
let advancedGltfPromise;
let preparedFileCounter = 0;
const preparedFilePaths = [];

function readAscii(bytes, offset, length) {
  return Array.from(bytes.slice(offset, offset + length), (byte) =>
    String.fromCharCode(byte),
  ).join("");
}

function readUint32LE(bytes, offset) {
  return (
    bytes[offset] |
    (bytes[offset + 1] << 8) |
    (bytes[offset + 2] << 16) |
    (bytes[offset + 3] << 24)
  ) >>> 0;
}

function toUint8Array(buffer) {
  if (buffer instanceof Uint8Array) {
    return buffer;
  }

  if (buffer instanceof ArrayBuffer) {
    return new Uint8Array(buffer);
  }

  if (ArrayBuffer.isView(buffer)) {
    return new Uint8Array(buffer.buffer, buffer.byteOffset, buffer.byteLength);
  }

  return new Uint8Array(buffer);
}

function defaultBaseUrl() {
  return new URL(".", import.meta.url).href;
}

function resolveRuntimeUrl(Module, path) {
  if (typeof Module.locateFile === "function") {
    return Module.locateFile(path, defaultBaseUrl());
  }

  return new URL(path, defaultBaseUrl()).href;
}

function parseGlbJson(bytes) {
  if (bytes.byteLength < 20 || readAscii(bytes, 0, 4) !== GLB_MAGIC) {
    return undefined;
  }

  const declaredLength = readUint32LE(bytes, 8);
  if (declaredLength > bytes.byteLength) {
    throw new Error(
      `Invalid GLB length: declared ${declaredLength}, got ${bytes.byteLength}`,
    );
  }

  let offset = 12;
  while (offset + 8 <= bytes.byteLength) {
    const chunkLength = readUint32LE(bytes, offset);
    const chunkType = readAscii(bytes, offset + 4, 4);
    if (chunkType === GLB_JSON_CHUNK_TYPE) {
      const jsonBytes = bytes.slice(offset + 8, offset + 8 + chunkLength);
      const jsonText = new TextDecoder()
        .decode(jsonBytes)
        .replace(/\0+$/g, "")
        .trim();
      return JSON.parse(jsonText);
    }

    offset += 8 + chunkLength;
  }

  return undefined;
}

function inspectBuffer(buffer) {
  const bytes = toUint8Array(buffer);
  const gltf = parseGlbJson(bytes);
  const requiredExtensions = Array.isArray(gltf?.extensionsRequired)
    ? [...gltf.extensionsRequired]
    : [];
  const usedExtensions = Array.isArray(gltf?.extensionsUsed)
    ? [...gltf.extensionsUsed]
    : [];
  const advancedExtensions = requiredExtensions.filter((extension) =>
    ADVANCED_GLTF_EXTENSIONS.has(extension),
  );

  return {
    isGlb: Boolean(gltf),
    usedExtensions,
    requiredExtensions,
    advancedExtensions,
    recommendedCapabilityPack:
      advancedExtensions.length > 0 ? "gltf-advanced" : null,
  };
}

async function loadCapabilityManifest(Module) {
  capabilityManifestPromise ??= fetch(
    resolveRuntimeUrl(Module, "f3d.capabilities.json"),
  ).then((response) => {
    if (!response.ok) {
      throw new Error(
        `Failed to load Glance3D capabilities manifest: ${response.status} ${response.statusText}`,
      );
    }

    return response.json();
  });

  return capabilityManifestPromise;
}

async function loadAdvancedGltfCapability(Module) {
  if (!advancedGltfPromise) {
    advancedGltfPromise = loadCapabilityManifest(Module).then(async (manifest) => {
      const pack = manifest?.packs?.["gltf-advanced"];
      if (!pack?.entry) {
        throw new Error(
          "Missing Glance3D capability pack: gltf-advanced",
        );
      }

      return import(resolveRuntimeUrl(Module, pack.entry));
    });
  }

  return advancedGltfPromise;
}

async function prepareBuffer(Module, buffer, options = {}) {
  const bytes = toUint8Array(buffer);
  const inspection = inspectBuffer(bytes);

  if (!inspection.recommendedCapabilityPack) {
    return bytes;
  }

  Module.onCapabilityEvent?.({
    type: "capability-loading",
    pack: inspection.recommendedCapabilityPack,
    extensions: inspection.advancedExtensions,
  });

  const capability = await loadAdvancedGltfCapability(Module);
  const prepared = await capability.prepareGLB(bytes, {
    ...options,
    locateFile: (path) => resolveRuntimeUrl(Module, path),
  });
  const preparedInspection = inspectBuffer(prepared);

  Module.onCapabilityEvent?.({
    type: "capability-loaded",
    pack: inspection.recommendedCapabilityPack,
    extensions: inspection.advancedExtensions,
  });
  Module.onCapabilityEvent?.({
    type: "capability-prepared",
    pack: inspection.recommendedCapabilityPack,
    extensions: inspection.advancedExtensions,
    inputByteLength: bytes.byteLength,
    outputByteLength: prepared.byteLength,
    remainingRequiredExtensions: preparedInspection.requiredExtensions,
    remainingAdvancedExtensions: preparedInspection.advancedExtensions,
  });

  return prepared;
}

function clearPreparedFiles(Module) {
  while (preparedFilePaths.length > 0) {
    const path = preparedFilePaths.pop();
    try {
      Module.FS?.unlink(path);
    } catch {
      // The scene or caller may already have removed the temporary file.
    }
  }
}

// Extract the file-name extension (including the leading dot, e.g. ".glb"),
// or "" when the name has none. Strips any directory portion first.
function extensionFromName(fileName) {
  if (typeof fileName !== "string") {
    return "";
  }
  const base = fileName.split(/[\\/]/).pop() ?? "";
  const dot = base.lastIndexOf(".");
  return dot > 0 ? base.slice(dot) : "";
}

// Load a prepared buffer through the in-memory filesystem: write it under a
// temporary path that preserves the original extension, then `scene.add(path)`
// so the reader is picked by file name. This mirrors how the desktop app and
// the default model load files, and avoids the raw memory path's requirement
// for `scene.force_reader` on VTK < 9.6.20260128.
function writePreparedFileToFilesystem(Module, prepared, extension) {
  const directory = "/__glance3d_prepared";
  try {
    Module.FS.mkdir(directory);
  } catch {
    // The in-memory directory is shared by the module and may already exist.
  }

  clearPreparedFiles(Module);
  const path = `${directory}/upload-${++preparedFileCounter}${extension || ""}`;
  Module.FS.writeFile(path, prepared);
  preparedFilePaths.push(path);
  return path;
}

function addPreparedFileFromFilesystem(Module, scene, prepared, extension) {
  return scene.add(writePreparedFileToFilesystem(Module, prepared, extension));
}

function installGLTFHelpers(Module) {
  Module.GLTF = {
    inspectBuffer,
    prepareBuffer: (buffer, options) => prepareBuffer(Module, buffer, options),
  };

  const engineCreate = Module.Engine?.create;
  if (typeof engineCreate !== "function") {
    return;
  }

  Module.Engine.create = function createEngineWithAsyncScene(...args) {
    const engine = engineCreate.apply(this, args);
    const getScene = engine.getScene?.bind(engine);

    function decorateScene(scene) {
      if (!scene || typeof scene.addBuffer !== "function" || scene.addBufferAsync) {
        return scene;
      }

      const addBuffer = scene.addBuffer.bind(scene);
      scene.addBufferAsync = async (buffer, options) => {
        const inspection = Module.GLTF.inspectBuffer(buffer);
        const prepared = await Module.GLTF.prepareBuffer(buffer, options);
        if (inspection.isGlb) {
          Module.onCapabilityEvent?.({
            type: "gltf-buffer-filesystem-load",
            pack: inspection.recommendedCapabilityPack,
            extensions: inspection.advancedExtensions,
            inputByteLength: toUint8Array(buffer).byteLength,
            outputByteLength: prepared.byteLength,
          });
          return addPreparedFileFromFilesystem(Module, scene, prepared, ".glb");
        }

        // Non-GLB: load via the in-memory filesystem using the original
        // extension so the reader is picked by file name. The raw memory path
        // (addBuffer) requires `scene.force_reader` on VTK < 9.6.20260128 and
        // throws "No force reader set ..." otherwise.
        const extension = extensionFromName(options?.fileName);
        if (extension) {
          return addPreparedFileFromFilesystem(Module, scene, prepared, extension);
        }

        return addBuffer(prepared);
      };

      // Background (worker-thread) variant for the threaded wasm build: prepare the buffer, write it
      // to the in-memory filesystem, kick off scene.addAsync() (which parses on a pthread), then poll
      // on this (the main/render) thread until the build finishes and commit it. The browser UI stays
      // responsive throughout because the heavy parse is off the main thread. `onProgress(fraction)`
      // receives values in [0, 1]. Only defined on the threaded build (scene.addAsync bound); callers
      // must also be cross-origin isolated, so feature-detect before using it.
      if (typeof scene.addAsync === "function") {
        scene.addBufferAsyncThreaded = async (buffer, options, onProgress) => {
          const inspection = Module.GLTF.inspectBuffer(buffer);
          const prepared = await Module.GLTF.prepareBuffer(buffer, options);
          const extension = inspection.isGlb
            ? ".glb"
            : extensionFromName(options?.fileName);
          if (!extension) {
            // addAsync picks the reader by file extension; with none, use the sync memory path.
            return addBuffer(prepared);
          }

          const path = writePreparedFileToFilesystem(Module, prepared, extension);
          scene.addAsync([path]); // returns immediately; parsing runs on a worker thread

          // This rAF poll loop is the web driver of the shared "loading-progress layer": it reads
          // the headless core model (getAsyncState + getAsyncProgress) and forwards the raw
          // fraction to the caller's presenter via onProgress. `onProgress(1)` after finalizeAsync
          // marks the FINALIZING/done transition. A new frontend plugs in the same way: poll
          // state+progress, drive its own presenter. (See main.js openFile for the presenter side.)
          const States = Module.SceneAsyncState;
          await new Promise((resolve, reject) => {
            const poll = () => {
              if (scene.getAsyncState() === States.LOADING) {
                onProgress?.(scene.getAsyncProgress());
                requestAnimationFrame(poll);
                return;
              }
              // READY commits, FAILED throws, IDLE is a no-op — finalize on the render thread.
              try {
                scene.finalizeAsync();
                onProgress?.(1);
                resolve();
              } catch (error) {
                reject(error);
              }
            };
            requestAnimationFrame(poll);
          });

          return scene;
        };
      }

      return scene;
    }

    if (typeof getScene === "function") {
      engine.getScene = function getDecoratedScene() {
        return decorateScene(getScene());
      };
      decorateScene(getScene());
    }

    return engine;
  };
}

export default async function f3d(options = {}) {
  const Module = await createCoreModule(options);
  installGLTFHelpers(Module);
  return Module;
}
