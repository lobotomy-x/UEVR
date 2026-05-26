# Side-task #4 — UEVR example project (`uevr-example-project`)

**Repo on disk:** `I:/code/lobotomy-x/uevr-example-project/`
**Upstream:** none — created locally for this branch.

## TL;DR

A minimal UE 5.x project + `UEVRTestSurface` plugin that exists solely
to exercise the luavrlib branch of UEVR end-to-end without relying on a
shipping game like InnocentAssault. The plugin defines a single
`AUEVRTestActor` class whose UPROPERTYs cover every FProperty type the
luavrlib branch claims to read/write, and whose UFUNCTIONs cover every
call shape the function caller supports. A paired Lua driver script
walks the whole battery from the LuaLoader sidebar.

## Why this exists

Up to this point the only way to verify a luavrlib change was:

1. Build `UEVRBackend.dll`.
2. Inject into a real game (currently `InnocentAssault-Win64-Shipping`).
3. Drive the UObjectHook menu / Lua scripts against whatever properties
   the game happens to expose.

Coverage was patchy — e.g. the game might not have any
`TSoftObjectPtr<>` fields, so the new soft-object handler is hard to
verify. With UEVRExample we control the property surface, so every
new handler can be exercised against a known layout.

It also doubles as a clean repro target for bug reports: "drop the
example project's `AUEVRTestActor`, set field X, hit caller button Y —
see Z" reproduces consistently across machines.

## What's in the project

```
uevr-example-project/
├── UEVRExample.uproject              # 5.3, Blueprint+C++
├── README.md                         # quick start
├── Config/
│   ├── DefaultEngine.ini             # default map = OpenWorld template
│   └── DefaultGame.ini               # standalone target rules
├── Source/UEVRExample/
│   ├── UEVRExample.Target.cs         # standalone game target
│   ├── UEVRExampleEditor.Target.cs   # editor target
│   ├── UEVRExample.Build.cs
│   ├── UEVRExample.h / .cpp          # game module stub
├── Plugins/UEVRTestSurface/
│   ├── UEVRTestSurface.uplugin       # runtime plugin
│   └── Source/UEVRTestSurface/
│       ├── UEVRTestSurface.Build.cs
│       ├── Public/
│       │   ├── UEVRTestSurface.h     # module interface
│       │   └── UEVRTestActor.h       # the test class — all property types
│       └── Private/
│           ├── UEVRTestSurfaceModule.cpp
│           └── UEVRTestActor.cpp     # records every call to TotalCallCount + LastCallName
└── Scripts/
    └── uevr_example_driver.lua       # paired script — auto-finds actor and drives every UFUNCTION
```

## Coverage matrix

Every entry in this table corresponds to a UPROPERTY (read/write path)
or a UFUNCTION (call path) on `AUEVRTestActor`. Cross-reference with
`docs/luavrlib-changes.md` to confirm parity.

### Properties (matches ScriptUtility.cpp's prop_to_object switch)

| Type                       | Field                  | Notes |
|----------------------------|------------------------|-------|
| BoolProperty               | `Test_Bool`            | |
| ByteProperty               | `Test_Byte`            | |
| IntProperty                | `Test_Int`             | |
| Int64Property              | `Test_Int64`           | |
| FloatProperty              | `Test_Float`           | |
| DoubleProperty             | `Test_Double`          | |
| NameProperty               | `Test_Name`            | |
| StrProperty                | `Test_String`          | |
| TextProperty               | `Test_Text`            | localised default |
| EnumProperty               | `Test_Colour`          | `EUEVRTestColour` (uint8) |
| StructProperty (FVector)   | `Test_Vector`          | |
| StructProperty (FRotator)  | `Test_Rotator`         | |
| StructProperty (FTransform)| `Test_Transform`       | |
| StructProperty (FLinearColor) | `Test_LinearColor`  | |
| StructProperty (custom)    | `Test_Substruct`       | `FUEVRTestSubstruct` |
| ObjectProperty             | `Test_StrongActor`     | strong `TObjectPtr<AActor>` |
| WeakObjectProperty         | `Test_WeakActor`       | `TWeakObjectPtr<AActor>` |
| LazyObjectProperty         | `Test_LazyActor`       | `TLazyObjectPtr<AActor>` |
| SoftObjectProperty         | `Test_SoftActor`       | `TSoftObjectPtr<AActor>` |
| ClassProperty              | `Test_ActorClass`      | `TSubclassOf<AActor>` |
| SoftClassProperty          | `Test_SoftActorClass`  | `TSoftClassPtr<AActor>` |
| InterfaceProperty          | `Test_PingInterface`   | `TScriptInterface<IUEVRPingInterface>` |
| ArrayProperty (primitives) | `Test_IntArray`, `Test_FloatArray` | |
| ArrayProperty (FName/FString) | `Test_NameArray`, `Test_StringArray` | |
| ArrayProperty (struct)     | `Test_VectorArray`, `Test_SubstructArray` | |
| ArrayProperty (object)     | `Test_StrongActorArray`, `Test_WeakActorArray`, `Test_SoftActorArray`, `Test_ClassArray` | exercises stride 8 / 24 / 32 paths |
| MapProperty                | `Test_NameToInt`       | TODO: currently returns nil |
| SetProperty                | `Test_NameSet`         | TODO: currently returns nil |
| DelegateProperty           | `Test_SingleDelegate`  | should display as `<delegate>` |
| MulticastDelegateProperty  | `Test_MulticastDelegate` | |

