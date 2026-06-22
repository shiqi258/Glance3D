import { spawnSync } from "node:child_process";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { fileURLToPath } from "node:url";

const scriptDir = path.dirname(fileURLToPath(import.meta.url));
const repoRoot = path.resolve(scriptDir, "..");
// The threaded build (Emscripten pthreads) parses off the browser main thread, so it is the DEFAULT
// — the web viewer loads large files without freezing the UI. It links against a thread-enabled VTK
// (wasmThreadsDepsDir) and emits pthread-tagged objects that cannot mix with the non-threaded build,
// so each flavor gets its own build tree. The threaded wasm REQUIRES the site to serve COOP/COEP
// cross-origin isolation headers (Vite dev/preview do; production must too). Opt out — for hosts that
// cannot set those headers — with F3D_WASM_THREADS=OFF (also accepts 0/false).
const threadsEnv = (process.env.F3D_WASM_THREADS ?? "ON").toUpperCase();
const threads = threadsEnv !== "OFF" && threadsEnv !== "0" && threadsEnv !== "FALSE";
const buildDir = path.join(repoRoot, threads ? "_wasm_build_threads" : "_wasm_build");
const distDir = path.join(repoRoot, "dist");
const runtimeDir = path.join(scriptDir, "runtime");

const command = process.argv[2] ?? "build";
const buildType = process.argv[3] ?? "Release";
const isWindows = process.platform === "win32";

// --- Machine-local dependency configuration --------------------------------
// WebAssembly dependency locations are machine-specific. Rather than requiring
// the user to export environment variables every time, read them from a
// git-ignored `webassembly/deps.local.json` (copy `deps.local.example.json` and
// edit the paths). Environment variables, when present, always take precedence
// so CI and manual `emsdk_env` setups keep working unchanged.
function loadLocalConfig() {
  const configPath = path.join(scriptDir, "deps.local.json");
  if (!fs.existsSync(configPath)) {
    return {};
  }
  try {
    return JSON.parse(fs.readFileSync(configPath, "utf8"));
  } catch (err) {
    throw new Error(`Failed to parse ${configPath}: ${err.message}`);
  }
}

// Capture the environment produced by emsdk's own activation script and merge it
// into process.env, so child build commands see an activated Emscripten toolchain
// (emcc/emcmake on PATH, EMSDK set) without the user activating it manually.
function activateEmsdk(emsdkDir) {
  if (!emsdkDir) {
    return;
  }
  if (!fs.existsSync(emsdkDir)) {
    throw new Error(`emsdkDir does not exist: ${emsdkDir}`);
  }
  const marker = "__GLANCE3D_ENV__";
  let result;
  if (isWindows) {
    // Run the activation + `set` from a temp .bat invoked through cmd.exe
    // explicitly. Inlining the quoted path into a shell command is fragile
    // (Node escapes the inner quotes in a way cmd misparses), and `shell: true`
    // may resolve to a non-cmd shell, so a plain batch file is the robust path.
    const tmpBat = path.join(os.tmpdir(), `glance3d_emsdk_env_${process.pid}.bat`);
    fs.writeFileSync(
      tmpBat,
      `@echo off\r\ncall "${path.join(emsdkDir, "emsdk_env.bat")}" >nul 2>nul\r\necho ${marker}\r\nset\r\n`,
    );
    try {
      result = spawnSync("cmd.exe", ["/d", "/c", tmpBat], { encoding: "utf8" });
    } finally {
      fs.rmSync(tmpBat, { force: true });
    }
  } else {
    result = spawnSync("bash", ["-c", `. "${path.join(emsdkDir, "emsdk_env.sh")}" >/dev/null 2>&1 && echo ${marker} && env`], { encoding: "utf8" });
  }
  const out = result.stdout ?? "";
  const markerIndex = out.indexOf(marker);
  const envText = markerIndex >= 0 ? out.slice(markerIndex + marker.length) : out;
  for (const line of envText.split(/\r?\n/)) {
    const eq = line.indexOf("=");
    if (eq <= 0) {
      continue;
    }
    process.env[line.slice(0, eq).trim()] = line.slice(eq + 1);
  }
}

