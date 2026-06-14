const GLB_MAGIC = 0x46546c67;
const GLB_VERSION = 2;
const GLB_JSON_CHUNK_TYPE = 0x4e4f534a;
const GLB_BIN_CHUNK_TYPE = 0x004e4942;
const EXT_MESHOPT = "EXT_meshopt_compression";
const KHR_TEXTURE_BASISU = "KHR_texture_basisu";
const KHR_MESH_QUANTIZATION = "KHR_mesh_quantization";
const TRANSCODER_FORMAT_RGBA32 = 13;

let meshoptDecoderPromise;
let basisModulePromise;
let crc32Table;

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

function readUint32LE(bytes, offset) {
  return (
    bytes[offset] |
    (bytes[offset + 1] << 8) |
    (bytes[offset + 2] << 16) |
    (bytes[offset + 3] << 24)
  ) >>> 0;
}

function writeUint32LE(bytes, offset, value) {
  bytes[offset] = value & 0xff;
  bytes[offset + 1] = (value >>> 8) & 0xff;
  bytes[offset + 2] = (value >>> 16) & 0xff;
  bytes[offset + 3] = (value >>> 24) & 0xff;
}

function writeUint32BE(bytes, offset, value) {
  bytes[offset] = (value >>> 24) & 0xff;
  bytes[offset + 1] = (value >>> 16) & 0xff;
  bytes[offset + 2] = (value >>> 8) & 0xff;
  bytes[offset + 3] = value & 0xff;
}

function align4(value) {
  return (value + 3) & ~3;
}

function removeExtensionName(gltf, extensionName) {
  for (const field of ["extensionsRequired", "extensionsUsed"]) {
    if (Array.isArray(gltf[field])) {
      gltf[field] = gltf[field].filter((name) => name !== extensionName);
      if (gltf[field].length === 0) {
        delete gltf[field];
      }
    }
  }
}

function removeObjectExtension(owner, extensionName) {
  if (!owner?.extensions) {
    return;
  }

  delete owner.extensions[extensionName];
  if (Object.keys(owner.extensions).length === 0) {
    delete owner.extensions;
  }
}

function parseGLB(input) {
  const bytes = toUint8Array(input);
  if (bytes.byteLength < 20 || readUint32LE(bytes, 0) !== GLB_MAGIC) {
    throw new Error("gltf-advanced can only prepare binary GLB buffers.");
  }

  const version = readUint32LE(bytes, 4);
  if (version !== GLB_VERSION) {
    throw new Error(`Unsupported GLB version: ${version}.`);
  }

  const declaredLength = readUint32LE(bytes, 8);
  if (declaredLength > bytes.byteLength) {
    throw new Error(
      `Invalid GLB length: declared ${declaredLength}, got ${bytes.byteLength}.`,
    );
  }

  let offset = 12;
  let json;
  let bin = new Uint8Array(0);

  while (offset + 8 <= declaredLength) {
    const chunkLength = readUint32LE(bytes, offset);
    const chunkType = readUint32LE(bytes, offset + 4);
    const chunkStart = offset + 8;
    const chunkEnd = chunkStart + chunkLength;

    if (chunkEnd > declaredLength) {
      throw new Error("Invalid GLB chunk length.");
    }

    const chunkBytes = bytes.slice(chunkStart, chunkEnd);
    if (chunkType === GLB_JSON_CHUNK_TYPE) {
      const text = new TextDecoder()
        .decode(chunkBytes)
        .replace(/\0+$/g, "")
        .trim();
      json = JSON.parse(text);
    } else if (chunkType === GLB_BIN_CHUNK_TYPE) {
      bin = chunkBytes;
    }

    offset = chunkEnd;
  }

  if (!json) {
    throw new Error("Invalid GLB: missing JSON chunk.");
  }

  return { json, bin };
}

