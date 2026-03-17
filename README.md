<p align="center">
  <img src="https://github.githubassets.com/images/modules/logos_page/GitHub-Mark.png" width="80" />
</p>

<h1 align="center">GitHubCopilotUE</h1>

<p align="center">
  <b>AI coding assistant inside Unreal Engine 5 — powered by GitHub Copilot</b>
</p>

<p align="center">
  <a href="#features">Features</a> •
  <a href="#requirements">Requirements</a> •
  <a href="#installation">Installation</a> •
  <a href="#authentication">Authentication</a> •
  <a href="#usage">Usage</a> •
  <a href="#models">Models</a> •
  <a href="#settings">Settings</a> •
  <a href="#architecture">Architecture</a> •
  <a href="#contributing">Contributing</a> •
  <a href="#license">License</a>
</p>

---

A native Unreal Engine 5.x C++ editor plugin that brings GitHub Copilot directly into the Unreal Editor. Chat with AI models, read and edit source files, create C++ classes, trigger compiles, analyze your VR/Quest setup — all from a dockable Slate panel without ever leaving the engine.

This is **not** a mock chat panel. The plugin features a full **agentic tool-calling system** — the AI can autonomously read files, write code, search your project, create classes, and compile, just like Copilot in VS Code.

### ✅ Latest Update (Mar 2026)
- Engine-wide deployment flow validated for UE **5.6** + **5.7** (`Engine/Plugins/Marketplace/GitHubCopilotUE`)
- Shared auth/session behavior across dockable panel + console/REPL
- Unified chat UI with prompt + transcript in one area, plus **Enter to send** (`Shift+Enter` newline)
- Visible in-chat working state via **`Copilot is thinking...`** animation
- Expanded tool surface (`view`, `glob`, `rg`, file ops, compile/live coding/tests, Blueprint asset creation)
- Tool-loop guard is now configurable (`Max Tool-Call Iterations`, default `0` = unlimited)
- Blueprint creation now **auto-saves to disk** (no manual Save All required)

## Features

### 🤖 Agentic AI Assistant
- **Tool-calling loop** — the AI can chain multiple tool calls (read → analyze → edit → compile) in a single conversation turn
- **19 built-in tools**: `view`, `glob`, `rg`, `read_file`, `write_file`, `edit_file`, `list_directory`, `create_directory`, `copy_file`, `move_file`, `search_files`, `get_project_structure`, `create_cpp_class`, `create_blueprint_asset`, `compile`, `live_coding_patch`, `run_automation_tests`, `get_file_info`, `delete_file`
- Automatic file backups before any write operation
- Safety-enforced write roots — the AI can only modify files within approved directories

### 💬 Native Editor Panel
- Dockable Slate tab: **Window → GitHub Copilot**
- Real-time project context (project name, engine version, current map, selected assets/actors)
- VR/Quest/OpenXR readiness summary
- Unified chat window (transcript + prompt composer), with **Enter** to send and **Shift+Enter** for newline
- Built-in thinking indicator (`Copilot is thinking...`) while backend/model work is in flight
- Persistent user handle + running chat transcript (`Handle: prompt` and `Model (returned-id): response`)
- 20+ action buttons (Analyze Selection, Generate C++ Class, Preview Patch, Trigger Compile, etc.)
- Diff preview area for code changes
- Execution log

### 🔧 Editor Integration
- **Compile & Live Coding** — trigger Hot Reload or Live Coding from the AI panel
- **Content Browser awareness** — selected assets are included in context
- **Level Editor awareness** — selected actors are included in context
- **Blueprint creation** — create and auto-save real Blueprint assets (including Blueprint Function Libraries) directly in `/Game/...`
- **Asset Registry** — create folders, refresh state after file changes
- **Automation Tests** — run tests from the panel

### 🥽 Meta Quest / XR Support
- Detects OpenXR, MetaXR, OculusXR plugin status
- Scans for VR-relevant actors (MotionController, XROrigin, HMD)
- Android manifest & Quest readiness audit
- Dedicated "Analyze VR Setup" and "Analyze Quest Readiness" commands

