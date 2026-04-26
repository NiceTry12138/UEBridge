/**
 * UAssetRead MCP Server
 *
 * Wraps the UE HTTP API (localhost:8765) as a proper MCP server.
 * Run with:   node server.js
 * Then register this process as an MCP server in your agent config
 * (stdio transport – the process communicates via stdin/stdout).
 *
 * Environment variables:
 *   UE_HTTP_URL   Base URL of the UE HTTP server (default: http://localhost:8765)
 *   UE_TIMEOUT_MS Request timeout in ms (default: 30000)
 */

import { McpServer } from "@modelcontextprotocol/sdk/server/mcp.js";
import { StdioServerTransport } from "@modelcontextprotocol/sdk/server/stdio.js";
import { z } from "zod";
import fetch from "node-fetch";

const BASE_URL = process.env.UE_HTTP_URL ?? "http://localhost:8765";
const TIMEOUT  = parseInt(process.env.UE_TIMEOUT_MS ?? "30000", 10);

// ---------------------------------------------------------------------------
// HTTP helpers
// ---------------------------------------------------------------------------

/** AbortSignal with a configurable timeout */
function makeSignal() {
  return AbortSignal.timeout(TIMEOUT);
}

/** POST JSON to the UE HTTP server, return parsed response body */
async function uePost(path, body) {
  const res = await fetch(`${BASE_URL}${path}`, {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify(body),
    signal: makeSignal(),
  });
  const text = await res.text();
  if (!res.ok) {
    let msg = text;
    try { msg = JSON.parse(text)?.error ?? text; } catch {}
    throw new Error(`UE HTTP ${res.status} on ${path}: ${msg}`);
  }
  try {
    return JSON.parse(text);
  } catch {
    throw new Error(`UE returned non-JSON on ${path}: ${text.slice(0, 200)}`);
  }
}

/** GET with query string params, return parsed response body */
async function ueGet(path, params = {}) {
  const qs = new URLSearchParams(
    Object.entries(params).filter(([, v]) => v !== undefined && v !== null)
  ).toString();
  const url = qs ? `${BASE_URL}${path}?${qs}` : `${BASE_URL}${path}`;
  const res = await fetch(url, { signal: makeSignal() });
  const text = await res.text();
  if (!res.ok) {
    let msg = text;
    try { msg = JSON.parse(text)?.error ?? text; } catch {}
    throw new Error(`UE HTTP ${res.status}: ${msg}`);
  }
  return JSON.parse(text);
}

/** Format a JSON value as pretty-printed text for the MCP response */
function toText(value) {
  return JSON.stringify(value, null, 2);
}

// ---------------------------------------------------------------------------
// MCP Server
// ---------------------------------------------------------------------------

const server = new McpServer({
  name:    "ue-asset-reader",
  version: "1.0.0",
});

// ---- dump_asset ------------------------------------------------------------

server.tool(
  "dump_asset",
  "Export a single Unreal Engine asset to structured JSON. " +
  "Returns properties, functions, graphs, components, inheritance chain, etc. " +
  "depending on the asset type (Blueprint, StaticMesh, Material, DataTable, ...).",
  {
    path: z
      .string()
      .describe(
        'Asset package path, e.g. "/Game/Characters/BP_Hero" or ' +
        '"/Game/ThirdPerson/Blueprints/BP_ThirdPersonCharacter"'
      ),
  },
  async ({ path }) => {
    const data = await uePost("/dump_asset", { path });
    return { content: [{ type: "text", text: toText(data) }] };
  }
);

// ---- list_assets -----------------------------------------------------------

