import * as os from 'os';
import * as std from 'std';

import {
    LIST_TOP_Y, FOOTER_RULE_Y,
    truncateText
} from '/data/UserData/schwung/shared/chain_ui_views.mjs';
import {
    drawMenuHeader as drawHeader,
    drawMenuFooter as drawFooter,
    drawMenuList
} from '/data/UserData/schwung/shared/menu_layout.mjs';
import {
    announce,
    announceMenuItem
} from '/data/UserData/schwung/shared/screen_reader.mjs';
import { ctx as _ctx } from './shadow_ui_ctx.mjs';

const MODULES_ROOT = "/data/UserData/schwung/modules";
const INPUT_STATE_FILE = "input_modules.json";
const TRACK_COUNT = 4;
const NATIVE_MODULE = {
    id: "native",
    name: "Native",
    abbrev: "NAT",
    description: "Use Move's native input unchanged.",
    component_type: "input_module"
};

let inputStateDir = "/data/UserData/schwung/slot_state";
let inputState = createDefaultState();
let inputModules = null;
let selectedInputTrack = 0;
let selectedInputModuleIndex = 0;
let inputModuleMetaCache = {};

function debugLog(msg) {
    if (_ctx && typeof _ctx.debugLog === "function") {
        _ctx.debugLog(msg);
    }
}

function hostFileExists(path) {
    try {
        if (typeof host_file_exists === "function") return !!host_file_exists(path);
    } catch (e) {}
    try {
        const st = os.stat(path);
        return !!st;
    } catch (e) {
        return false;
    }
}

function readTextFile(path) {
    try {
        if (typeof host_read_file === "function") return host_read_file(path);
    } catch (e) {}
    try {
        return std.loadFile(path);
    } catch (e) {
        return null;
    }
}

function writeTextFile(path, content) {
    try {
        if (typeof host_write_file === "function") {
            host_write_file(path, content);
            return true;
        }
    } catch (e) {
        return false;
    }
    return false;
}

function ensureDir(path) {
    try {
        if (typeof host_ensure_dir === "function") {
            host_ensure_dir(path);
            return true;
        }
    } catch (e) {}
    return false;
}

function setShimInputParam(slot, key, value) {
    try {
        if (typeof shadow_set_param_timeout === "function") {
            return !!shadow_set_param_timeout(slot, key, String(value), 500);
        }
    } catch (e) {}
    try {
        if (typeof shadow_set_param === "function") {
            return !!shadow_set_param(slot, key, String(value));
        }
    } catch (e) {}
    return false;
}

function syncInputRuntimeTrack(track) {
    const state = getTrackState(track);
    setShimInputParam(track, "input_module:module", state.module || "native");
    if (state.params && typeof state.params === "object") {
        for (const key of Object.keys(state.params)) {
            setShimInputParam(track, "input_module:param:" + key, state.params[key]);
        }
    }
}

function syncInputRuntimeStateDir(dir) {
    setShimInputParam(0, "input_module:state_dir", dir || inputStateDir);
}

function syncInputRuntimeAll() {
    syncInputRuntimeStateDir(inputStateDir);
    for (let i = 0; i < TRACK_COUNT; i++) syncInputRuntimeTrack(i);
}

function statePath(dir) {
    return `${dir || inputStateDir}/${INPUT_STATE_FILE}`;
}

function normalizeTrackIndex(track) {
    const n = parseInt(track, 10);
    if (!Number.isFinite(n)) return 0;
    if (n < 0) return 0;
    if (n >= TRACK_COUNT) return TRACK_COUNT - 1;
    return n;
}

function createDefaultTrack(track) {
    return {
        track,
        module: "native",
        params: {},
        led_mode: "native"
    };
}

function createDefaultState() {
    const tracks = [];
    for (let i = 0; i < TRACK_COUNT; i++) tracks.push(createDefaultTrack(i));
    return { version: 1, tracks };
}

function normalizeState(raw) {
    const next = createDefaultState();
    if (!raw || typeof raw !== "object") return next;
    const rawTracks = Array.isArray(raw.tracks) ? raw.tracks : [];
    for (let i = 0; i < TRACK_COUNT; i++) {
        const t = rawTracks[i] && typeof rawTracks[i] === "object" ? rawTracks[i] : {};
        next.tracks[i] = {
            track: i,
            module: typeof t.module === "string" && t.module.length ? t.module : "native",
            params: t.params && typeof t.params === "object" ? t.params : {},
            led_mode: typeof t.led_mode === "string" && t.led_mode.length ? t.led_mode : "native"
        };
    }
    return next;
}

