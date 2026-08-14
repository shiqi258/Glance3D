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

/**
 * Signatures that embind can only describe as `any`, plus the wrapper-only methods it cannot see at
 * all. Each entry must match exactly once; a miss is a build error rather than a silent downgrade
 * back to `any` — which is how `getG3DDataInfo` quietly lost its type before.
 */
const TYPE_SUBSTITUTIONS = [
  {
    name: "Scene.addBuffer (wrapper-only async variants)",
    pattern: /^(\s*)addBuffer\(_0: any\): Scene;$/m,
    replacement: [
      "$1addBuffer(_0: any): Scene;",
      "$1addBufferAsync(_0: ArrayBuffer | ArrayBufferView, _1?: Glance3DGLTFPrepareOptions): Promise<Scene>;",
      "$1addBufferAsyncThreaded?(_0: ArrayBuffer | ArrayBufferView, _1?: Glance3DGLTFPrepareOptions, _2?: (progress: number) => void): Promise<Scene>;",
      "$1addFileSetAsync?(_0: Glance3DVirtualFile[], _1?: Glance3DFileSetLoadOptions): Promise<Scene>;",
      "$1addFileSetAsyncThreaded?(_0: Glance3DVirtualFile[], _1?: Glance3DFileSetLoadOptions, _2?: (progress: number) => void): Promise<Scene>;",
    ].join("\n"),
  },
  {
    name: "Scene.getG3DDataInfo",
    pattern: /^(\s*)getG3DDataInfo\(\): any;$/m,
    replacement: "$1getG3DDataInfo(): Glance3DDataInfo;",
  },
  {
    name: "Scene.getSceneTreeInfo",
    pattern: /^(\s*)getSceneTreeInfo\(\): any;$/m,
    replacement: "$1getSceneTreeInfo(): Glance3DTreeInfo;",
  },
  {
    name: "Scene.getSceneTreeRows",
    pattern: /^(\s*)getSceneTreeRows\(_0: number, _1: number\): any;$/m,
    replacement: "$1getSceneTreeRows(begin: number, count: number): Glance3DTreeRow[];",
  },
  {
    name: "Scene.getSceneTreeNodeProperties",
    // embind types a std::string parameter as its own EmbindString union, kept here so the
    // signature still accepts everything the binding really accepts.
    pattern: /^(\s*)getSceneTreeNodeProperties\(_0: EmbindString\): any;$/m,
    replacement: "$1getSceneTreeNodeProperties(path: EmbindString): Glance3DNodeProperty[];",
  },
  {
    name: "Scene.setSceneTreeTypeFilter",
    pattern: /^(\s*)setSceneTreeTypeFilter\(_0: any\): void;$/m,
    replacement: "$1setSceneTreeTypeFilter(types: Glance3DNodeType[]): void;",
  },
  {
    name: "Engine.getReadersInfo",
    pattern: /^(\s*)getReadersInfo\(\): any;$/m,
    replacement: "$1getReadersInfo(): Glance3DReaderInfo[];",
  },
  {
    name: "Log.forward",
    pattern: /^(\s*)forward\(_0: any\): void;$/m,
    replacement: "$1forward(_0: (level: Glance3DLogLevel, message: string) => void): void;",
  },
  // The hand-written file redeclares both of these, so the generated ones have to go.
  {
    name: "generated MainModule alias",
    pattern: /^export type MainModule = WasmModule & typeof RuntimeExports & EmbindModule;$/m,
    replacement: "",
  },
  {
    name: "generated module factory",
    pattern:
      /^export default function MainModuleFactory ?\(options\?: unknown\): Promise<MainModule>;$/m,
    replacement: "",
  },
];

/** Symbols the web viewer imports; if any is missing the declarations shipped are not usable. */
const REQUIRED_TYPE_SYMBOLS = [
  "Glance3DModule",
  "Glance3DModuleFactory",
  "Glance3DFactoryOptions",
  "Glance3DGLTFNamespace",
  "Glance3DReaderInfo",
  "Glance3DDataInfo",
  "Glance3DTreeRow",
  "Glance3DTreeInfo",
  "Glance3DNodeType",
  "getSceneTreeRows(begin: number, count: number): Glance3DTreeRow[];",
  "getSceneTreeInfo(): Glance3DTreeInfo;",
  "setSceneTreeTypeFilter(types: Glance3DNodeType[]): void;",
  "getSceneTreeNodeProperties(path: EmbindString): Glance3DNodeProperty[];",
  "Glance3DNodeProperty",
  "activateSceneTreeNode",
  "getG3DDataInfo(): Glance3DDataInfo;",
];

/**
 * Assemble dist/f3d.d.ts from the emcc-generated declarations plus the hand-written ones.
 *
 * The hand-written half lives in `f3d.types.d.ts` as real, formatted TypeScript rather than a
 * multi-kilobyte string literal in here, and both halves are checked after assembly.
 */
function patchTypes(inputPath, outputPath) {
  let types = fs.readFileSync(inputPath, "utf8");

  for (const { name, pattern, replacement } of TYPE_SUBSTITUTIONS) {
    if (!pattern.test(types)) {
      throw new Error(
        `Type patching failed: no match for "${name}" in ${inputPath}.\n` +
          `The emcc-generated declarations changed shape; update TYPE_SUBSTITUTIONS in ` +
          `webassembly/build-local.mjs to match.`,
      );
    }
    types = types.replace(pattern, replacement);
  }

  const handWrittenPath = path.join(scriptDir, "f3d.types.d.ts");
  const handWritten = fs.readFileSync(handWrittenPath, "utf8");
  types = `${types.trimEnd()}\n\n${handWritten}`;

  const missing = REQUIRED_TYPE_SYMBOLS.filter((symbol) => !types.includes(symbol));
  if (missing.length > 0) {
    throw new Error(
      `Type patching produced declarations missing: ${missing.join(", ")}.\n` +
        `Check webassembly/f3d.types.d.ts and the embind bindings.`,
    );
  }

  fs.writeFileSync(outputPath, types);
  console.log(
    `Wrote ${outputPath} (${TYPE_SUBSTITUTIONS.length} substitutions, ${REQUIRED_TYPE_SYMBOLS.length} symbols verified)`,
  );
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