server.tool(
  "list_assets",
  "List Unreal Engine assets under a given content-browser directory path. " +
  "Returns asset paths and types. Use dump_asset to inspect individual assets.",
  {
    path: z
      .string()
      .describe(
        'Directory path ending with "/", e.g. "/Game/" or "/Game/Characters/"'
      ),
    filter: z
      .string()
      .optional()
      .describe(
        'Comma-separated asset class names to include, ' +
        'e.g. "Blueprint" or "Blueprint,DataTable,StaticMesh". Omit for all types.'
      ),
    recursive: z
      .boolean()
      .optional()
      .default(false)
      .describe("Whether to recurse into sub-directories (default false)"),
  },
  async ({ path, filter, recursive }) => {
    const data = await ueGet("/list_assets", {
      path,
      filter:    filter    ?? undefined,
      recursive: recursive ? "true" : undefined,
    });
    return { content: [{ type: "text", text: toText(data) }] };
  }
);

// ---- health_check ----------------------------------------------------------

server.tool(
  "health_check",
  "Check whether the Unreal Editor HTTP server is running and reachable.",
  {},
  async () => {
    const data = await ueGet("/health");
    return { content: [{ type: "text", text: toText(data) }] };
  }
);

// ---------------------------------------------------------------------------
// Asset Inspection Tools
// ---------------------------------------------------------------------------

// ---- get_asset_references --------------------------------------------------

server.tool(
  "get_asset_references",
  "Get dependency/referencer relationships for an Unreal Engine asset. " +
  "Returns a tree of assets that the target depends on and/or assets that reference it.",
  {
    path: z.string().describe('Asset package path, e.g. "/Game/Characters/BP_Hero"'),
    direction: z
      .enum(["dependencies", "referencers", "both"])
      .optional()
      .default("both")
      .describe("Which direction to traverse: dependencies (what it uses), referencers (what uses it), or both"),
    depth: z
      .number()
      .int()
      .min(1)
      .max(10)
      .optional()
      .default(1)
      .describe("How many levels deep to traverse (default 1, max 10)"),
  },
  async ({ path, direction, depth }) => {
    const data = await uePost("/get_asset_references", { path, direction, depth });
    return { content: [{ type: "text", text: toText(data) }] };
  }
);

// ---- dump_niagara_system ---------------------------------------------------

server.tool(
  "dump_niagara_system",
  "Export a Niagara particle system to structured JSON. " +
  "Returns emitters, scripts, renderers, and optionally module input parameters.",
  {
    path: z.string().describe('Niagara System asset path, e.g. "/Game/VFX/NS_Explosion"'),
    include_module_inputs: z
      .boolean()
      .optional()
      .default(true)
      .describe("Whether to include module input parameters (default true)"),
  },
  async ({ path, include_module_inputs }) => {
    const data = await uePost("/dump_niagara_system", { path, include_module_inputs });
    return { content: [{ type: "text", text: toText(data) }] };
  }
);

// ---- dump_level_sequence ---------------------------------------------------

server.tool(
  "dump_level_sequence",
  "Export a Level Sequence asset to structured JSON. " +
  "Returns bindings, tracks, sections, and optionally keyframe data.",
  {
    path: z.string().describe('Level Sequence asset path, e.g. "/Game/Cinematics/LS_Intro"'),
    include_keyframes: z
      .boolean()
      .optional()
      .default(true)
      .describe("Whether to include keyframe channel data (default true)"),
  },
  async ({ path, include_keyframes }) => {
    const data = await uePost("/dump_level_sequence", { path, include_keyframes });
    return { content: [{ type: "text", text: toText(data) }] };
  }
);

// ---- dump_widget_tree ------------------------------------------------------

server.tool(
  "dump_widget_tree",
  "Export a UMG Widget Blueprint's widget tree to structured JSON. " +
  "Returns the full hierarchy of widgets with their properties and slot bindings.",
  {
    path: z.string().describe('Widget Blueprint asset path, e.g. "/Game/UI/WBP_MainMenu"'),
    include_slot_properties: z
      .boolean()
      .optional()
      .default(true)
      .describe("Whether to include panel slot properties (anchors, padding, etc.)"),
  },
  async ({ path, include_slot_properties }) => {
    const data = await uePost("/dump_widget_tree", { path, include_slot_properties });
    return { content: [{ type: "text", text: toText(data) }] };
  }
);

// ---- dump_animation_blueprint ----------------------------------------------

