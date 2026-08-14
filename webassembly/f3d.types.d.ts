// Hand-written TypeScript declarations for the Glance3D wasm module.
//
// `emcc --emit-tsd` can only describe what embind sees, so anything crossing the boundary as an
// `emscripten::val` arrives typed `any`, and anything added by the JS wrapper layer is invisible to
// it entirely. This file supplies both. `build-local.mjs` appends it to the generated declarations
// and verifies afterwards that every symbol declared here actually landed — a check that exists
// because these used to be regex patches that failed silently when the generated text shifted.
//
// Keep this file free of `import`/`export {}` statements other than the `export` keyword on each
// declaration: it is concatenated into the generated module, not compiled on its own.

// -- capability packs and the GLTF namespace ---------------------------------------------------

export type Glance3DCapabilityPack = "gltf-advanced";

export interface Glance3DGLTFInspection {
  isGlb: boolean;
  usedExtensions: string[];
  requiredExtensions: string[];
  advancedExtensions: string[];
  recommendedCapabilityPack: Glance3DCapabilityPack | null;
}

export interface Glance3DGLTFPrepareOptions {
  locateFile?: (path: string) => string;
  fileName?: string;
}

export interface Glance3DVirtualFile {
  path: string;
  data: ArrayBuffer | ArrayBufferView;
}

export interface Glance3DFileSetLoadOptions {
  primaryPath?: string;
  packageName?: string;
}

export interface Glance3DGLTFNamespace {
  inspectBuffer(buffer: ArrayBuffer | ArrayBufferView): Glance3DGLTFInspection;
  prepareBuffer(
    buffer: ArrayBuffer | ArrayBufferView,
    options?: Glance3DGLTFPrepareOptions,
  ): Promise<Uint8Array>;
}

export type Glance3DCapabilityEvent =
  | {
      type: "capability-loading" | "capability-loaded";
      pack: Glance3DCapabilityPack;
      extensions: string[];
    }
  | {
      type: "capability-prepared";
      pack: Glance3DCapabilityPack;
      extensions: string[];
      inputByteLength?: number;
      outputByteLength?: number;
      remainingRequiredExtensions?: string[];
      remainingAdvancedExtensions?: string[];
    }
  | {
      type: "gltf-buffer-filesystem-load";
      pack: Glance3DCapabilityPack | null;
      extensions: string[];
      inputByteLength?: number;
      outputByteLength?: number;
    }
  | {
      type: "file-set-filesystem-load";
      primaryPath: string;
      fileCount: number;
      totalByteLength: number;
    };

// Shorter aliases kept for existing callers.
export type GLTFCapabilityPack = Glance3DCapabilityPack;
export type GLTFInspection = Glance3DGLTFInspection;
export type GLTFPrepareOptions = Glance3DGLTFPrepareOptions;
export type GLTFNamespace = Glance3DGLTFNamespace;
export type GLTFCapabilityEvent = Glance3DCapabilityEvent;

export type Glance3DOptionBag = Options;
export type Glance3DScene = Scene;
export type Glance3DCamera = Camera;
export type Glance3DWindow = Window;
export type Glance3DInteractor = Interactor;
export type Glance3DEngine = Engine;

// -- readers and logging -------------------------------------------------------------------------

export interface Glance3DReaderInfo {
  name: string;
  description: string;
  pluginName: string;
  extensions: string[];
  mimeTypes: string[];
  hasSceneReader: boolean;
  hasGeometryReader: boolean;
}

export type Glance3DLogLevel = LogVerboseLevel | number;

export interface Glance3DFactoryOptions {
  canvas?: HTMLCanvasElement;
  locateFile?: (path: string, prefix?: string) => string;
  print?: (message: string) => void;
  printErr?: (message: string) => void;
}

// -- scene data info -----------------------------------------------------------------------------

export interface Glance3DDataArrayInfo {
  name: string;
  /** "point" or "cell". */
  association: string;
  components: number;
  /** Magnitude range. */
  range: [number, number];
}