function getOriginalBufferSlice(gltf, bin, bufferIndex, byteOffset = 0, byteLength) {
  const buffer = gltf.buffers?.[bufferIndex];
  if (!buffer) {
    throw new Error(`GLB references missing buffer ${bufferIndex}.`);
  }

  if (buffer.uri) {
    throw new Error("gltf-advanced does not support external glTF buffers yet.");
  }

  if (bufferIndex !== 0) {
    throw new Error(
      `GLB buffer ${bufferIndex} is not backed by the binary chunk and was not decoded.`,
    );
  }

  const length = byteLength ?? Math.max(0, buffer.byteLength - byteOffset);
  return bin.slice(byteOffset, byteOffset + length);
}

async function loadMeshoptDecoder() {
  meshoptDecoderPromise ??= import("./meshopt_decoder.module.js").then(
    async ({ MeshoptDecoder }) => {
      await MeshoptDecoder.ready;
      if (!MeshoptDecoder.supported) {
        throw new Error("Meshopt decoding is not supported by this browser.");
      }

      return MeshoptDecoder;
    },
  );

  return meshoptDecoderPromise;
}

async function decodeMeshoptViews(gltf, bin, replacementViews) {
  const compressedViews = (gltf.bufferViews ?? [])
    .map((bufferView, index) => ({
      bufferView,
      extension: bufferView.extensions?.[EXT_MESHOPT],
      index,
    }))
    .filter(({ extension }) => Boolean(extension));

  if (compressedViews.length === 0) {
    return;
  }

  const decoder = await loadMeshoptDecoder();

  for (const { bufferView, extension, index } of compressedViews) {
    const count = extension.count;
    const byteStride = extension.byteStride;
    const byteLength = count * byteStride;
    const source = getOriginalBufferSlice(
      gltf,
      bin,
      extension.buffer,
      extension.byteOffset ?? 0,
      extension.byteLength,
    );

    let decoded;
    if (typeof decoder.decodeGltfBufferAsync === "function") {
      decoded = await decoder.decodeGltfBufferAsync(
        count,
        byteStride,
        source,
        extension.mode,
        extension.filter,
      );
    } else {
      decoded = new Uint8Array(byteLength);
      decoder.decodeGltfBuffer(
        decoded,
        count,
        byteStride,
        source,
        extension.mode,
        extension.filter,
      );
    }

    replacementViews.set(index, toUint8Array(decoded));
    bufferView.buffer = 0;
    bufferView.byteOffset = 0;
    bufferView.byteLength = byteLength;
    bufferView.byteStride ??= byteStride;
    removeObjectExtension(bufferView, EXT_MESHOPT);
  }

  for (const buffer of gltf.buffers ?? []) {
    removeObjectExtension(buffer, EXT_MESHOPT);
  }

  removeExtensionName(gltf, EXT_MESHOPT);
}

async function loadBasisModule(locateFile) {
  basisModulePromise ??= (async () => {
    const [{ default: createBasisModule }, wasmResponse] = await Promise.all([
      import("./basis_transcoder.module.js"),
      fetch(locateFile("basis_transcoder.wasm")),
    ]);

    if (!wasmResponse.ok) {
      throw new Error(
        `Failed to load BasisU transcoder WASM: ${wasmResponse.status} ${wasmResponse.statusText}`,
      );
    }

    const wasmBinary = await wasmResponse.arrayBuffer();
    const module = { wasmBinary };
    await createBasisModule(module);
    module.initializeBasis();

    if (!module.KTX2File) {
      throw new Error("BasisU transcoder does not expose KTX2File support.");
    }

    return module;
  })();

  return basisModulePromise;
}

function getBufferViewBytes(gltf, bin, replacementViews, index) {
  if (replacementViews.has(index)) {
    return replacementViews.get(index);
  }

  const bufferView = gltf.bufferViews?.[index];
  if (!bufferView) {
    throw new Error(`GLB references missing bufferView ${index}.`);
  }

  return getOriginalBufferSlice(
    gltf,
    bin,
    bufferView.buffer ?? 0,
    bufferView.byteOffset ?? 0,
    bufferView.byteLength,
  );
}