export function loadInputModuleState(dir) {
    inputStateDir = dir || inputStateDir;
    ensureDir(inputStateDir);

    const raw = readTextFile(statePath(inputStateDir));
    if (raw && raw.trim()) {
        try {
            inputState = normalizeState(JSON.parse(raw));
            syncInputRuntimeAll();
            return inputState;
        } catch (e) {
            debugLog("input modules: failed to parse state: " + e);
        }
    }

    inputState = createDefaultState();
    saveInputModuleState();
    syncInputRuntimeAll();
    return inputState;
}

export function setInputModuleStateDir(dir) {
    inputStateDir = dir || inputStateDir;
    return loadInputModuleState(inputStateDir);
}

export function ensureInputModuleState(dir) {
    const path = statePath(dir || inputStateDir);
    if (!hostFileExists(path)) {
        const prevDir = inputStateDir;
        const prevState = inputState;
        inputStateDir = dir || inputStateDir;
        inputState = createDefaultState();
        saveInputModuleState();
        inputStateDir = prevDir;
        inputState = prevState;
    }
}

export function copyInputModuleState(sourceDir, targetDir) {
    if (!sourceDir || !targetDir) return false;
    const src = readTextFile(statePath(sourceDir));
    if (src && src.trim()) {
        ensureDir(targetDir);
        return writeTextFile(statePath(targetDir), src);
    }
    ensureInputModuleState(targetDir);
    return true;
}

export function saveInputModuleState() {
    ensureDir(inputStateDir);
    return writeTextFile(statePath(inputStateDir), JSON.stringify(inputState, null, 2) + "\n");
}

function getTrackState(track) {
    const idx = normalizeTrackIndex(track);
    if (!inputState || !Array.isArray(inputState.tracks)) inputState = createDefaultState();
    if (!inputState.tracks[idx]) inputState.tracks[idx] = createDefaultTrack(idx);
    return inputState.tracks[idx];
}

function moduleType(meta) {
    return (meta && (meta.component_type ||
        (meta.capabilities && (meta.capabilities.component_type ||
                               (meta.capabilities.input_module ? "input_module" : ""))))) || "";
}

function addModule(result, seen, meta, fallbackId) {
    if (!meta || typeof meta !== "object") return;
    const id = String(meta.id || fallbackId || "");
    if (!id || seen[id]) return;
    if (moduleType(meta) !== "input_module") return;
    const item = {
        id,
        name: meta.name || id,
        abbrev: meta.abbrev || id.substring(0, 3).toUpperCase(),
        description: meta.description || "",
        meta
    };
    seen[id] = true;
    result.push(item);
    if (hasFullInputModuleMetadata(meta)) {
        inputModuleMetaCache[id] = meta;
    }
}

function hasFullInputModuleMetadata(meta) {
    return !!(meta && typeof meta === "object" &&
        (meta.ui_hierarchy || meta.input || meta.chain_params ||
         (meta.capabilities && (meta.capabilities.ui_hierarchy ||
                                meta.capabilities.chain_params ||
                                meta.capabilities.input_module))));
}

function readModuleJson(path) {
    try {
        const raw = std.loadFile(path);
        if (!raw) return null;
        return JSON.parse(raw);
    } catch (e) {
        return null;
    }
}

function scanModuleDir(dirPath, result, seen) {
    try {
        const entries = os.readdir(dirPath) || [];
        const list = entries[0];
        if (!Array.isArray(list)) return;
        for (const entry of list) {
            if (entry === "." || entry === "..") continue;
            const meta = readModuleJson(`${dirPath}/${entry}/module.json`);
            addModule(result, seen, meta, entry);
        }
    } catch (e) {
        /* Directory may not exist on older installs. */
    }
}

export function scanForInputModules() {
    const result = [{ ...NATIVE_MODULE, meta: NATIVE_MODULE }];
    const seen = { native: true };
    inputModuleMetaCache.native = NATIVE_MODULE;

    try {
        if (typeof host_list_modules === "function") {
            const modules = host_list_modules() || [];
            for (const mod of modules) addModule(result, seen, mod, mod && mod.id);
        }
    } catch (e) {}

    scanModuleDir(`${MODULES_ROOT}/inputs`, result, seen);
    scanModuleDir(`${MODULES_ROOT}/input_modules`, result, seen);
    scanModuleDir(MODULES_ROOT, result, seen);

    const native = result[0];
    const rest = result.slice(1);
    rest.sort((a, b) => String(a.name || a.id).localeCompare(String(b.name || b.id)));
    inputModules = [native, ...rest];
    return inputModules;
}

