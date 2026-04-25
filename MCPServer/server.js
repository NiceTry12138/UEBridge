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
    method:  "POST",
    headers: { "Content-Type": "application/json" },
    body:    JSON.stringify(body),
    signal:  makeSignal(),
  });
  const text = await res.text();
  if (!res.ok) {
    let msg = text;
    try { msg = JSON.parse(text)?.error ?? text; } catch {}
    throw new Error(`UE HTTP ${res.status}: ${msg}`);
  }
  return JSON.parse(text);
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
// Start
// ---------------------------------------------------------------------------

const transport = new StdioServerTransport();
await server.connect(transport);
// Server is now listening on stdio; the process stays alive until the client disconnects.