function getComponentCount(type) {
  return {
    SCALAR: 1,
    VEC2: 2,
    VEC3: 3,
    VEC4: 4,
    MAT2: 4,
    MAT3: 9,
    MAT4: 16,
  }[type];
}

function getComponentSize(componentType) {
  return {
    5120: 1,
    5121: 1,
    5122: 2,
    5123: 2,
    5125: 4,
    5126: 4,
  }[componentType];
}

function readAccessorComponent(dataView, offset, componentType) {
  switch (componentType) {
    case 5120:
      return dataView.getInt8(offset);
    case 5121:
      return dataView.getUint8(offset);
    case 5122:
      return dataView.getInt16(offset, true);
    case 5123:
      return dataView.getUint16(offset, true);
    case 5125:
      return dataView.getUint32(offset, true);
    case 5126:
      return dataView.getFloat32(offset, true);
    default:
      throw new Error(`Unsupported accessor componentType: ${componentType}.`);
  }
}

function normalizeAccessorComponent(value, componentType) {
  switch (componentType) {
    case 5120:
      return Math.max(value / 127, -1);
    case 5121:
      return value / 255;
    case 5122:
      return Math.max(value / 32767, -1);
    case 5123:
      return value / 65535;
    default:
      return value;
  }
}

function collectMeshAttributeAccessors(gltf) {
  const indices = new Set();
  for (const mesh of gltf.meshes ?? []) {
    for (const primitive of mesh.primitives ?? []) {
      for (const accessorIndex of Object.values(primitive.attributes ?? {})) {
        if (typeof accessorIndex === "number") {
          indices.add(accessorIndex);
        }
      }

      for (const target of primitive.targets ?? []) {
        for (const accessorIndex of Object.values(target ?? {})) {
          if (typeof accessorIndex === "number") {
            indices.add(accessorIndex);
          }
        }
      }
    }
  }

  return indices;
}

function dequantizeMeshAttributes(gltf, bin, replacementViews) {
  if (
    !Array.isArray(gltf.extensionsRequired) ||
    !gltf.extensionsRequired.includes(KHR_MESH_QUANTIZATION)
  ) {
    return;
  }

  const accessorIndices = collectMeshAttributeAccessors(gltf);
  for (const accessorIndex of accessorIndices) {
    const accessor = gltf.accessors?.[accessorIndex];
    if (!accessor || accessor.componentType === 5126) {
      continue;
    }

    const bufferViewIndex = accessor.bufferView;
    if (typeof bufferViewIndex !== "number") {
      continue;
    }

    const bufferView = gltf.bufferViews?.[bufferViewIndex];
    const componentCount = getComponentCount(accessor.type);
    const componentSize = getComponentSize(accessor.componentType);
    if (!bufferView || !componentCount || !componentSize) {
      continue;
    }

    const source = getBufferViewBytes(gltf, bin, replacementViews, bufferViewIndex);
    const dataView = new DataView(source.buffer, source.byteOffset, source.byteLength);
    const sourceStride = bufferView.byteStride ?? componentCount * componentSize;
    const accessorOffset = accessor.byteOffset ?? 0;
    const output = new Float32Array(accessor.count * componentCount);
    const min = new Array(componentCount).fill(Number.POSITIVE_INFINITY);
    const max = new Array(componentCount).fill(Number.NEGATIVE_INFINITY);

    for (let element = 0; element < accessor.count; element += 1) {
      const elementOffset = accessorOffset + element * sourceStride;
      for (let component = 0; component < componentCount; component += 1) {
        const sourceOffset = elementOffset + component * componentSize;
        const rawValue = readAccessorComponent(
          dataView,
          sourceOffset,
          accessor.componentType,
        );
        const value = accessor.normalized
          ? normalizeAccessorComponent(rawValue, accessor.componentType)
          : rawValue;
        const outputIndex = element * componentCount + component;
        output[outputIndex] = value;
        min[component] = Math.min(min[component], value);
        max[component] = Math.max(max[component], value);
      }
    }

    const outputBytes = new Uint8Array(output.buffer);
    const nextBufferViewIndex = gltf.bufferViews.length;
    gltf.bufferViews.push({
      buffer: 0,
      byteOffset: 0,
      byteLength: outputBytes.byteLength,
    });
    replacementViews.set(nextBufferViewIndex, outputBytes);

    accessor.bufferView = nextBufferViewIndex;
    accessor.byteOffset = 0;
    accessor.componentType = 5126;
    delete accessor.normalized;
    accessor.min = min;
    accessor.max = max;
  }

  removeExtensionName(gltf, KHR_MESH_QUANTIZATION);
}