### 🔌 Multi-Model Support
- Works with **any model** available on your GitHub Copilot subscription
- Claude (Opus, Sonnet, Haiku), GPT-4o, GPT-4.1, o1, o3, Gemini, and more
- Dynamic model catalog fetched from the Copilot API at startup
- Switch models at any time with `/model <name>`
- Per-model endpoint routing (chat/completions vs. responses)

### 📟 Console Commands
Use the plugin from the UE console (`~` key) without opening the panel:
```
Copilot <prompt>          Chat with the AI
Copilot /model <name>     Switch active model
Copilot /models           List available models
Copilot /context           Show current project context
Copilot /help              Show all commands
```

---

## Requirements

| Requirement | Details |
|-------------|---------|
| **Unreal Engine** | 5.3 or later (tested on 5.4 and 5.5) |
| **Platform** | Windows (macOS/Linux should compile but are untested) |
| **GitHub Copilot** | Active subscription — Individual, Business, or Enterprise |
| **Compiler** | Visual Studio 2022 or compatible (for editor module) |

> **Note:** This plugin uses the **GitHub Copilot API** with OAuth device-flow authentication. You do **not** need a separate API key or PAT — the plugin handles authentication automatically, the same way VS Code does.

---

## Installation

### Option A: Clone into your project (recommended)

```bash
cd "YourProject/Plugins"
git clone https://github.com/YOUR_USERNAME/GitHubCopilotUE.git
```

### Option B: Download and copy

1. Download the latest release or clone this repo
2. Copy the `GitHubCopilotUE` folder into your project's `Plugins/` directory

```
YourProject/
├── Content/
├── Source/
├── Plugins/
│   └── GitHubCopilotUE/          ← here
│       ├── GitHubCopilotUE.uplugin
│       ├── Resources/
│       └── Source/
│           ├── GitHubCopilotUE/          (Editor module)
│           └── GitHubCopilotUERuntime/   (Runtime module)
└── YourProject.uproject
```

### Option C: Engine-level plugin

Copy to `Engine/Plugins/Marketplace/GitHubCopilotUE/` to make it available across all projects.

For installed engines under `C:\Program Files\...`, use this flow:

1. Close Unreal Editor
2. Open **PowerShell as Administrator**
3. Put the plugin in:
   `C:\Program Files\Epic Games\UE_5.7\Engine\Plugins\Marketplace\GitHubCopilotUE`
4. If source plugin compilation fails due permissions, prebuild/package it:
   - Run:
     `Engine\Build\BatchFiles\RunUAT.bat BuildPlugin -Plugin="<path>\GitHubCopilotUE.uplugin" -Package="<output-folder>" -TargetPlatforms=Win64`
   - Copy the packaged output into `Engine\Plugins\Marketplace\GitHubCopilotUE`

> Why this matters: engine plugins in `Program Files` may fail to write `Binaries/Intermediate` without admin rights.
>
> When updating an existing engine install, close **all** Unreal Editor instances first so DLLs are not locked.

### After Installation

1. Open your project in Unreal Editor
2. The plugin will compile automatically on first launch
3. If prompted, click **Yes** to enable the plugin
4. Go to **Window → GitHub Copilot** to open the panel

### Install Mode Verification (Project vs Engine)

Use this quick check to confirm both install modes behave correctly:

1. **Project plugin install**
   - Place plugin at `YourProject/Plugins/GitHubCopilotUE`
   - Open project, verify plugin appears in **Edit → Plugins**
   - Open **Window → GitHub Copilot** and verify panel opens
2. **Engine-wide install**
   - Place plugin at `Engine/Plugins/Marketplace/GitHubCopilotUE`
   - Open two different projects and verify plugin is available in both
   - In each project, open panel and verify compile/sign-in/model list works
3. Confirm auth cache is project-scoped at `YourProject/Saved/CopilotAuth.json`
   - This is expected for both install modes

---

## Authentication

The plugin uses GitHub's **OAuth Device Flow** — the same method VS Code uses. No tokens to manually create or paste.

### First-Time Sign-In