server.tool(
  "dump_animation_blueprint",
  "Export an Animation Blueprint to structured JSON. " +
  "Returns variables, all graphs (AnimGraph, EventGraph), nodes, and connections.",
  {
    path: z.string().describe('Animation Blueprint asset path, e.g. "/Game/Characters/ABP_Hero"'),
  },
  async ({ path }) => {
    const data = await uePost("/dump_animation_blueprint", { path });
    return { content: [{ type: "text", text: toText(data) }] };
  }
);

// ---------------------------------------------------------------------------
// Blueprint CRUD Tools
// ---------------------------------------------------------------------------

// ---- create_blueprint ------------------------------------------------------

server.tool(
  "create_blueprint",
  "Create a new Blueprint asset in the Unreal Editor content browser.",
  {
    name: z.string().describe('Name of the new Blueprint, e.g. "BP_MyActor"'),
    parent_class: z
      .string()
      .optional()
      .default("Actor")
      .describe('Parent class name, e.g. "Actor", "Pawn", "Character", or a full class path'),
    path: z
      .string()
      .optional()
      .default("/Game/Blueprints/")
      .describe('Content browser folder path ending with "/", e.g. "/Game/Blueprints/"'),
  },
  async ({ name, parent_class, path }) => {
    const data = await uePost("/create_blueprint", { name, parent_class, path });
    return { content: [{ type: "text", text: toText(data) }] };
  }
);

// ---- add_blueprint_component -----------------------------------------------

server.tool(
  "add_blueprint_component",
  "Add a component to an existing Blueprint's Simple Construction Script.",
  {
    blueprint_path: z.string().describe('Blueprint asset path, e.g. "/Game/Blueprints/BP_MyActor"'),
    component_type: z
      .string()
      .describe(
        'Component type name, e.g. "StaticMesh", "PointLight", "SpotLight", ' +
        '"Camera", "Box", "Sphere", "Capsule", "Audio", "Arrow", or a full class path'
      ),
    component_name: z
      .string()
      .optional()
      .default("NewComponent")
      .describe("Variable name for the new component"),
    attach_to: z
      .string()
      .optional()
      .describe('Variable name of the parent component to attach to (omit to attach to root)'),
  },
  async ({ blueprint_path, component_type, component_name, attach_to }) => {
    const data = await uePost("/add_blueprint_component", {
      blueprint_path, component_type, component_name, attach_to,
    });
    return { content: [{ type: "text", text: toText(data) }] };
  }
);

// ---- compile_blueprint -----------------------------------------------------

server.tool(
  "compile_blueprint",
  "Compile a Blueprint asset in the Unreal Editor.",
  {
    blueprint_path: z.string().describe('Blueprint asset path, e.g. "/Game/Blueprints/BP_MyActor"'),
  },
  async ({ blueprint_path }) => {
    const data = await uePost("/compile_blueprint", { blueprint_path });
    return { content: [{ type: "text", text: toText(data) }] };
  }
);

// ---- add_blueprint_variable ------------------------------------------------

server.tool(
  "add_blueprint_variable",
  "Add a new member variable to a Blueprint.",
  {
    blueprint_path: z.string().describe('Blueprint asset path'),
    variable_name: z.string().describe('Name of the new variable, e.g. "MyHealth"'),
    variable_type: z
      .string()
      .describe(
        'Variable type: "Boolean", "Integer", "Float", "String", "Name", "Text", ' +
        '"Vector", "Rotator", "Transform", "Color", "Actor", or a class path'
      ),
    default_value: z
      .string()
      .optional()
      .describe('Default value as string, e.g. "1.0", "true", "(X=0,Y=0,Z=100)"'),
    is_instance_editable: z
      .boolean()
      .optional()
      .default(false)
      .describe("Expose to instances in the Details panel (default false)"),
    is_blueprint_read_only: z
      .boolean()
      .optional()
      .default(false)
      .describe("Mark as read-only in Blueprint graphs (default false)"),
  },
  async ({ blueprint_path, variable_name, variable_type, default_value, is_instance_editable, is_blueprint_read_only }) => {
    const data = await uePost("/add_blueprint_variable", {
      blueprint_path, variable_name, variable_type, default_value,
      is_instance_editable, is_blueprint_read_only,
    });
    return { content: [{ type: "text", text: toText(data) }] };
  }
);