function transcodeKtx2ToRgba(ktx2Bytes, BasisModule) {
  const ktx2File = new BasisModule.KTX2File(ktx2Bytes);

  function cleanup() {
    ktx2File.close();
    ktx2File.delete();
  }

  try {
    if (!ktx2File.isValid()) {
      throw new Error("Invalid or unsupported KTX2 texture.");
    }

    const width = ktx2File.getWidth();
    const height = ktx2File.getHeight();
    const levelCount = ktx2File.getLevels();
    const layerCount = ktx2File.getLayers() || 1;
    const faceCount = ktx2File.getFaces();

    if (!width || !height || !levelCount) {
      throw new Error("Invalid KTX2 texture dimensions.");
    }

    if (layerCount !== 1 || faceCount !== 1) {
      throw new Error("Array and cubemap KTX2 textures are not supported yet.");
    }

    if (!ktx2File.startTranscoding()) {
      throw new Error("BasisU startTranscoding failed.");
    }

    const levelInfo = ktx2File.getImageLevelInfo(0, 0, 0);
    const outputWidth = levelCount > 1 ? levelInfo.origWidth : levelInfo.width;
    const outputHeight = levelCount > 1 ? levelInfo.origHeight : levelInfo.height;
    const rgba = new Uint8Array(
      ktx2File.getImageTranscodedSizeInBytes(
        0,
        0,
        0,
        TRANSCODER_FORMAT_RGBA32,
      ),
    );

    if (
      !ktx2File.transcodeImage(
        rgba,
        0,
        0,
        0,
        TRANSCODER_FORMAT_RGBA32,
        0,
        -1,
        -1,
      )
    ) {
      throw new Error("BasisU transcodeImage failed.");
    }

    return { rgba, width: outputWidth, height: outputHeight };
  } finally {
    cleanup();
  }
}

function getCrc32Table() {
  if (crc32Table) {
    return crc32Table;
  }

  crc32Table = new Uint32Array(256);
  for (let n = 0; n < 256; n += 1) {
    let c = n;
    for (let k = 0; k < 8; k += 1) {
      c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
    }
    crc32Table[n] = c >>> 0;
  }

  return crc32Table;
}

function crc32(bytes) {
  const table = getCrc32Table();
  let c = 0xffffffff;
  for (const byte of bytes) {
    c = table[(c ^ byte) & 0xff] ^ (c >>> 8);
  }

  return (c ^ 0xffffffff) >>> 0;
}

function adler32(bytes) {
  let a = 1;
  let b = 0;
  for (const byte of bytes) {
    a = (a + byte) % 65521;
    b = (b + a) % 65521;
  }

  return ((b << 16) | a) >>> 0;
}

function makeChunk(type, data) {
  const typeBytes = new TextEncoder().encode(type);
  const chunk = new Uint8Array(12 + data.byteLength);
  writeUint32BE(chunk, 0, data.byteLength);
  chunk.set(typeBytes, 4);
  chunk.set(data, 8);
  writeUint32BE(chunk, 8 + data.byteLength, crc32(chunk.slice(4, 8 + data.byteLength)));
  return chunk;
}