1. Open the Copilot panel (**Window → GitHub Copilot**)
2. Click **Sign In** (or use `Copilot.Login`, `/login`, or `/signin`)
3. A dialog will show a **device code** and open your browser to `https://github.com/login/device`
4. Enter the code in your browser and authorize
5. The plugin will automatically detect authorization and fetch your Copilot token

### How It Works

```
Plugin                      GitHub
  │                           │
  ├──POST /login/device/code──►  (get device_code + user_code)
  │                           │
  │  ┌── User enters code ────►  (browser at github.com/login/device)
  │  │                        │
  ├──POST /login/oauth/access ►  (poll until authorized)
  │                           │
  ├──GET /user ───────────────►  (get username)
  │                           │
  ├──GET /copilot_internal/v2/token ──►  (exchange for Copilot token)
  │                           │
  ├──GET /models ─────────────►  (fetch available models)
  │                           │
  └── Ready ✓                 │
```

### Token Storage

Tokens are cached locally at:
```
YourProject/Saved/CopilotAuth.json
```
This file contains your GitHub access token, active model, and API endpoint. It is **not** committed to source control (add `Saved/` to `.gitignore`).

### Subscription Detection

The plugin automatically detects your subscription tier (Individual, Business, Enterprise) from the token exchange and routes API calls to the correct endpoint:
- Individual → `api.individual.githubcopilot.com`
- Business → `api.business.githubcopilot.com`  
- Enterprise → `api.enterprise.githubcopilot.com`

### Shared Sign-In Verification (Panel + REPL)

Sign-in state is shared across the dockable panel, `Copilot.*` console commands, and Output Log **Copilot** REPL.

Quick test:

1. Sign in from panel (**Sign In with GitHub**)
2. Run `Copilot.Status` and confirm it shows authenticated user/model
3. Switch Output Log executor to **Copilot**, run `/context` and `/model`
4. Sign out from REPL (`/logout` or `/signout`)
5. Confirm panel updates to signed-out state and model list clears

---

## Usage

### Panel UI

Open via **Window → GitHub Copilot**. The panel has:

| Section | Description |
|---------|-------------|
| **Status Bar** | Connection status, active model, refresh button |
| **Context** | Auto-detected project name, engine version, current map, selected assets/actors |
| **VR/Quest** | OpenXR/MetaXR status, detected VR actors (collapsible) |
| **Chat** | Running transcript plus prompt composer in one area (`Enter` sends, `Shift+Enter` adds newline) |
| **Target Inputs** | Target path + optional line number for patch/insert/open/create operations |
| **Actions** | Button rows for common commands |
| **Diff** | Code diff preview (collapsible) |
| **Log** | Execution status messages |

### Example Prompts

```
Read my GameMode.h and explain what it does

Create a new ActorComponent called HealthComponent with a float Health property and TakeDamage function

Create a Blueprint Function Library asset named BP_GameHelpers in /Game/Blueprints

Search for all uses of UGameplayStatics::ApplyDamage in my project

Refactor my PlayerCharacter.cpp to use the new Enhanced Input system

List all files in my Source directory

What plugins are enabled in my project? Is it ready for Quest deployment?
```

### Slash Commands

| Command | Description |
|---------|-------------|
| `/help` | Show all available commands |
| `/login`, `/signin` | Start GitHub OAuth sign-in |
| `/logout`, `/signout` | Sign out and clear cached tokens |
| `/model <name>` | Switch active model (e.g., `/model gpt-4o`) |
| `/models` | List all available models on your subscription |
| `/context` | Show current project context, API endpoint, active model |
| `/clear` | Clear conversation history |
| `/version` | Show plugin version |

### Action Buttons

The panel includes 20+ action buttons grouped by category:

**Analysis:** Analyze Selection, Explain Code, Analyze VR Setup, Analyze Quest Readiness  
**Generation:** Generate C++ Class, Generate Actor Component, Generate BP Function Library, Generate Editor Utility  
**Code Operations:** Suggest Refactor, Preview Patch, Apply Patch, Insert Into File, Open Related File  
**Build:** Trigger Compile, Trigger Live Coding, Run Automation Tests  
**Utility:** Copy Response, Clear