export function getInputModuleMetadata(moduleId) {
    const id = moduleId || "native";
    const cached = inputModuleMetaCache[id];
    if (cached && hasFullInputModuleMetadata(cached)) return cached;
    try {
        if (typeof host_get_module_metadata === "function") {
            const meta = host_get_module_metadata(id);
            if (meta) {
                inputModuleMetaCache[id] = meta;
                return meta;
            }
        }
    } catch (e) {}

    const searchDirs = [
        `${MODULES_ROOT}/inputs/${id}`,
        `${MODULES_ROOT}/${id}`,
        `${MODULES_ROOT}/input_modules/${id}`
    ];
    for (const dir of searchDirs) {
        const meta = readModuleJson(`${dir}/module.json`);
        if (meta) {
            inputModuleMetaCache[id] = meta;
            return meta;
        }
    }
    return id === "native" ? NATIVE_MODULE : null;
}

function findParamMetaInLevel(levelDef, key) {
    const params = levelDef && Array.isArray(levelDef.params) ? levelDef.params : [];
    for (const param of params) {
        if (param && typeof param === "object" && param.key === key) return param;
        if (typeof param === "string" && param === key) return { key };
    }
    return null;
}

function findInputParamMeta(moduleId, key) {
    const hierarchy = getInputModuleHierarchyForModule(moduleId);
    const levels = hierarchy && hierarchy.levels ? hierarchy.levels : {};
    for (const levelName of Object.keys(levels)) {
        const meta = findParamMetaInLevel(levels[levelName], key);
        if (meta) return meta;
    }
    return null;
}

function defaultForMeta(meta) {
    if (!meta || typeof meta !== "object") return "";
    if (meta.default !== undefined) return meta.default;
    if (meta.value !== undefined) return meta.value;
    if (meta.type === "bool") return "0";
    if (meta.type === "int" || meta.type === "float" || meta.type === "note") {
        if (meta.min !== undefined) return meta.min;
        if (meta.min_note !== undefined) return meta.min_note;
        return 0;
    }
    if (meta.type === "enum" && Array.isArray(meta.options) && meta.options.length > 0) {
        return meta.options[0];
    }
    return "";
}

function metadataChainParams(meta) {
    if (!meta || typeof meta !== "object") return [];
    if (Array.isArray(meta.chain_params)) return meta.chain_params;
    if (meta.capabilities && Array.isArray(meta.capabilities.chain_params)) {
        return meta.capabilities.chain_params;
    }
    return [];
}

function deriveChainParamsFromHierarchy(hierarchy) {
    const result = [];
    const seen = {};
    const levels = hierarchy && hierarchy.levels ? hierarchy.levels : {};
    for (const levelName of Object.keys(levels)) {
        const params = levels[levelName].params || [];
        for (const p of params) {
            if (!p || typeof p !== "object" || !p.key || p.level) continue;
            if (seen[p.key]) continue;
            seen[p.key] = true;
            result.push(p);
        }
    }
    return result;
}

function getInputModuleHierarchyForModule(moduleId) {
    const meta = getInputModuleMetadata(moduleId);
    const hierarchy = meta && (meta.ui_hierarchy ||
        (meta.capabilities && meta.capabilities.ui_hierarchy));
    if (hierarchy && typeof hierarchy === "object") return hierarchy;
    return {
        levels: {
            root: {
                name: meta && meta.name ? meta.name : "Input Module",
                params: [],
                knobs: []
            }
        }
    };
}

export function getInputModuleHierarchy(track) {
    const state = getTrackState(track);
    return getInputModuleHierarchyForModule(state.module || "native");
}

export function getInputModuleChainParams(track) {
    const state = getTrackState(track);
    const meta = getInputModuleMetadata(state.module || "native");
    const direct = metadataChainParams(meta);
    if (direct.length > 0) return direct;
    return deriveChainParamsFromHierarchy(getInputModuleHierarchy(track));
}

export function getInputModuleName(track) {
    const state = getTrackState(track);
    const meta = getInputModuleMetadata(state.module || "native");
    return (meta && meta.name) || state.module || "Native";
}

export function getInputModuleAbbrev(track) {
    const state = getTrackState(track);
    const meta = getInputModuleMetadata(state.module || "native");
    return (meta && meta.abbrev) || (state.module || "IN").substring(0, 3).toUpperCase();
}

function stripInputPrefix(key) {
    const raw = String(key || "");
    return raw.indexOf("input:") === 0 ? raw.slice(6) : raw;
}