### Functions (matches render_function_call / encode_param)

| Shape                      | Function name            |
|----------------------------|--------------------------|
| No args                    | `Call_NoArgs`            |
| 1 bool                     | `Call_Bool`              |
| 1 int / int64              | `Call_Int`, `Call_Int64` |
| 1 float / double           | `Call_Float`, `Call_Double` |
| 1 FName / FString / FText  | `Call_Name`, `Call_String`, `Call_Text` |
| 1 enum                     | `Call_Enum`              |
| 1 FVector / FRotator       | `Call_Vector`, `Call_Rotator` |
| 1 FTransform               | `Call_Transform`         |
| 1 FLinearColor             | `Call_LinearColor`       |
| 1 UObject* / TSubclassOf   | `Call_Object`, `Call_Class` |
| 1 custom struct            | `Call_Substruct`         |
| Multi-arg POD              | `Call_MultiArg(int32, float, bool, FVector, FRotator)` |
| Return int/name/string/vec/etc | `Return*`            |
| Return UObject* / UClass*  | `ReturnSelf`, `ReturnClass` |
| Return TArray<various>     | `ReturnIntArray`, `ReturnNameArray`, `ReturnStringArray`, `ReturnVectorArray`, `ReturnActorArray` |
| Out params                 | `OutParams_TwoInts`, `OutParams_Mixed` |

### Telemetry

`AUEVRTestActor` records every call:

- `TotalCallCount` (int32, VisibleAnywhere) — bumps once per call.
- `LastCallName` (FString, VisibleAnywhere) — name of last invoked UFUNCTION.

Use these to verify that the in-game caller's "Call" button actually
landed (not just that the UI registered the click).

## Build / package / inject — full workflow

The C++ scaffolding is written by hand so you can clone-and-go without
running the editor first to generate `Source/`. The actual asset content
(maps, blueprints, test actor instances placed in level) still has to be
authored in the editor.

1. **Open the project**: `UEVRExample.uproject` in UE 5.3 (or compatible
   5.x — adjust `EngineAssociation` field in the `.uproject` if needed).
2. **First-time build**: the editor will prompt to compile the C++
   modules. Let it. Should produce `UEVRExample.dll` + plugin DLL.
3. **Spawn a test actor**: open the default level (OpenWorld template
   or anything else), drag `AUEVRTestActor` from the Place Actors panel
   into the world. Save the level.
4. **Package for Windows**: Platforms → Windows → Package Project →
   Shipping (or Development if you want logs). Pick an output folder.
5. **Inject UEVR**: launch the UEVR injector, point at the cooked
   `UEVRExample.exe` under the Shipping/Win64 staging dir.
6. **Drop in driver script**: copy `Scripts/uevr_example_driver.lua`
   into `%APPDATA%/UnrealVRMod/UEVRExample-Win64-Shipping/scripts/`.
7. **In-game**: open the UObjectHook menu — the actor appears in
   Objects-by-class as `UEVRTestActor`. The LuaLoader sidebar shows the
   `UEVRExample Driver` panel. Verify telemetry bumps after each call.

## Status log

_(append-only)_

- 2026-05-25 — initial scaffolding written: `.uproject`, target rules,
  game module stub, `UEVRTestSurface` plugin, `AUEVRTestActor` class with
  every property type, paired Lua driver, README. Editor work (open,
  compile, place actor, save, package) has NOT been done — the project
  has never been opened in UE Editor. No `.uasset` files exist yet.