function prependToPath(dir) {
  if (!dir) {
    return;
  }
  if (!fs.existsSync(dir)) {
    throw new Error(`Configured tool directory does not exist: ${dir}`);
  }
  process.env.PATH = `${dir}${path.delimiter}${process.env.PATH ?? ""}`;
}

function run(cmd, args, options = {}) {
  const result = spawnSync(cmd, args, {
    cwd: repoRoot,
    env: process.env,
    stdio: "inherit",
    shell: isWindows,
    ...options,
  });

  if (result.error) {
    throw result.error;
  }

  if (result.status !== 0) {
    process.exit(result.status ?? 1);
  }
}

function clean() {
  fs.rmSync(buildDir, { recursive: true, force: true });
  fs.rmSync(distDir, { recursive: true, force: true });
}

function copyBuildArtifacts() {
  const binDir = path.join(buildDir, "bin");
  if (!fs.existsSync(binDir)) {
    throw new Error(`Expected build artifacts in ${binDir}, but it does not exist.`);
  }

  fs.rmSync(distDir, { recursive: true, force: true });
  fs.mkdirSync(distDir, { recursive: true });

  const requiredCoreArtifacts = ["f3d.js", "f3d.wasm", "f3d.d.ts"];
  for (const artifact of requiredCoreArtifacts) {
    const input = path.join(binDir, artifact);
    if (!fs.existsSync(input)) {
      throw new Error(`Missing WebAssembly artifact: ${input}`);
    }
  }

  fs.copyFileSync(path.join(binDir, "f3d.js"), path.join(distDir, "f3d-core.js"));
  fs.copyFileSync(path.join(binDir, "f3d.wasm"), path.join(distDir, "f3d.wasm"));
  fs.copyFileSync(path.join(scriptDir, "f3d-wrapper.js"), path.join(distDir, "f3d.js"));
  fs.copyFileSync(
    path.join(scriptDir, "f3d-gltf-advanced.js"),
    path.join(distDir, "f3d-gltf-advanced.js"),
  );
  fs.copyFileSync(
    path.join(scriptDir, "f3d.capabilities.json"),
    path.join(distDir, "f3d.capabilities.json"),
  );
  fs.copyFileSync(
    path.join(runtimeDir, "meshopt_decoder.module.js"),
    path.join(distDir, "meshopt_decoder.module.js"),
  );
  fs.copyFileSync(
    path.join(runtimeDir, "basis_transcoder.wasm"),
    path.join(distDir, "basis_transcoder.wasm"),
  );

  const basisTranscoder = fs.readFileSync(
    path.join(runtimeDir, "basis_transcoder.js"),
    "utf8",
  );
  fs.writeFileSync(
    path.join(distDir, "basis_transcoder.module.js"),
    `${basisTranscoder}\nexport default BASIS;\n`,
  );

  patchTypes(path.join(binDir, "f3d.d.ts"), path.join(distDir, "f3d.d.ts"));
}