// ---- get_blueprint_info ----------------------------------------------------

server.tool(
  "get_blueprint_info",
  "Get detailed information about a Blueprint: parent class, variables, components, and function list.",
  {
    path: z.string().describe('Blueprint asset path, e.g. "/Game/Blueprints/BP_MyActor"'),
  },
  async ({ path }) => {
    const data = await ueGet("/get_blueprint_info", { path });
    return { content: [{ type: "text", text: toText(data) }] };
  }
);

// ---- list_blueprints -------------------------------------------------------

server.tool(
  "list_blueprints",
  "List all Blueprint assets under a content browser directory.",
  {
    path: z
      .string()
      .optional()
      .default("/Game/")
      .describe('Directory path, e.g. "/Game/" or "/Game/Characters/"'),
    recursive: z
      .boolean()
      .optional()
      .default(true)
      .describe("Recurse into sub-directories (default true)"),
  },
  async ({ path, recursive }) => {
    const data = await ueGet("/list_blueprints", { path, recursive: String(recursive) });
    return { content: [{ type: "text", text: toText(data) }] };
  }
);

// ---------------------------------------------------------------------------
// Blueprint Node Tools
// ---------------------------------------------------------------------------

// ---- add_blueprint_event_node ----------------------------------------------

server.tool(
  "add_blueprint_event_node",
  "Add an event node to a Blueprint's EventGraph. " +
  "Creates an override event node for parent-class events (e.g. BeginPlay, Tick) " +
  "or a Custom Event node for any other name.",
  {
    blueprint_path: z.string().describe('Blueprint asset path'),
    event_name: z
      .string()
      .describe('Event name, e.g. "BeginPlay", "Tick", "ActorBeginOverlap", or any custom name'),
    node_position: z
      .object({ x: z.number(), y: z.number() })
      .optional()
      .describe("Graph position in pixels (omit for auto-layout)"),
  },
  async ({ blueprint_path, event_name, node_position }) => {
    const data = await uePost("/add_blueprint_event_node", {
      blueprint_path, event_name, node_position,
    });
    return { content: [{ type: "text", text: toText(data) }] };
  }
);

// ---- add_blueprint_function_node -------------------------------------------

server.tool(
  "add_blueprint_function_node",
  "Add a function call node to a Blueprint graph. " +
  "Searches the Blueprint's own functions first, then common engine libraries " +
  "(KismetSystemLibrary, KismetMathLibrary, GameplayStatics, etc.) " +
  "and all BlueprintFunctionLibrary subclasses.",
  {
    blueprint_path: z.string().describe('Blueprint asset path'),
    function_name: z
      .string()
      .describe('Function name, e.g. "PrintString", "GetActorLocation", "SetActorLocation"'),
    target_class: z
      .string()
      .optional()
      .describe(
        'Class to search first, e.g. "KismetSystemLibrary", "KismetMathLibrary", ' +
        'or a full class path. Omit to search all known libraries.'
      ),
    graph_name: z
      .string()
      .optional()
      .default("EventGraph")
      .describe('Graph to add the node to (default "EventGraph")'),
    node_position: z
      .object({ x: z.number(), y: z.number() })
      .optional()
      .describe("Graph position in pixels (omit for auto-layout)"),
  },
  async ({ blueprint_path, function_name, target_class, graph_name, node_position }) => {
    const data = await uePost("/add_blueprint_function_node", {
      blueprint_path, function_name, target_class, graph_name, node_position,
    });
    return { content: [{ type: "text", text: toText(data) }] };
  }
);

// ---- connect_blueprint_nodes -----------------------------------------------

