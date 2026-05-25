# Side-task #3 — Steal from praydog/REFramework

**Repo:** https://github.com/praydog/REFramework
**Not cloned on disk yet.** Defer the clone to the session that actually starts
this work, since REFramework is large and we don't know yet which pieces
we need.

## Why this is here

The user mentioned that REFramework has grown a fairly mature MCP and C#
tooling layer that's noticeably more polished than `elliotttate/uevr-mcp`
in some places. REFramework targets RE Engine games (RE2/3/4/7/8, MHRise,
DMC5, etc.) but the **patterns** it uses for AI-tool integration are
engine-agnostic and worth porting into our `uevr-mcp` fork.

This is a **reference task, not a port task** — we are not bringing
REFramework code in wholesale. We are picking specific subsystems whose
design or implementation pattern is worth lifting into the `uevr-mcp` fork
(side-task #2).

## What to look for

When auditing REFramework as a source of patterns, prioritize the
following (in roughly decreasing leverage):

### 1. C# MCP server patterns

REFramework's MCP server is written in the same .NET tooling
generation as `uevr-mcp`'s, but tends to have:
- More consistent tool naming conventions.
- Better error responses (typed enums vs free-form strings).
- Cleaner separation of "transport layer" from "tool definitions" —
  worth comparing `Tools.cs` in `uevr-mcp` against REFramework's
  equivalent.

Action: skim REFramework's `tools/` (or wherever the MCP lives) and
extract a "patterns we want to adopt" checklist into the `uevr-mcp`
fork PR.

### 2. Object / reflection inspection idioms

REFramework's reflection layer is older and has been hardened by more
games. Worth comparing field-walking, struct-handling, and array
serialization against ours. If REFramework handles a corner case
(e.g. UE5 LWC float-vs-double for FVector inside arrays of structs)
that our `ScriptUtility.cpp::prop_to_object` doesn't, port the
detection.

Action: diff the field-type dispatch tables. Anything REFramework
handles that we don't → file an issue in `uevr-mcp` fork OR fix in
this repo's `ScriptUtility.cpp` if it's relevant to UE too.

### 3. Lua sandboxing / hot-reload

REFramework's Lua hot-reload and per-script isolation are worth
comparing to our `LuaLoader::reset_scripts()` and worker thread
model. If REFramework has cleaner separation of script lifetimes,
copy the pattern.

### 4. CVar / console-variable surface

REFramework exposes RE Engine's equivalent of CVars via the MCP very
cleanly. Our `uevr-mcp` fork should expose UE's CVar tree the same
way; check REFramework's tool definitions for the right shape.

### 5. Tooling not relevant to us

REFramework has piles of code specific to RE Engine (mesh helpers,
material editing, animation graph inspection). Skip all of that —
RE Engine and Unreal Engine reflection layouts don't translate.

## Working method when this task starts

1. Clone REFramework into the sibling tree:
   ```
   I:/code/lobotomy-x/REFramework/
   ```
2. Browse `tools/` or `mcp/` or wherever the MCP lives.
3. For each pattern worth lifting, write a short note in
   `docs/side-tasks/uevr-mcp-fork.md` under a new "Patterns lifted from
   REFramework" section.
4. Actually lift the pattern in a follow-up commit in the `uevr-mcp`
   fork — keep the commit message tagged so it's clear where the design
   came from (e.g. `mcp-tools: adopt REFramework-style error envelope`).

## Status log

_(append-only)_

- 2026-05-25 — initial hand-off doc written. REFramework NOT cloned.
  No code changes yet.