### Dockable UI Wiring Smoke Test

Recommended quick pass in-editor:

1. **Status row**: Sign In, Sign Out, Refresh Models, Refresh Context
2. **Model picker**: pick a model, restart editor, verify selection persists
3. **Action rows**:
   - Analyze/Explain/Refactor return AI output
   - Generate C++ Class and Generate BP Func Library create assets/files
   - Preview Patch + Apply Patch + Rollback work end-to-end
   - Compile/Live Coding/Test commands return status
4. Prompt slash commands:
   - `/clear` clears panel fields
   - `/copy` copies latest response text

If a selected model becomes unavailable, the plugin auto-falls back to a valid model from your available catalog.

---

## Models

The plugin supports **any model** available through your GitHub Copilot subscription. Available models are fetched dynamically at startup.

### Common Models

| Model | ID | Notes |
|-------|----|-------|
| Claude Opus 4.6 | `claude-opus-4.6` | Most capable, premium tier |
| Claude Sonnet 4.5 | `claude-sonnet-4.5` | Great balance of quality and speed |
| GPT-4o | `gpt-4o` | Fast, strong general-purpose |
| GPT-4.1 | `gpt-4.1` | Latest GPT, strong at coding |
| o3-mini | `o3-mini` | Fast reasoning model |
| Gemini 2.5 Pro | `gemini-2.5-pro` | Google's latest |

> Model availability depends on your subscription tier and organization settings. Use `/models` to see what's available to you.

### Switching Models

From the panel or console:
```
Copilot /model claude-sonnet-4.5
```

The active model persists across editor sessions (saved in `CopilotAuth.json`).

### Model Verification Checklist

1. Run `/model` and confirm your expected model ID exists in the list
2. Select model via combo box or `/model <id>`
3. Send a prompt and verify the transcript shows `Model (<returned-model-id>): ...` (returned by API, not just picker selection)
4. If a stale cached model was removed from your subscription, plugin should auto-fallback to a valid model and continue working

---

## Settings

Go to **Edit → Project Settings → Plugins → GitHub Copilot UE**

### Connection
| Setting | Default | Description |
|---------|---------|-------------|
| Backend Type | `GitHubCopilot` | Backend service type |
| Endpoint URL | (auto-detected) | API endpoint override |
| API Key | (empty) | Manual API key override (not needed for Copilot OAuth) |
| Model Name | `claude-sonnet-4.5` | Default model |
| Timeout (Seconds) | `120` | HTTP request timeout |

### Chat
| Setting | Default | Description |
|---------|---------|-------------|
| User Handle | (empty) | Prefix used for user chat turns in the transcript (required before sending prompts) |

### Logging
| Setting | Default | Description |
|---------|---------|-------------|
| Enable Verbose Logging | `false` | Show detailed debug logs in Output Log |

### XR / Quest
| Setting | Default | Description |
|---------|---------|-------------|
| Enable Quest Workflow Analysis | `true` | Include Quest readiness in context |
| Enable OpenXR Context Collection | `true` | Detect OpenXR/MetaXR plugins |

### Execution
| Setting | Default | Description |
|---------|---------|-------------|
| Require Patch Preview | `true` | Show diff preview before applying changes |
| Max Tool-Call Iterations (0 = Unlimited) | `0` | Iteration cap for tool-call loops; keep `0` for unrestricted deep analysis |

### Safety
| Setting | Default | Description |
|---------|---------|-------------|
| Allowed Write Roots | `["Source", "Config", "Plugins"]` | Directories the AI is allowed to write to (relative to project root) |

### Compile
| Setting | Default | Description |
|---------|---------|-------------|
| Enable Compile Commands | `true` | Allow AI to trigger compiles |
| Enable Live Coding Commands | `true` | Allow AI to trigger Live Coding |

### Context
| Setting | Default | Description |
|---------|---------|-------------|
| Enable Blueprint Context Collection | `true` | Include Blueprint assets in context |
| Default Target Platform | `Android` | Default platform for context/analysis |

---

## Architecture