function patchTypes(inputPath, outputPath) {
  let types = fs.readFileSync(inputPath, "utf8");
  types = types.replace(
    /  addBuffer\(_0: any\): Scene;\r?\n/,
    "  addBuffer(_0: any): Scene;\n  addBufferAsync(_0: ArrayBuffer | ArrayBufferView, _1?: Glance3DGLTFPrepareOptions): Promise<Scene>;\n  addBufferAsyncThreaded?(_0: ArrayBuffer | ArrayBufferView, _1?: Glance3DGLTFPrepareOptions, _2?: (progress: number) => void): Promise<Scene>;\n  addFileSetAsync?(_0: Glance3DVirtualFile[], _1?: Glance3DFileSetLoadOptions): Promise<Scene>;\n  addFileSetAsyncThreaded?(_0: Glance3DVirtualFile[], _1?: Glance3DFileSetLoadOptions, _2?: (progress: number) => void): Promise<Scene>;\n",
  );
  types = types.replace(
    /  getG3DSceneTree\(\): any;\r?\n/,
    "  getG3DSceneTree(): Glance3DSceneTreeSnapshot;\n",
  );
  types = types.replace(
    /    getReadersInfo\(\): any;\r?\n/,
    "    getReadersInfo(): Glance3DReaderInfo[];\n",
  );
  types = types.replace(
    /    forward\(_0: any\): void;\r?\n/,
    "    forward(_0: (level: Glance3DLogLevel, message: string) => void): void;\n",
  );
  types = types.replace(
    /export type MainModule = WasmModule & typeof RuntimeExports & EmbindModule;\r?\n/,
    `export type Glance3DCapabilityPack = "gltf-advanced";\n\nexport interface Glance3DGLTFInspection {\n  isGlb: boolean;\n  usedExtensions: string[];\n  requiredExtensions: string[];\n  advancedExtensions: string[];\n  recommendedCapabilityPack: Glance3DCapabilityPack | null;\n}\n\nexport interface Glance3DGLTFPrepareOptions {\n  locateFile?: (path: string) => string;\n  fileName?: string;\n}\n\nexport interface Glance3DVirtualFile {\n  path: string;\n  data: ArrayBuffer | ArrayBufferView;\n}\n\nexport interface Glance3DFileSetLoadOptions {\n  primaryPath?: string;\n  packageName?: string;\n}\n\nexport interface Glance3DGLTFNamespace {\n  inspectBuffer(buffer: ArrayBuffer | ArrayBufferView): Glance3DGLTFInspection;\n  prepareBuffer(buffer: ArrayBuffer | ArrayBufferView, options?: Glance3DGLTFPrepareOptions): Promise<Uint8Array>;\n}\n\nexport type Glance3DCapabilityEvent =\n  | {\n      type: "capability-loading" | "capability-loaded";\n      pack: Glance3DCapabilityPack;\n      extensions: string[];\n    }\n  | {\n      type: "capability-prepared";\n      pack: Glance3DCapabilityPack;\n      extensions: string[];\n      inputByteLength?: number;\n      outputByteLength?: number;\n      remainingRequiredExtensions?: string[];\n      remainingAdvancedExtensions?: string[];\n    }\n  | {\n      type: "gltf-buffer-filesystem-load";\n      pack: Glance3DCapabilityPack | null;\n      extensions: string[];\n      inputByteLength?: number;\n      outputByteLength?: number;\n    }\n  | {\n      type: "file-set-filesystem-load";\n      primaryPath: string;\n      fileCount: number;\n      totalByteLength: number;\n    };\n\nexport type GLTFCapabilityPack = Glance3DCapabilityPack;\nexport type GLTFInspection = Glance3DGLTFInspection;\nexport type GLTFPrepareOptions = Glance3DGLTFPrepareOptions;\nexport type GLTFNamespace = Glance3DGLTFNamespace;\nexport type GLTFCapabilityEvent = Glance3DCapabilityEvent;\n\nexport type Glance3DOptionBag = Options;\nexport type Glance3DScene = Scene;\nexport type Glance3DCamera = Camera;\nexport type Glance3DWindow = Window;\nexport type Glance3DInteractor = Interactor;\nexport type Glance3DEngine = Engine;\n\nexport interface Glance3DReaderInfo {\n  name: string;\n  description: string;\n  pluginName: string;\n  extensions: string[];\n  mimeTypes: string[];\n  hasSceneReader: boolean;\n  hasGeometryReader: boolean;\n}\n\nexport type Glance3DLogLevel = LogVerboseLevel | number;\n\nexport interface Glance3DFactoryOptions {\n  canvas?: HTMLCanvasElement;\n  locateFile?: (path: string, prefix?: string) => string;\n  print?: (message: string) => void;\n  printErr?: (message: string) => void;\n}\n\nexport type Glance3DSceneTreeNodeKind = "root" | "group" | "object";\n\nexport interface Glance3DSceneTreeCapabilities {\n  visibility: boolean;\n  solo: boolean;\n  focus: boolean;\n  selection: boolean;\n  bounds: boolean;\n  stats: boolean;\n}\n\nexport interface Glance3DSceneTreeNode {\n  id: string;\n  label: string;\n  kind: Glance3DSceneTreeNodeKind;\n  visible: boolean;\n  partiallyVisible: boolean;\n  collapsedByDefault: boolean;\n  path: string;\n  bounds?: [number, number, number, number, number, number];\n  children: Glance3DSceneTreeNode[];\n}\n\nexport interface Glance3DSceneTreeSnapshot {\n  schemaVersion: 1;\n  capabilities: Glance3DSceneTreeCapabilities;\n  children: Glance3DSceneTreeNode[];\n}\n\nexport type Glance3DModule = WasmModule & typeof RuntimeExports & EmbindModule & {\n  GLTF: Glance3DGLTFNamespace;\n  onCapabilityEvent?: (event: Glance3DCapabilityEvent) => void;\n};\n\nexport type Glance3DModuleFactory = (options?: Glance3DFactoryOptions) => Promise<Glance3DModule>;\nexport type MainModule = Glance3DModule;\n`,
  );
  types = types.replace(
    /export default function MainModuleFactory \(options\?: unknown\): Promise<MainModule>;\r?\n/,
    "export default function MainModuleFactory(options?: Glance3DFactoryOptions): Promise<Glance3DModule>;\n",
  );
  fs.writeFileSync(outputPath, types);
}

