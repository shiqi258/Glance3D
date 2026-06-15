import { spawnSync } from "node:child_process";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { fileURLToPath } from "node:url";

const scriptDir = path.dirname(fileURLToPath(import.meta.url));
const repoRoot = path.resolve(scriptDir, "..");
const buildDir = path.join(repoRoot, "_wasm_build");
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
    "  addBuffer(_0: any): Scene;\n  addBufferAsync(_0: any, _1?: GLTFPrepareOptions): Promise<Scene>;\n",
  );
  types = types.replace(
    /export type MainModule = WasmModule & typeof RuntimeExports & EmbindModule;\r?\n/,
    `export type GLTFCapabilityPack = "gltf-advanced";\n\nexport interface GLTFInspection {\n  isGlb: boolean;\n  usedExtensions: string[];\n  requiredExtensions: string[];\n  advancedExtensions: string[];\n  recommendedCapabilityPack: GLTFCapabilityPack | null;\n}\n\nexport interface GLTFPrepareOptions {\n  locateFile?: (path: string) => string;\n}\n\nexport interface GLTFNamespace {\n  inspectBuffer(buffer: ArrayBuffer | ArrayBufferView): GLTFInspection;\n  prepareBuffer(buffer: ArrayBuffer | ArrayBufferView, options?: GLTFPrepareOptions): Promise<Uint8Array>;\n}\n\nexport interface GLTFCapabilityEvent {\n  type: "capability-loading" | "capability-loaded" | "capability-prepared";\n  pack: GLTFCapabilityPack;\n  extensions: string[];\n  inputByteLength?: number;\n  outputByteLength?: number;\n  remainingRequiredExtensions?: string[];\n  remainingAdvancedExtensions?: string[];\n}\n\nexport type MainModule = WasmModule & typeof RuntimeExports & EmbindModule & {\n  GLTF: GLTFNamespace;\n  onCapabilityEvent?: (event: GLTFCapabilityEvent) => void;\n};\n`,
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

  const depsDir = process.env.F3D_WASM_DEPS_DIR ?? config.wasmDepsDir;
  const generator = process.env.F3D_WASM_CMAKE_GENERATOR ?? config.cmakeGenerator ?? "Ninja";
  const fullPlugins = (process.env.F3D_WASM_FULL_PLUGINS ?? (config.fullPlugins ? "ON" : "")) === "ON";

  if (!depsDir) {
    throw new Error(
      "WebAssembly dependency prefix is not configured. Set \"wasmDepsDir\" in webassembly/deps.local.json (copy deps.local.example.json), or export F3D_WASM_DEPS_DIR.",
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