### Module Layout

```
GitHubCopilotUE/
├── GitHubCopilotUE.uplugin
└── Source/
    ├── GitHubCopilotUE/                    (Editor module – LoadingPhase: PostEngineInit)
    │   ├── GitHubCopilotUE.Build.cs
    │   ├── Public/
    │   │   ├── GitHubCopilotUEModule.h
    │   │   ├── GitHubCopilotUESettings.h
    │   │   ├── GitHubCopilotUECommands.h
    │   │   ├── GitHubCopilotUEStyle.h
    │   │   ├── Services/
    │   │   │   ├── GitHubCopilotUEBridgeService.h
    │   │   │   ├── GitHubCopilotUECommandRouter.h
    │   │   │   ├── GitHubCopilotUECompileService.h
    │   │   │   ├── GitHubCopilotUEConsoleCommands.h
    │   │   │   ├── GitHubCopilotUEConsoleExecutor.h
    │   │   │   ├── GitHubCopilotUEContextService.h
    │   │   │   ├── GitHubCopilotUEFileService.h
    │   │   │   ├── GitHubCopilotUEPatchService.h
    │   │   │   ├── GitHubCopilotUEQuestService.h
    │   │   │   ├── GitHubCopilotUESlashCommands.h
    │   │   │   ├── GitHubCopilotUEToolExecutor.h
    │   │   │   └── GitHubCopilotUETypes.h
    │   │   └── Widgets/
    │   │       └── SGitHubCopilotUEPanel.h
    │   └── Private/
    │       ├── GitHubCopilotUEModule.cpp
    │       ├── GitHubCopilotUESettings.cpp
    │       ├── GitHubCopilotUECommands.cpp
    │       ├── GitHubCopilotUEStyle.cpp
    │       ├── Services/
    │       │   ├── GitHubCopilotUEBridgeService.cpp    (OAuth, token exchange, chat completions, agentic loop)
    │       │   ├── GitHubCopilotUECommandRouter.cpp
    │       │   ├── GitHubCopilotUECompileService.cpp
    │       │   ├── GitHubCopilotUEConsoleCommands.cpp
    │       │   ├── GitHubCopilotUEConsoleExecutor.cpp
    │       │   ├── GitHubCopilotUEContextService.cpp
    │       │   ├── GitHubCopilotUEFileService.cpp
    │       │   ├── GitHubCopilotUEPatchService.cpp
    │       │   ├── GitHubCopilotUEQuestService.cpp
    │       │   ├── GitHubCopilotUESlashCommands.cpp
    │       │   ├── GitHubCopilotUEToolExecutor.cpp
    │       │   └── GitHubCopilotUETypes.cpp
    │       └── Widgets/
    │           └── SGitHubCopilotUEPanel.cpp
    └── GitHubCopilotUERuntime/             (Runtime module – shared types)
        ├── GitHubCopilotUERuntime.Build.cs
        ├── Public/
        │   └── GitHubCopilotUERuntimeModule.h
        └── Private/
            └── GitHubCopilotUERuntimeModule.cpp
```

### Service Architecture

| Service | Responsibility |
|---------|---------------|
| **BridgeService** | OAuth device flow, Copilot token management, HTTP requests, chat completions, agentic tool-call loop, model catalog |
| **ToolExecutor** | Executes the built-in tool surface (`view/glob/rg`, file ops, class/Blueprint creation, compile/test tools) and returns results to the AI |
| **CommandRouter** | Central dispatcher — validates requests, routes to local handlers or backend |
| **ContextService** | Gathers project/editor/asset/actor/plugin/platform context for AI prompts |
| **FileService** | Safe file read/write/create with backup, path validation, and allowed-root enforcement |
| **PatchService** | Diff generation, preview, validation, apply-with-rollback, line insertion |
| **CompileService** | Hot Reload, Live Coding, automation test triggers |
| **QuestService** | OpenXR/MetaXR detection, VR actor scanning, Quest readiness audit |
| **ConsoleCommands** | `Copilot` console command registration and response display |
| **SlashCommands** | `/model`, `/models`, `/context`, `/login`/`/signin`, `/logout`/`/signout`, `/help`, `/clear`, `/version` |

