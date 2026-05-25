# Side-task hand-offs

This folder contains coordination docs for work that is **scoped to a separate
session** so it does not pollute the main `luavrlib` branch session context.
Each side-task has its own .md with: scope, status, audit, plan, and concrete
next steps. The actual work happens in **sibling repos** outside the main
UEVR tree.

## Repository layout on disk

The main session works in `I:/code/lobotomy-x/UEVR/` (this repo). The side
tasks have their own sibling clones at the same level:

```
I:/code/lobotomy-x/
    UEVR/             ← main luavrlib branch (this repo)
    uevrlib/          ← jbusfield/uevrlib clone, side-task #1
    uevr-mcp/         ← elliotttate/uevr-mcp clone, side-task #2
    REFramework/      ← NOT cloned yet; reference only (see #3)
```

Keeping the side-task work in sibling repos (rather than inside `UEVR/`) means:
- Each can be pushed to its own fork without polluting the UEVR tree.
- The main session does not have to worry about a giant uncommitted diff
  inside `UEVR/` from a different repo's checkout.
- Future sessions can clone fresh if they prefer.

## The hand-offs

| # | File | Title | TL;DR |
|---|------|-------|-------|
| 1 | `uevrlib-rework.md`            | Strip + rework `jbusfield/uevrlib` | 30 Lua modules + a TArray helper DLL. Roughly half of it duplicates what we now have natively (`uevr.api_fast`, GLM, TArray, multistate). The other half is real VR feature code (attachments, IK, gunstock, reticule, etc.) worth keeping. |
| 2 | `uevr-mcp-fork.md`             | Fork `elliotttate/uevr-mcp`, teach it our new APIs | C# .NET MCP server + C++ UEVR plugin. The plugin needs to expose `uevr.api_fast`, `uevr.spawn_worker`, `uevr.log_*`, the new TArray paths, and the function-caller surface. The MCP server side needs corresponding tool definitions. |
| 3 | `reframework-cross-pollination.md` | What to lift from praydog/REFramework | REFramework has a more mature MCP / C# tooling stack. Use it as a reference for patterns we are missing in `uevr-mcp`. |

## Working style for new sessions

Each hand-off doc is self-contained and assumes the new session has **no
memory of the main session**. They include:
- Inventory of what is in the sibling repo (file lists, line counts).
- Map of each upstream concept → our luavrlib equivalent.
- A prioritized "what to do first" list, with explicit acceptance criteria.
- Pointers to the relevant files inside `UEVR/` that the rework should consume.

When a side-task makes progress, append a "## Status" entry at the top of its
doc with the date and what landed. Do not edit the historical sections.

## Why these tasks exist now

The main luavrlib branch added:
- `uevr.api_fast` (direct SDK accessor bindings)
- Worker threads + shared map + log channel
- TArray<T> first-class support across primitives, FName, FString, UObject*, structs
- GLM Vector/Quat/Matrix/Transform with full metamethod surface
- A pinned Live Function Caller with drag-and-drop
- Pretty-printed return values + drag handles on UObject/UClass returns

Both sibling repos predate this work and either reinvent or pipe-around
pieces of it. The side tasks bring them up to date so they consume the new
surface instead of working around its absence.
