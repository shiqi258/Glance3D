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

function addPreparedGLBFromFilesystem(Module, scene, prepared) {
  const directory = "/__glance3d_prepared";
  try {
    Module.FS.mkdir(directory);
  } catch {
    // The in-memory directory is shared by the module and may already exist.
  }

  clearPreparedFiles(Module);
  const path = `${directory}/prepared-${++preparedFileCounter}.glb`;
  Module.FS.writeFile(path, prepared);
  preparedFilePaths.push(path);
  return scene.add(path);
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
        if (inspection.recommendedCapabilityPack) {
          return addPreparedGLBFromFilesystem(Module, scene, prepared);
        }

        return addBuffer(prepared);
      };

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