function zlibStore(bytes) {
  const blockCount = Math.ceil(bytes.byteLength / 65535) || 1;
  const outputLength = 2 + bytes.byteLength + blockCount * 5 + 4;
  const output = new Uint8Array(outputLength);
  let outputOffset = 0;
  let inputOffset = 0;

  output[outputOffset++] = 0x78;
  output[outputOffset++] = 0x01;

  for (let block = 0; block < blockCount; block += 1) {
    const length = Math.min(65535, bytes.byteLength - inputOffset);
    const isFinal = block === blockCount - 1;
    output[outputOffset++] = isFinal ? 1 : 0;
    output[outputOffset++] = length & 0xff;
    output[outputOffset++] = (length >>> 8) & 0xff;
    output[outputOffset++] = (~length) & 0xff;
    output[outputOffset++] = ((~length) >>> 8) & 0xff;
    output.set(bytes.slice(inputOffset, inputOffset + length), outputOffset);
    outputOffset += length;
    inputOffset += length;
  }

  writeUint32BE(output, outputOffset, adler32(bytes));
  return output;
}

function encodePngRgbaFallback(rgba, width, height) {
  const stride = width * 4;
  const raw = new Uint8Array((stride + 1) * height);
  for (let row = 0; row < height; row += 1) {
    const rowOffset = row * (stride + 1);
    raw[rowOffset] = 0;
    raw.set(rgba.slice(row * stride, row * stride + stride), rowOffset + 1);
  }

  const signature = new Uint8Array([137, 80, 78, 71, 13, 10, 26, 10]);
  const ihdr = new Uint8Array(13);
  writeUint32BE(ihdr, 0, width);
  writeUint32BE(ihdr, 4, height);
  ihdr[8] = 8;
  ihdr[9] = 6;
  ihdr[10] = 0;
  ihdr[11] = 0;
  ihdr[12] = 0;

  const chunks = [
    signature,
    makeChunk("IHDR", ihdr),
    makeChunk("IDAT", zlibStore(raw)),
    makeChunk("IEND", new Uint8Array(0)),
  ];
  const length = chunks.reduce((sum, chunk) => sum + chunk.byteLength, 0);
  const png = new Uint8Array(length);
  let offset = 0;
  for (const chunk of chunks) {
    png.set(chunk, offset);
    offset += chunk.byteLength;
  }

  return png;
}

async function encodePngRgba(rgba, width, height) {
  const imageData = new ImageData(new Uint8ClampedArray(rgba), width, height);

  if (typeof OffscreenCanvas !== "undefined") {
    const canvas = new OffscreenCanvas(width, height);
    const context = canvas.getContext("2d");
    context.putImageData(imageData, 0, 0);
    const blob = await canvas.convertToBlob({ type: "image/png" });
    return new Uint8Array(await blob.arrayBuffer());
  }

  if (typeof document !== "undefined") {
    const canvas = document.createElement("canvas");
    canvas.width = width;
    canvas.height = height;
    const context = canvas.getContext("2d");
    context.putImageData(imageData, 0, 0);
    const blob = await new Promise((resolve, reject) => {
      canvas.toBlob((result) => {
        if (result) {
          resolve(result);
        } else {
          reject(new Error("Canvas PNG encoding failed."));
        }
      }, "image/png");
    });
    return new Uint8Array(await blob.arrayBuffer());
  }

  return encodePngRgbaFallback(rgba, width, height);
}