### Agentic Tool-Call Flow

```
User: "Read my GameMode.h and add a Score variable"
  │
  ├─► BridgeService sends prompt + tool definitions to Copilot API
  │
  ◄── API returns tool_calls: [read_file("GameMode.h")]
  │
  ├─► ToolExecutor reads file, returns contents
  │
  ├─► BridgeService sends tool result back to API
  │
  ◄── API returns tool_calls: [edit_file("GameMode.h", ...)]
  │
  ├─► ToolExecutor edits file (with backup)
  │
  ├─► BridgeService sends tool result back to API
  │
  ◄── API returns final response: "Done! I added an int32 Score property..."
  │
  └─► Displayed in panel / console
```

The loop continues until the AI returns a text response with no further tool calls. A configurable cap exists in settings (`Max Tool-Call Iterations`), and the default is `0` (unlimited).

---

## Troubleshooting

### "module 'GitHubCopilotUE' could not be found"
- This means UE found the plugin descriptor but could not load/build the editor DLL.
- Verify placement:
  - Project install: `<Project>/Plugins/GitHubCopilotUE/GitHubCopilotUE.uplugin`
  - Engine install: `<Engine>/Plugins/Marketplace/GitHubCopilotUE/GitHubCopilotUE.uplugin`
- For source installs, ensure `.uplugin` has `"Installed": false` so UE treats it as buildable source plugin.
- Close UE, delete plugin `Binaries/` + `Intermediate/`, reopen project, and allow module rebuild.
- If installed under `Program Files`, prebuild/package with:
  - `RunUAT BuildPlugin -Plugin="<path>\GitHubCopilotUE.uplugin" -Package="<out>" -TargetPlatforms=Win64`
  - then copy packaged output to `Engine\Plugins\Marketplace\GitHubCopilotUE`
- Confirm Visual Studio 2022 + C++ toolchain is installed and the project can build editor targets.

### "Sign in required" / not connecting
- Make sure you have an active GitHub Copilot subscription
- Try `/logout` then `/login` (or `/signout` then `/signin`) to re-authenticate
- Check Output Log (filter by `LogGitHubCopilotUE`) for detailed errors

### Model returns errors
- Use `/models` to confirm the model is available on your subscription
- Some models (e.g., Opus) may require a Business/Enterprise tier
- Try switching to a different model: `/model gpt-4o`

### Tool calls not working
- Check that `Allowed Write Roots` in Project Settings includes the directories you expect
- Ensure `Enable Compile Commands` is checked if you want the AI to compile
- Check Output Log with verbose logging enabled for detailed tool execution traces

### Plugin doesn't appear
- Ensure the folder structure is correct (`.uplugin` must be at the root of the plugin folder)
- Check **Edit → Plugins** and search for "GitHub Copilot" — enable it if disabled
- Delete `Binaries/` and `Intermediate/` inside the plugin folder and restart the editor to force recompile
- If installed under `Program Files`, run Unreal as Administrator once or package/prebuild the plugin with `RunUAT BuildPlugin`
- Put engine-level installs under `Engine\Plugins\Marketplace\GitHubCopilotUE` (not directly in `Engine\Plugins` root)

---

## Contributing

Contributions are welcome! This is an open-source project and we'd love your help.

1. Fork the repository
2. Create a feature branch (`git checkout -b feature/my-feature`)
3. Make your changes
4. Test in Unreal Editor
5. Submit a Pull Request

### Areas We'd Love Help With
- **Streaming support** — SSE streaming for real-time response display
- **macOS / Linux testing** — the plugin should compile cross-platform but is untested
- **Blueprint integration** — deeper Blueprint asset inspection and manipulation
- **Additional tools** — more agentic tools (rename symbol, refactor, etc.)
- **UI polish** — Slate styling, icons, better diff rendering
- **Context window management** — smart truncation of conversation history

---

## License

This project is open source. See [LICENSE](LICENSE) for details.

---

<p align="center">
  Built with ❤️ for the Unreal Engine community
</p>