server.tool(
  "connect_blueprint_nodes",
  "Connect two pins between nodes in a Blueprint graph. " +
  'Use "then" for the unnamed output exec pin and "execute" for the unnamed input exec pin.',
  {
    blueprint_path: z.string().describe('Blueprint asset path'),
    source_node_id: z.string().describe("GUID of the source node (from node_id field)"),
    source_pin: z
      .string()
      .describe('Source pin name, e.g. "then", "ReturnValue", "OutHit"'),
    target_node_id: z.string().describe("GUID of the target node"),
    target_pin: z
      .string()
      .describe('Target pin name, e.g. "execute", "Target", "WorldContextObject"'),
    graph_name: z
      .string()
      .optional()
      .default("EventGraph")
      .describe('Graph name (default "EventGraph")'),
  },
  async ({ blueprint_path, source_node_id, source_pin, target_node_id, target_pin, graph_name }) => {
    const data = await uePost("/connect_blueprint_nodes", {
      blueprint_path, source_node_id, source_pin, target_node_id, target_pin, graph_name,
    });
    return { content: [{ type: "text", text: toText(data) }] };
  }
);

// ---- find_blueprint_nodes --------------------------------------------------

server.tool(
  "find_blueprint_nodes",
  "Search for nodes in a Blueprint graph by class name or title substring.",
  {
    blueprint_path: z.string().describe('Blueprint asset path'),
    graph_name: z
      .string()
      .optional()
      .default("EventGraph")
      .describe('Graph to search (default "EventGraph")'),
    node_class: z
      .string()
      .optional()
      .describe('Partial class name filter, e.g. "K2Node_Event", "K2Node_CallFunction"'),
    node_title: z
      .string()
      .optional()
      .describe('Partial title filter, e.g. "BeginPlay", "Print String"'),
  },
  async ({ blueprint_path, graph_name, node_class, node_title }) => {
    const data = await uePost("/find_blueprint_nodes", {
      blueprint_path, graph_name, node_class, node_title,
    });
    return { content: [{ type: "text", text: toText(data) }] };
  }
);

// ---- add_blueprint_var_get_node --------------------------------------------

server.tool(
  "add_blueprint_var_get_node",
  "Add a Variable Get node for a Blueprint member variable.",
  {
    blueprint_path: z.string().describe('Blueprint asset path'),
    variable_name: z.string().describe('Variable name to read, e.g. "MyHealth"'),
    graph_name: z
      .string()
      .optional()
      .default("EventGraph")
      .describe('Graph name (default "EventGraph")'),
    node_position: z
      .object({ x: z.number(), y: z.number() })
      .optional()
      .describe("Graph position (omit for auto-layout)"),
  },
  async ({ blueprint_path, variable_name, graph_name, node_position }) => {
    const data = await uePost("/add_blueprint_var_get_node", {
      blueprint_path, variable_name, graph_name, node_position,
    });
    return { content: [{ type: "text", text: toText(data) }] };
  }
);

// ---- add_blueprint_var_set_node --------------------------------------------

server.tool(
  "add_blueprint_var_set_node",
  "Add a Variable Set node for a Blueprint member variable.",
  {
    blueprint_path: z.string().describe('Blueprint asset path'),
    variable_name: z.string().describe('Variable name to write, e.g. "MyHealth"'),
    graph_name: z
      .string()
      .optional()
      .default("EventGraph")
      .describe('Graph name (default "EventGraph")'),
    node_position: z
      .object({ x: z.number(), y: z.number() })
      .optional()
      .describe("Graph position (omit for auto-layout)"),
  },
  async ({ blueprint_path, variable_name, graph_name, node_position }) => {
    const data = await uePost("/add_blueprint_var_set_node", {
      blueprint_path, variable_name, graph_name, node_position,
    });
    return { content: [{ type: "text", text: toText(data) }] };
  }
);

// ---- get_blueprint_node_pins -----------------------------------------------