async function convertBasisTextures(gltf, bin, replacementViews, locateFile) {
  const imagesToConvert = new Set();

  for (const [index, image] of (gltf.images ?? []).entries()) {
    if (image.mimeType === "image/ktx2") {
      imagesToConvert.add(index);
    }
  }

  for (const texture of gltf.textures ?? []) {
    const basisu = texture.extensions?.[KHR_TEXTURE_BASISU];
    if (basisu && typeof basisu.source === "number") {
      imagesToConvert.add(basisu.source);
      texture.source = basisu.source;
      removeObjectExtension(texture, KHR_TEXTURE_BASISU);
    }
  }

  if (imagesToConvert.size === 0) {
    return;
  }

  const BasisModule = await loadBasisModule(locateFile);

  for (const imageIndex of imagesToConvert) {
    const image = gltf.images?.[imageIndex];
    if (!image) {
      throw new Error(`KHR_texture_basisu references missing image ${imageIndex}.`);
    }

    if (typeof image.bufferView !== "number") {
      throw new Error("gltf-advanced only supports embedded KTX2 images.");
    }

    const ktx2Bytes = getBufferViewBytes(gltf, bin, replacementViews, image.bufferView);
    const { rgba, width, height } = transcodeKtx2ToRgba(ktx2Bytes, BasisModule);
    const png = await encodePngRgba(rgba, width, height);

    replacementViews.set(image.bufferView, png);
    image.mimeType = "image/png";
    delete image.uri;
  }

  removeExtensionName(gltf, KHR_TEXTURE_BASISU);
}

function packBufferViews(gltf, bin, replacementViews) {
  const chunks = [];
  let byteLength = 0;

  for (const [index, bufferView] of (gltf.bufferViews ?? []).entries()) {
    const data = getBufferViewBytes(gltf, bin, replacementViews, index);
    const alignedOffset = align4(byteLength);
    const padding = alignedOffset - byteLength;
    if (padding > 0) {
      chunks.push(new Uint8Array(padding));
      byteLength += padding;
    }

    bufferView.buffer = 0;
    bufferView.byteOffset = byteLength;
    bufferView.byteLength = data.byteLength;
    chunks.push(data);
    byteLength += data.byteLength;
  }

  const paddedLength = align4(byteLength);
  if (paddedLength > byteLength) {
    chunks.push(new Uint8Array(paddedLength - byteLength));
  }

  const packed = new Uint8Array(paddedLength);
  let offset = 0;
  for (const chunk of chunks) {
    packed.set(chunk, offset);
    offset += chunk.byteLength;
  }

  gltf.buffers = [{ byteLength: byteLength }];
  return packed;
}

function buildGLB(gltf, bin) {
  const jsonBytes = new TextEncoder().encode(JSON.stringify(gltf));
  const jsonLength = align4(jsonBytes.byteLength);
  const binLength = align4(bin.byteLength);
  const totalLength = 12 + 8 + jsonLength + 8 + binLength;
  const glb = new Uint8Array(totalLength);

  writeUint32LE(glb, 0, GLB_MAGIC);
  writeUint32LE(glb, 4, GLB_VERSION);
  writeUint32LE(glb, 8, totalLength);
  writeUint32LE(glb, 12, jsonLength);
  writeUint32LE(glb, 16, GLB_JSON_CHUNK_TYPE);
  glb.fill(0x20, 20, 20 + jsonLength);
  glb.set(jsonBytes, 20);

  const binHeaderOffset = 20 + jsonLength;
  writeUint32LE(glb, binHeaderOffset, binLength);
  writeUint32LE(glb, binHeaderOffset + 4, GLB_BIN_CHUNK_TYPE);
  glb.set(bin, binHeaderOffset + 8);

  return glb;
}

export async function prepareGLB(bytes, options = {}) {
  const locateFile = options.locateFile ?? ((path) => path);
  const { json, bin } = parseGLB(bytes);
  const gltf = JSON.parse(JSON.stringify(json));
  const replacementViews = new Map();

  await decodeMeshoptViews(gltf, bin, replacementViews);
  dequantizeMeshAttributes(gltf, bin, replacementViews);
  await convertBasisTextures(gltf, bin, replacementViews, locateFile);

  const packedBin = packBufferViews(gltf, bin, replacementViews);
  return buildGLB(gltf, packedBin);
}