function build() {
  const config = loadLocalConfig();

  // Make `npm run build` work without any manually exported environment:
  // activate Emscripten and put Ninja on PATH using the configured locations.
  // Skip activation if the shell already has Emscripten active (EMSDK set).
  if (!process.env.EMSDK) {
    activateEmsdk(config.emsdkDir);
  }
  prependToPath(config.ninjaDir);

  const depsDir =
    process.env.F3D_WASM_DEPS_DIR ?? (threads ? config.wasmThreadsDepsDir : config.wasmDepsDir);
  const generator = process.env.F3D_WASM_CMAKE_GENERATOR ?? config.cmakeGenerator ?? "Ninja";
  const fullPlugins = (process.env.F3D_WASM_FULL_PLUGINS ?? (config.fullPlugins ? "ON" : "")) === "ON";

  if (!depsDir) {
    const key = threads ? "wasmThreadsDepsDir" : "wasmDepsDir";
    throw new Error(
      `WebAssembly dependency prefix is not configured. Set "${key}" in webassembly/deps.local.json (copy deps.local.example.json), or export F3D_WASM_DEPS_DIR.`,
    );
  }

  if (!fs.existsSync(depsDir)) {
    throw new Error(`WebAssembly dependency prefix does not exist: ${depsDir}`);
  }

  const configureArgs = [
    "cmake",
    "-S",
    repoRoot,
    "-B",
    buildDir,
    "-G",
    generator,
    "-DBUILD_SHARED_LIBS=OFF",
    `-DCMAKE_BUILD_TYPE=${buildType}`,
    "-DBUILD_TESTING=OFF",
    "-DF3D_WASM_BUILD_TESTING=OFF",
    "-DF3D_MODULE_UI=OFF",
    `-DF3D_MODULE_WEBP=${fullPlugins ? "ON" : "OFF"}`,
    `-DF3D_PLUGIN_BUILD_ASSIMP=${fullPlugins ? "ON" : "OFF"}`,
    `-DF3D_PLUGIN_BUILD_DRACO=${fullPlugins ? "ON" : "OFF"}`,
    "-DF3D_PLUGIN_BUILD_HDF=OFF",
    `-DF3D_PLUGIN_BUILD_OCCT=${fullPlugins ? "ON" : "OFF"}`,
    "-DF3D_PLUGIN_BUILD_PDAL=OFF",
    `-DF3D_PLUGIN_BUILD_WEBIFC=${fullPlugins ? "ON" : "OFF"}`,
    "-DF3D_STRICT_BUILD=ON",
    `-DF3D_WASM_THREADS=${threads ? "ON" : "OFF"}`,
  ];

  configureArgs.push(`-DCMAKE_FIND_ROOT_PATH:PATH=${path.resolve(depsDir)}`);

  run("emcmake", configureArgs);
  run("cmake", ["--build", buildDir]);
  copyBuildArtifacts();
}

function test() {
  run("ctest", ["--test-dir", path.join(buildDir, "webassembly"), "--output-on-failure"]);
}

switch (command) {
  case "clean":
    clean();
    break;
  case "package":
    copyBuildArtifacts();
    break;
  case "build":
    build();
    break;
  case "test":
    test();
    break;
  default:
    console.error(`Unknown command: ${command}`);
    console.error("Usage: node webassembly/build-local.mjs <clean|package|build|test> [build-type]");
    process.exit(1);
}