server.tool(
  "get_blueprint_node_pins",
  "Get all input and output pins of a specific Blueprint graph node. " +
  "Returns pin names, types, default values, and connection info.",
  {
    blueprint_path: z.string().describe('Blueprint asset path'),
    node_id: z.string().describe("GUID of the node (from node_id field)"),
    graph_name: z
      .string()
      .optional()
      .default("EventGraph")
      .describe('Graph name (default "EventGraph")'),
    include_hidden: z
      .boolean()
      .optional()
      .default(false)
      .describe("Include hidden/internal pins (default false)"),
  },
  async ({ blueprint_path, node_id, graph_name, include_hidden }) => {
    const data = await uePost("/get_blueprint_node_pins", {
      blueprint_path, node_id, graph_name, include_hidden,
    });
    return { content: [{ type: "text", text: toText(data) }] };
  }
);

// ---- delete_blueprint_node -------------------------------------------------

server.tool(
  "delete_blueprint_node",
  "Delete a node from a Blueprint graph by its GUID.",
  {
    blueprint_path: z.string().describe('Blueprint asset path'),
    node_id: z.string().describe("GUID of the node to delete"),
    graph_name: z
      .string()
      .optional()
      .default("EventGraph")
      .describe('Graph name (default "EventGraph")'),
  },
  async ({ blueprint_path, node_id, graph_name }) => {
    const data = await uePost("/delete_blueprint_node", {
      blueprint_path, node_id, graph_name,
    });
    return { content: [{ type: "text", text: toText(data) }] };
  }
);

// ---- batch_edit_blueprint_nodes --------------------------------------------

server.tool(
  "batch_edit_blueprint_nodes",
  "Create multiple Blueprint nodes and connections in a single call. " +
  "Use temp_ids to reference newly created nodes in the connections array. " +
  "Supported node types: event, custom_event, function, variable_get, variable_set, self.",
  {
    blueprint_path: z.string().describe('Blueprint asset path'),
    graph_name: z
      .string()
      .optional()
      .default("EventGraph")
      .describe('Graph name (default "EventGraph")'),
    nodes: z
      .array(
        z.object({
          type: z
            .enum(["event", "custom_event", "function", "variable_get", "variable_set", "self"])
            .describe("Node type"),
          temp_id: z
            .string()
            .optional()
            .describe('Temporary ID used to reference this node in the connections array, e.g. "n1"'),
          event_name: z.string().optional().describe('For type "event" or "custom_event"'),
          function_name: z.string().optional().describe('For type "function"'),
          target_class: z.string().optional().describe('For type "function": class to search first'),
          variable_name: z.string().optional().describe('For type "variable_get" or "variable_set"'),
          node_position: z
            .object({ x: z.number(), y: z.number() })
            .optional()
            .describe("Position in the graph (omit for auto-layout)"),
        })
      )
      .optional()
      .describe("Nodes to create"),
    connections: z
      .array(
        z.object({
          source_temp_id: z
            .string()
            .optional()
            .describe("temp_id of the source node (preferred over source_node_id)"),
          source_node_id: z
            .string()
            .optional()
            .describe("GUID of an existing source node"),
          source_pin: z.string().describe('Source pin name, e.g. "then"'),
          target_temp_id: z
            .string()
            .optional()
            .describe("temp_id of the target node"),
          target_node_id: z
            .string()
            .optional()
            .describe("GUID of an existing target node"),
          target_pin: z.string().describe('Target pin name, e.g. "execute"'),
        })
      )
      .optional()
      .describe("Connections to make between nodes"),
  },
  async ({ blueprint_path, graph_name, nodes, connections }) => {
    const data = await uePost("/batch_edit_blueprint_nodes", {
      blueprint_path, graph_name, nodes, connections,
    });
    return { content: [{ type: "text", text: toText(data) }] };
  }
);

// ---------------------------------------------------------------------------
// Start
// ---------------------------------------------------------------------------

// 启动前打日志（stderr）
console.error(`[ue-mcp] connecting UE at ${BASE_URL}, timeout=${TIMEOUT}ms`);

const transport = new StdioServerTransport();
await server.connect(transport);
console.error(`[ue-mcp] ready`);
// Server is now listening on stdio; the process stays alive until the client disconnects.
