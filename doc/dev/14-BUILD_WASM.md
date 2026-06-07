# Build WebAssembly

Glance3D can be built in WebAssembly using emscripten in order to embed it into a web browser.
It is used by our [web viewer](https://glance3d.app/viewer) and covers most of the [libf3d public API](../libf3d/04-LANGUAGE_BINDINGS.md).
A simple example is available [here](https://github.com/glance3d-app/glance3d/blob/master/examples/libf3d/web).

This guide is describing how to build VTK and Glance3D with emscripten on Linux or Windows.

## Building

### Preparing the build

Install `npm`, CMake, Ninja, and the Emscripten SDK locally. Make sure the Emscripten environment is active in the current shell and that `emcmake` is available in `PATH`.

On Windows, a typical setup is:

```powershell
winget install OpenJS.NodeJS.LTS Kitware.CMake Ninja-build.Ninja Git.Git Python.Python.3.12
git clone https://github.com/emscripten-core/emsdk.git C:\emsdk
cd C:\emsdk
.\emsdk install latest
.\emsdk activate latest
.\emsdk_env.ps1
```

Glance3D also needs WebAssembly builds of its C++ dependencies. At minimum, provide a WebAssembly VTK installation. If you want the full web viewer plugin set, also provide WebAssembly builds of Assimp, Draco, OpenCASCADE, web-ifc, and WebP.

Set `F3D_WASM_DEPS_DIR` to the install prefix containing these WebAssembly dependencies:

```powershell
$env:F3D_WASM_DEPS_DIR = "D:\wasm-deps\install"
```

The dependencies must be built with the same Emscripten SDK that is active when building Glance3D. Native Windows, macOS, or Linux libraries cannot be reused for the WebAssembly build.

### Building F3D

From the root of the repository run the following command:

```sh
npm run build
```

On completion, a directory `dist` is created containing the artifacts (`f3d.js` and `f3d.wasm`).

By default, the local build keeps optional plugins disabled to reduce the local dependency requirements. To build the Docker-equivalent plugin set, install the optional WebAssembly dependencies into `F3D_WASM_DEPS_DIR` and run:

```powershell
$env:F3D_WASM_FULL_PLUGINS = "ON"
npm run build
```

You can select a different CMake generator with `F3D_WASM_CMAKE_GENERATOR`.

## Run tests

From the root of the repository, after the build step, run the following command:

```sh
npm test
```

## Integrating

It's possible to generate a local package to use in other javascript projects.
Run the following command:

```sh
npm pack
```

It will build and generate a `f3d-vX.X.X.tgz` file.
This file can be imported into your project:

```sh
npm install f3d-vX.X.X.tgz
```
