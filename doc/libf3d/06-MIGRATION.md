# Migrate from v3.5

This guide explains how to migrate the libf3d code base from v3.5 to v4.0.

> [!WARNING]
> This guide assumes all deprecation warnings have been addressed, since the deprecated APIs have been removed.

## User callback

When calling `f3d::interactor::start()` or `f3d::interactor::playInteraction()`, it was possible to set a user callback automatically called at each event loop.
If you were setting such callback, please call `f3d::interactor::setEventLoopUserCallback()` manually before calling `start()` or `playInteractor()`.
The callback takes an argument of type `f3d::interactor::interactor_state_t` allowing you to retrieve useful information about the interactor state.

## F3D_PLUGINS_PATH

When running F3D, it was possible to specify the path for loading plugins using the environment variable `F3D_PLUGINS_PATH`. This variable has been removed in favor of the CLI option `--plugins-path` which is more secure.

## Glance3D scene tree API

The recursive scene tree snapshot has been replaced by a windowed row API. `getG3DSceneTree()` and
the types it returned — `g3d_scene_tree_snapshot`, `g3d_scene_tree_node`,
`g3d_scene_tree_capabilities` and `g3d_scene_tree_node_kind` — have been removed, along with the
`g3d:<importer>:<node>` node ids they were keyed by. Those ids were build-order indices: they went
stale on reload and collided across files, so nothing could be persisted against them.

Read the tree with `getSceneTreeInfo()` plus `getSceneTreeRows(begin, count)`, which return rows
already flattened, filtered and rolled up rather than a tree to walk. Node ids become **paths**
(`/f3d.glb/Body/Bolt[3]`), which are stable across reloads and unique across a multi-file scene.
`schemaVersion` moves from 1 to 2 to mark the change.

The write methods keep their behaviour and lose the `G3D` infix, now taking a path:

| v3.5                                    | v4.0                             |
| --------------------------------------- | -------------------------------- |
| `setG3DSceneTreeNodeVisibility(id, b)`  | `setSceneTreeNodeVisibility(path, b)` |
| `setOnlyG3DSceneTreeNodeVisible(id)`    | `setOnlySceneTreeNodeVisible(path)`   |
| `resetG3DSceneTreeVisibility()`         | `resetSceneTreeVisibility()`          |
| `focusG3DSceneTreeNode(id)`             | `focusSceneTreeNode(path)`            |

Expansion, filtering and selection are new and had no v3.5 equivalent: they were previously either
absent or trapped inside a frontend. Recovering a node's path from a scene walked with the old API
is a matter of reading `path` off the rows; there is no id-to-path translation, because the old ids
did not survive long enough to be worth translating.

Roughly, this:

```cpp
f3d::g3d_scene_tree_snapshot tree = scene.getG3DSceneTree();
scene.setG3DSceneTreeNodeVisibility(tree.children[0].id, false);
```

becomes:

```cpp
std::vector<f3d::g3d_tree_row> rows = scene.getSceneTreeRows(0, 1);
scene.setSceneTreeNodeVisibility(rows[0].path, false);
```