export interface Glance3DDataInfo {
  schemaVersion: 1;
  points: number;
  cells: number;
  actors: number;
  files: number;
  hasBounds: boolean;
  bounds?: [number, number, number, number, number, number];
  arrays: Glance3DDataArrayInfo[];
}

// -- scene tree ----------------------------------------------------------------------------------

/** What a node is, independent of the file format it came from. */
export type Glance3DNodeType =
  | "root"
  | "file"
  | "group"
  | "assembly"
  | "part"
  | "instance"
  | "face"
  | "mesh"
  | "point_cloud"
  | "volume"
  | "camera"
  | "light"
  | "skeleton"
  | "joint"
  | "other";

/**
 * One on-screen row, as the shared view-model resolved it.
 *
 * Rows arrive flat: expansion, filtering and the effective-visibility roll-up have already been
 * applied, so a frontend renders exactly what it is given. Rows are fetched one window at a time,
 * which is what keeps the cost of crossing the wasm boundary independent of scene size.
 */
export interface Glance3DTreeRow {
  /** Stable key, eg. "/f3d.glb/Body/Bolt[3]". Survives reloads; safe to persist or deep-link. */
  path: string;
  /** Display text. Empty when the file named nothing — see `placeholder`. */
  label: string;
  /**
   * For an occurrence (`type === "instance"`), the product it is an occurrence of; empty otherwise.
   *
   * Set only when it adds something the label does not — an occurrence named after its own product,
   * the common STEP case, would just repeat itself. The view-model applies that rule, so every
   * frontend shows the target in the same places.
   */
  instanceTarget: string;
  type: Glance3DNodeType;
  /** Indentation level as drawn; loaded files sit at depth 0. */
  depth: number;
  childCount: number;
  /**
   * B-rep faces behind this node, whether or not they have been built into rows yet.
   *
   * Non-zero only for CAD formats that kept the correspondence. A node with faces and no children
   * yet still reports `hasChildren`, because opening it is how the faces get built —
   * `setSceneTreeExpanded` does that and returns false when there are too many to be worth it.
   */
  faceCount: number;
  /** 1-based index among same-parent placeholders of the same kind, or -1 when it stands alone. */
  placeholderOrdinal: number;
  hasChildren: boolean;
  expanded: boolean;
  /** Effective visibility: the node's own toggle AND every ancestor's. */
  visible: boolean;
  /** Visible, but not all of the subtree is — the tri-state an eye icon shows as mixed. */
  partiallyVisible: boolean;
  /** The label was synthesised; substitute a noun plus `placeholderOrdinal`. */
  placeholder: boolean;
  selected: boolean;
  /** Matches the active filter query, as opposed to being kept only to lead to a match. */
  matched: boolean;
  /**
   * The node has something to show or hide. False for cameras: a viewpoint is not part of the
   * picture, so do not offer an eye for it.
   */
  canToggleVisibility: boolean;
}

/**
 * One name/value fact a format attached to a node.
 *
 * Fetched per selected node rather than carried on every row: rows are re-fetched on every scroll,
 * properties are read once when the selection changes.
 */
export interface Glance3DNodeProperty {
  key: string;
  value: string;
}

export interface Glance3DTreeInfo {
  schemaVersion: 3;
  /** Number of rows currently displayable, ie. after expansion and filtering. */
  rowCount: number;
  /** Total number of nodes in the scene, whether displayed or not. */
  nodeCount: number;
  /** Path of the selected node, empty when nothing is selected. */
  selectedPath: string;
  canVisibility: boolean;
  canSolo: boolean;
  canFocus: boolean;
}

// -- module --------------------------------------------------------------------------------------

export type Glance3DModule = WasmModule &
  typeof RuntimeExports &
  EmbindModule & {
    GLTF: Glance3DGLTFNamespace;
    onCapabilityEvent?: (event: Glance3DCapabilityEvent) => void;
  };

export type Glance3DModuleFactory = (
  options?: Glance3DFactoryOptions,
) => Promise<Glance3DModule>;

export type MainModule = Glance3DModule;

export default function MainModuleFactory(
  options?: Glance3DFactoryOptions,
): Promise<Glance3DModule>;
