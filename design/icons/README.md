# Glance3D 图标资源

定版图标：**光圈 · 石板蓝（Aperture · Slate）**。相机光圈造型，叶片为同色相深浅两档交替，低调简洁。

## 文件清单

| 文件 | 用途 |
|------|------|
| `logo.svg` | **应用图标母版**：透明底、放大版彩色光圈（无圆角底）。`logo.ico/.icns/png` 由它派生 |
| `logo-tile.svg` | 备用母版：带深色圆角底的 tile 版（同一光圈），需要「带底」风格时用 |
| `logo-mono.svg` | 单色实心光圈，`currentColor`；浅色环境深墨、深色环境自动转浅白 |
| `logo.ico` | Windows 多分辨率图标（16/24/32/48/64/128/256，透明底） |
| `logo.icns` | macOS 图标（16→1024，含 @2x 视网膜尺寸，透明底） |
| `logo-white.png` / `logo-black.png` | 256px 白/黑实心光圈剪影（透明底；`logo-black.png` 同时是应用内拖放区 logo） |
| `png/logo-16.png … logo-1024.png` | 位图 16/24/32/48/64/128/256/512/1024（透明底） |

## 配色规格

- 叶片亮档：`#6e7da8`　叶片暗档：`#49567c`（深浅两档交替的 6 叶光圈）
- 中心开口：`#20263a`
- `logo.svg` 光圈相对 512 画布以中心 `scale(1.46)` 放大、透明底、无缝隙描边
- `logo-tile.svg` 额外有：底色渐变 `#1d2233`→`#12141d`、缝隙描边 `#12141d`、圆角 120/512 ≈ 23.4%
- 单色墨色：`#1f2430`（深色模式自动 `#f4f4f4`）

> 注意：透明底版在**深色任务栏/Dock**上，深色叶片与中心会与背景融合、对比偏低；
> 若需在深色背景稳定显示，改用 `logo-tile.svg`（带底）派生整套即可。

## 重新生成（改了 logo.svg 后）

需 ImageMagick（`magick`）与 Python。在本目录执行：

```bash
# 1) 多尺寸 PNG
for s in 16 24 32 48 64 128 256 512 1024; do
  magick -background none -density 384 logo.svg -resize ${s}x${s} png/logo-${s}.png
done

# 2) Windows ICO
magick png/logo-16.png png/logo-24.png png/logo-32.png png/logo-48.png \
       png/logo-64.png png/logo-128.png png/logo-256.png logo.ico

# 3) macOS ICNS（本机 ImageMagick 无 icns 编码器，用 Python 按规范打包）
python pack_icns.py   # 见下方脚本；按 png/ 里的尺寸打包 icp4/icp5/ic07..ic14

# 4) 白/黑剪影
magick -background none logo-mono.svg -resize 256x256 _m.png
magick _m.png -channel RGB -evaluate set 100% +channel logo-white.png
magick _m.png -channel RGB -evaluate set 0      +channel logo-black.png
```

`pack_icns.py` 关键逻辑：按 `[("icp4",16),("icp5",32),("ic07",128),("ic08",256),`
`("ic09",512),("ic10",1024),("ic11",32),("ic12",64),("ic13",256),("ic14",512)]`
读取对应 `png/logo-N.png`，每块 = 4 字节类型码 + 大端长度(含 8 字节头) + PNG 数据，
文件头 `icns` + 大端总长。

## 接入应用（已完成）

已铺进 `resources/`：`logo.svg / logo.ico / logo.icns / logo-mono.svg / logo-white.png /`
`logo-black.png`，以及 `png/logo-{16,24,32,48,64,256}.png → resources/logoNN.png`
（注意 `resources` 里 PNG 命名为 `logoNN.png` 无连字符）。文件名与上游一致，无需改动
`f3d.rc` / `application/CMakeLists.txt` / `BundleInfo.plist` 等引用。

- Windows exe 图标由 `f3d.rc` 编译期嵌入 `logo.ico`；只换 `.ico` 不动 `.rc` 时，
  MSBuild 可能不重编资源——`touch` 一下 `resources/f3d.rc` 再 `cmake --build` 即可。
- 应用内拖放区 logo 由 `F3D_DEFAULT_LOGO`（= `resources/logo-black.png`）编译期嵌入，
  改它需重新构建。