export function getInputSlotParam(slot, fullKey) {
    const state = getTrackState(slot);
    const key = stripInputPrefix(fullKey);
    const moduleId = state.module || "native";
    const meta = getInputModuleMetadata(moduleId);

    if (key.endsWith(":base") || key.endsWith(":modulated")) {
        return null;
    }

    if (key === "module" || key === "module_id") return moduleId;
    if (key === "name" || key === "display_name") return getInputModuleName(slot);
    if (key === "abbrev") return getInputModuleAbbrev(slot);
    if (key === "led_mode") return state.led_mode || "native";
    if (key === "ui_hierarchy") return JSON.stringify(getInputModuleHierarchy(slot));
    if (key === "chain_params") return JSON.stringify(getInputModuleChainParams(slot));
    if (key === "is_loading" || key === "load_error" || key === "error") return "";

    if (Object.prototype.hasOwnProperty.call(state.params, key)) {
        return String(state.params[key]);
    }

    const paramMeta = findInputParamMeta(moduleId, key);
    const def = defaultForMeta(paramMeta);
    if (def === undefined || def === null) return "";
    return String(def);
}

export function setInputSlotParam(slot, fullKey, value) {
    const state = getTrackState(slot);
    const key = stripInputPrefix(fullKey);
    if (key === "module" || key === "module_id") {
        state.module = value ? String(value) : "native";
        state.params = {};
        setShimInputParam(slot, "input_module:module", state.module);
    } else if (key === "led_mode") {
        state.led_mode = value ? String(value) : "native";
    } else if (key === "ui_hierarchy" || key === "chain_params" ||
               key === "name" || key === "display_name" ||
               key === "abbrev" || key === "is_loading" ||
               key === "load_error" || key === "error") {
        return true;
    } else {
        state.params[key] = String(value);
        setShimInputParam(slot, "input_module:param:" + key, state.params[key]);
    }
    saveInputModuleState();
    return true;
}

export function enterInputModuleSelect(track) {
    selectedInputTrack = normalizeTrackIndex(track);
    if (!inputModules) inputModules = scanForInputModules();
    const current = getTrackState(selectedInputTrack).module || "native";
    const idx = inputModules.findIndex(m => m.id === current);
    selectedInputModuleIndex = idx >= 0 ? idx : 0;
    if (_ctx && _ctx.VIEWS && typeof _ctx.setView === "function") {
        _ctx.setView(_ctx.VIEWS.INPUT_MODULE_SELECT);
    } else if (_ctx) {
        _ctx.view = "inputmodselect";
    }
    if (_ctx) _ctx.needsRedraw = true;
    const selected = inputModules[selectedInputModuleIndex] || NATIVE_MODULE;
    announce(`Track ${selectedInputTrack + 1} input, ${selected.name || selected.id}`);
}

export function drawInputModuleSelect() {
    if (!inputModules) inputModules = scanForInputModules();
    const current = getTrackState(selectedInputTrack).module || "native";
    if (typeof clear_screen === "function") clear_screen();
    drawHeader(`Track ${selectedInputTrack + 1} Input`);

    const items = inputModules.map(mod => ({
        id: mod.id,
        name: mod.name || mod.id,
        value: mod.id === current ? "*" : ""
    }));

    drawMenuList({
        items,
        selectedIndex: selectedInputModuleIndex,
        listArea: { topY: LIST_TOP_Y, bottomY: FOOTER_RULE_Y },
        getLabel: (item) => truncateText(item.name, 18),
        getValue: (item) => item.value,
        valueAlignRight: true
    });
    drawFooter({ left: "Push: select", right: "Jog: scroll" });
}

export function handleInputModuleSelectJog(delta) {
    if (!inputModules) inputModules = scanForInputModules();
    selectedInputModuleIndex = Math.max(0, Math.min(inputModules.length - 1, selectedInputModuleIndex + delta));
    const selected = inputModules[selectedInputModuleIndex] || NATIVE_MODULE;
    announceMenuItem("Input", selected.name || selected.id || "Native");
    if (_ctx) _ctx.needsRedraw = true;
}

export function handleInputModuleSelectSelect() {
    if (!inputModules) inputModules = scanForInputModules();
    const selected = inputModules[selectedInputModuleIndex] || NATIVE_MODULE;
    setInputSlotParam(selectedInputTrack, "input:module", selected.id || "native");
    if (_ctx && typeof _ctx.enterInputModuleHierarchy === "function") {
        _ctx.enterInputModuleHierarchy(selectedInputTrack);
    }
    if (_ctx) _ctx.needsRedraw = true;
}

export function handleInputModuleSelectBack() {
    if (typeof shadow_request_exit === "function") {
        shadow_request_exit();
        return;
    }
    announce("Input Module");
    if (_ctx) _ctx.needsRedraw = true;
}
