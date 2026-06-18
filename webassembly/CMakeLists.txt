cmake_minimum_required(VERSION 3.17) # 3.17 required for CMP0099

add_executable(f3djs F3DEmscriptenBindings.cxx)

set_target_properties(f3djs PROPERTIES
  OUTPUT_NAME f3d
)

target_link_libraries(f3djs PRIVATE libf3d embind)
target_compile_options(f3djs PRIVATE -fwasm-exceptions)

target_link_options(f3djs PRIVATE
  "$<IF:$<CONFIG:Release>,-Oz,-O0>"
  "-fwasm-exceptions"
  "--emit-tsd=f3d.d.ts"
  "SHELL:-sEXPORT_NAME=f3d"
  "SHELL:-sALLOW_MEMORY_GROWTH=1"
  "SHELL:-sEMULATE_FUNCTION_POINTER_CASTS=0"
  "SHELL:-sMODULARIZE=1"
  "SHELL:-sEXPORT_ES6=1"
  "SHELL:-sSTACK_SIZE=1048576"
  "SHELL:-sWASM=1"
  "SHELL:-sFORCE_FILESYSTEM"
  "SHELL:-sEXPORTED_RUNTIME_METHODS=FS"
  "SHELL:-sWASM_BIGINT"
)

if(F3D_WASM_THREADS)
  # Pthreads on: the async loader's parse runs on a Web Worker pthread. PTHREAD_POOL_SIZE pre-spawns
  # workers so the on-demand parse thread starts without first yielding to the event loop. The
  # runtime must also be allowed to bootstrap in a 'worker' environment. (-pthread itself is applied
  # to every f3d target via the top-level CMakeLists F3D_WASM_THREADS block.)
  target_compile_options(f3djs PRIVATE -pthread)
  target_link_options(f3djs PRIVATE
    "-pthread"
    "SHELL:-sPTHREAD_POOL_SIZE=2"
    "SHELL:-sENVIRONMENT=web,worker"
  )
else()
  target_link_options(f3djs PRIVATE
    "SHELL:-sENVIRONMENT=web"
    "SHELL:-sUSE_PTHREADS=0"
  )
endif()

option(F3D_WASM_BUILD_TESTING "Build WebAssembly tests" OFF)
if(F3D_WASM_BUILD_TESTING)
  enable_testing()
  add_subdirectory(testing)
endif()
