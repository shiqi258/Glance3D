- clip: v1.15
- cxxopts: v3.3.1
- dmon: 1.3.10
- imgui: v1.92.8 — **本地补丁**（升级时须重放）：`io.ScrollbarStyleFn` 逐条滚动条外观回调，
  见 `imgui.h` / `imgui_widgets.cpp` 中标 `[Glance3D]` 的三处。未挂回调时行为与原版逐像素一致。
- nlohmann_json: v3.12.0
- tinyfiledialogs: v3.21.3
