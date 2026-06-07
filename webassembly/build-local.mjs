import { spawnSync } from "node:child_process";
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

const scriptDir = path.dirname(fileURLToPath(import.meta.url));
const repoRoot = path.resolve(scriptDir, "..");
const buildDir = path.join(repoRoot, "_wasm_build");
const distDir = path.join(repoRoot, "dist");

const command = process.argv[2] ?? "build";
const buildType = process.argv[3] ?? "Release";
const isWindows = process.platform === "win32";

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

  for (const entry of fs.readdirSync(binDir)) {
    fs.cpSync(path.join(binDir, entry), path.join(distDir, entry), { recursive: true });
  }
}

function build() {
  const depsDir = process.env.F3D_WASM_DEPS_DIR;
  const generator = process.env.F3D_WASM_CMAKE_GENERATOR ?? "Ninja";
  const fullPlugins = process.env.F3D_WASM_FULL_PLUGINS === "ON";

  if (!depsDir) {
    throw new Error(
      "F3D_WASM_DEPS_DIR must point to a local install prefix containing WebAssembly builds of VTK and optional dependencies.",
    );
  }

  if (!fs.existsSync(depsDir)) {
    throw new Error(`F3D_WASM_DEPS_DIR does not exist: ${depsDir}`);
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
  case "build":
    build();
    break;
  case "test":
    test();
    break;
  default:
    console.error(`Unknown command: ${command}`);
    console.error("Usage: node webassembly/build-local.mjs <clean|build|test> [build-type]");
    process.exit(1);
}
