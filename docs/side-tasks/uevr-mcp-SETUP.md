# uevr-mcp — local integration setup (2026-05-29)

Goal: let Claude drive a running UEVR instance directly (reflection, Lua exec,
cvar/console, render diagnostics, SDK dumps) instead of bottlenecking on manual
test cycles. This documents the LOCAL build + wiring done on this machine.

Repo: `I:/code/lobotomy-x/uevr-mcp/` (clone of `elliotttate/uevr-mcp`, the
maintained one — the sibling `I:/code/uevr-mcp/` is the older April clone).

## Pieces

1. **C# .NET MCP server** (`mcp-server/`, net9.0) — speaks MCP over stdio to
   Claude Code; talks to the plugin over localhost HTTP + named pipe.
   - Built: `dotnet build mcp-server/UevrMcpServer.csproj -c Release`
   - Output: `mcp-server/bin/Release/net9.0/UevrMcpServer.dll`
   - Smoke test: `dotnet <dll> wait-plugin 500` → `{"ok":false,...timed out...}`
     with no game running (expected); `{"ok":true}` once a game+plugin is live.

2. **C++ UEVR plugin** (`plugin/` → `uevr_mcp.dll`) — loads into the game as a
   UEVR plugin, exposes the HTTP/pipe servers the C# side queries.
   - Built: `cmake -S plugin -B plugin/build -A x64 && cmake --build plugin/build --config Release`
   - Output: `plugin/build/Release/uevr_mcp.dll`
   - Self-contained (vendors uevr/API headers, httplib, nlohmann, lua) — no
     dependency on our UEVR repo internals.

## How it's registered with Claude Code

`I:/code/lobotomy-x/UEVR/.mcp.json` (gitignored — local dev only):
```json
{ "mcpServers": { "uevr": { "type": "stdio", "command": "dotnet",
  "args": ["I:/code/lobotomy-x/uevr-mcp/mcp-server/bin/Release/net9.0/UevrMcpServer.dll"] } } }
```
Project-scoped: opening Claude Code in `I:/code/lobotomy-x/UEVR` exposes the
`uevr_*` tools after a one-time approval prompt. (Alternative: global
registration via `uevr-mcp/release/install.ps1 -McpConfig claude-code-user`,
which merges into `%USERPROFILE%/.claude.json`.)

## Using it (next session)

1. Install the plugin into the game you want to drive:
   - Per-game: copy `uevr_mcp.dll` → `%APPDATA%/UnrealVRMod/<Game>-Win64-Shipping/plugins/`
   - OR global: `%APPDATA%/UnrealVRMod/UEVR/plugins/` (loads for every game)
2. Launch + inject UEVR into the game as usual (our luavrlib build).
3. In Claude Code: `uevr_get_status` to confirm the plugin answered, then e.g.
   `uevr_lua_exec("return uevr.api.get_local_pawn():get_full_name()")`,
   `uevr_search_objects("PlayerController")`, `uevr_get_camera`, etc.
   Full tool list in `uevr-mcp/AGENT.md`; deep reference in `UEVR_GUIDE.md`.

This unblocks self-testing: e.g. after a UEVR rebuild, inject, then verify the
engine hook + cvars + UObjectHook from the MCP instead of asking the user to
read the in-game tree.

## Notes / TODO (from uevr-mcp-fork.md)

- The plugin's reflection predates our luavrlib API surface. To exercise
  `uevr.api_fast`, multistate workers, log-subscribe, etc. through
  `uevr_lua_exec`, the plugin/server still need the updates in
  `uevr-mcp-fork.md` ("What needs updating"). Base reflection/Lua works now.
- Did NOT fork/push to GitHub and did NOT commit the sibling repo (per scope).
