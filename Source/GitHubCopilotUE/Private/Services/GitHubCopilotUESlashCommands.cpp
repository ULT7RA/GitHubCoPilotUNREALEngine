// Copyright GitHub, Inc. All Rights Reserved.

#include "Services/GitHubCopilotUESlashCommands.h"
#include "Services/GitHubCopilotUECommandRouter.h"
#include "Services/GitHubCopilotUEBridgeService.h"
#include "Services/GitHubCopilotUEContextService.h"
#include "Services/GitHubCopilotUEFileService.h"
#include "GitHubCopilotUESettings.h"
#include "HAL/PlatformApplicationMisc.h"
#include "Misc/App.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

FGitHubCopilotUESlashCommands::FGitHubCopilotUESlashCommands()
{
	RegisterCommands();
}

void FGitHubCopilotUESlashCommands::Initialize(
	TSharedPtr<FGitHubCopilotUECommandRouter> InCommandRouter,
	TSharedPtr<FGitHubCopilotUEBridgeService> InBridgeService,
	TSharedPtr<FGitHubCopilotUEContextService> InContextService,
	TSharedPtr<FGitHubCopilotUEFileService> InFileService)
{
	CommandRouter = InCommandRouter;
	BridgeService = InBridgeService;
	ContextService = InContextService;
	FileService = InFileService;
	Log(TEXT("SlashCommands: Initialized"));
}

void FGitHubCopilotUESlashCommands::RegisterCommands()
{
	// ===== Auth & Account =====
	Commands.Add({TEXT("login"), {}, TEXT("/login"), TEXT("Log in to GitHub Copilot"), false, true});
	Commands.Add({TEXT("logout"), {}, TEXT("/logout"), TEXT("Log out of GitHub Copilot"), false, true});

	// ===== Model Selection =====
	Commands.Add({TEXT("model"), {TEXT("models")}, TEXT("/model [model-name]"), TEXT("Select AI model to use or list available models"), false, true});

	// ===== Context & Info =====
	Commands.Add({TEXT("context"), {}, TEXT("/context"), TEXT("Show project context, selected assets, and active state"), false, true});
	Commands.Add({TEXT("help"), {}, TEXT("/help"), TEXT("Show all available slash commands"), false, true});
	Commands.Add({TEXT("list-dirs"), {}, TEXT("/list-dirs"), TEXT("Display all allowed directories for file access"), false, true});
	Commands.Add({TEXT("add-dir"), {}, TEXT("/add-dir <directory>"), TEXT("Add a directory to the allowed list for file access"), false, true});
	Commands.Add({TEXT("init"), {}, TEXT("/init"), TEXT("Initialize Copilot instructions for this project"), false, true});
	Commands.Add({TEXT("instructions"), {}, TEXT("/instructions"), TEXT("View and toggle custom instruction files"), false, true});

	// ===== Session =====
	Commands.Add({TEXT("clear"), {TEXT("new")}, TEXT("/clear"), TEXT("Clear the conversation history"), false, true});
	Commands.Add({TEXT("copy"), {}, TEXT("/copy"), TEXT("Copy the last response to the clipboard"), false, true});
	Commands.Add({TEXT("compact"), {}, TEXT("/compact"), TEXT("Summarize conversation to reduce context"), true, false});
	Commands.Add({TEXT("session"), {}, TEXT("/session [info|files|plan]"), TEXT("Show session info and workspace summary"), false, true});

	// ===== Code Intelligence (sent to Copilot) =====
	Commands.Add({TEXT("explain"), {}, TEXT("/explain [code or selection]"), TEXT("Explain the selected code or file"), true, false});
	Commands.Add({TEXT("refactor"), {TEXT("suggest-refactor")}, TEXT("/refactor [prompt]"), TEXT("Suggest refactoring for selected code"), true, false});
	Commands.Add({TEXT("review"), {}, TEXT("/review [prompt]"), TEXT("Run code review agent to analyze changes"), true, false});
	Commands.Add({TEXT("plan"), {}, TEXT("/plan [prompt]"), TEXT("Create an implementation plan before coding"), true, false});
	Commands.Add({TEXT("research"), {}, TEXT("/research <topic>"), TEXT("Run deep research investigation using GitHub search and web sources"), true, false});
	Commands.Add({TEXT("diff"), {}, TEXT("/diff"), TEXT("Review the changes made in the current directory"), true, false});
	Commands.Add({TEXT("delegate"), {}, TEXT("/delegate [prompt]"), TEXT("Send this session to Copilot to create a PR"), true, false});
	Commands.Add({TEXT("fleet"), {}, TEXT("/fleet [prompt]"), TEXT("Enable fleet mode for parallel subagent execution"), true, false});
	Commands.Add({TEXT("agent"), {}, TEXT("/agent"), TEXT("Browse and select from available agents"), true, false});

	// ===== Code Generation =====
	Commands.Add({TEXT("generate"), {TEXT("gen")}, TEXT("/generate <class|component|blueprint|utility> [name]"), TEXT("Generate C++ class, component, Blueprint library, or editor utility"), false, true});
	Commands.Add({TEXT("blueprint"), {TEXT("bp")}, TEXT("/blueprint [name]"), TEXT("Create a Blueprint Function Library"), false, true});

	// ===== File Operations =====
	Commands.Add({TEXT("open"), {}, TEXT("/open <file-or-asset-path>"), TEXT("Open a source file or asset in the editor"), false, true});
	Commands.Add({TEXT("patch"), {}, TEXT("/patch <file-path>"), TEXT("Preview a patch for a file (provide proposed content in prompt)"), false, true});
	Commands.Add({TEXT("rollback"), {TEXT("undo")}, TEXT("/rollback [file-path]"), TEXT("Restore the last patched file from its backup"), false, true});

	// ===== Build & Test =====
	Commands.Add({TEXT("compile"), {TEXT("build")}, TEXT("/compile"), TEXT("Trigger project recompile"), false, true});
	Commands.Add({TEXT("live-coding"), {TEXT("lc")}, TEXT("/live-coding"), TEXT("Trigger Live Coding patch"), false, true});
	Commands.Add({TEXT("test"), {TEXT("tests")}, TEXT("/test [filter]"), TEXT("Run automation tests"), false, true});

	// ===== VR / Quest =====
	Commands.Add({TEXT("quest"), {TEXT("quest-audit")}, TEXT("/quest"), TEXT("Analyze Meta Quest readiness"), false, true});
	Commands.Add({TEXT("vr"), {TEXT("xr")}, TEXT("/vr"), TEXT("Analyze VR/XR setup and plugin state"), false, true});

	// ===== PR & Git =====
	Commands.Add({TEXT("pr"), {}, TEXT("/pr [view|create|fix|auto]"), TEXT("Operate on pull requests for the current branch"), true, false});
	Commands.Add({TEXT("changelog"), {}, TEXT("/changelog [summarize]"), TEXT("Display changelog or summarize recent changes"), true, false});
	Commands.Add({TEXT("chronicle"), {}, TEXT("/chronicle <standup|tips|improve>"), TEXT("Session history tools and insights"), true, false});

	// ===== Share & Export =====
	Commands.Add({TEXT("share"), {}, TEXT("/share [file|gist] [path]"), TEXT("Share session or research report to file or GitHub gist"), true, false});
	Commands.Add({TEXT("feedback"), {}, TEXT("/feedback"), TEXT("Provide feedback about the plugin"), false, true});

	// ===== Skills & Extensions =====
	Commands.Add({TEXT("skills"), {}, TEXT("/skills [list|info|add|remove]"), TEXT("Manage skills for enhanced capabilities"), true, false});
	Commands.Add({TEXT("extensions"), {TEXT("extension")}, TEXT("/extensions"), TEXT("Manage Copilot extensions"), true, false});
	Commands.Add({TEXT("experimental"), {}, TEXT("/experimental [on|off|show]"), TEXT("Show or toggle experimental features"), false, true});

	// ===== Knowledge =====
	Commands.Add({TEXT("knowledge"), {TEXT("know")}, TEXT("/knowledge"), TEXT("Explore project and generate project.md with a full synopsis"), true, false});

	// ===== Misc =====
	Commands.Add({TEXT("allow-all"), {TEXT("yolo")}, TEXT("/allow-all"), TEXT("Enable all permissions (tools, paths, and URLs)"), false, true});
	Commands.Add({TEXT("reset-allowed-tools"), {}, TEXT("/reset-allowed-tools"), TEXT("Reset the list of allowed tools"), false, true});
}

bool FGitHubCopilotUESlashCommands::IsSlashCommand(const FString& Input) const
{
	FString Trimmed = Input.TrimStartAndEnd();
	return Trimmed.StartsWith(TEXT("/"));
}

TArray<FCopilotSlashCommand> FGitHubCopilotUESlashCommands::GetMatchingCommands(const FString& Partial) const
{
	TArray<FCopilotSlashCommand> Matches;
	FString Search = Partial.ToLower();
	if (Search.StartsWith(TEXT("/")))
	{
		Search.RemoveAt(0);
	}

	for (const FCopilotSlashCommand& Cmd : Commands)
	{
		if (Cmd.Name.StartsWith(Search))
		{
			Matches.Add(Cmd);
			continue;
		}
		for (const FString& Alias : Cmd.Aliases)
		{
			if (Alias.StartsWith(Search))
			{
				Matches.Add(Cmd);
				break;
			}
		}
	}
	return Matches;
}

FString FGitHubCopilotUESlashCommands::GetHelpText() const
{
	FString Help = TEXT("=== GitHub Copilot UE — Slash Commands ===\n\n");

	Help += TEXT("--- Auth & Account ---\n");
	Help += TEXT("  /login                               Log in to GitHub Copilot\n");
	Help += TEXT("  /logout                              Log out of GitHub Copilot\n\n");

	Help += TEXT("--- Model ---\n");
	Help += TEXT("  /model, /models [model-name]         Select AI model or list available\n\n");

	Help += TEXT("--- Context & Info ---\n");
	Help += TEXT("  /context                             Show project context and state\n");
	Help += TEXT("  /help                                Show this help\n");
	Help += TEXT("  /list-dirs                           Display allowed write directories\n");
	Help += TEXT("  /add-dir <directory>                 Add directory to allowed list\n");
	Help += TEXT("  /init                                Initialize Copilot for this project\n");
	Help += TEXT("  /instructions                        View custom instruction files\n\n");

	Help += TEXT("--- Session ---\n");
	Help += TEXT("  /clear, /new                         Clear conversation history\n");
	Help += TEXT("  /copy                                Copy last response to clipboard\n");
	Help += TEXT("  /compact                             Summarize conversation context\n");
	Help += TEXT("  /session [info|files|plan]            Show session info\n\n");

	Help += TEXT("--- Code Intelligence ---\n");
	Help += TEXT("  /explain [code]                      Explain selected code\n");
	Help += TEXT("  /refactor [prompt]                   Suggest refactoring\n");
	Help += TEXT("  /review [prompt]                     Run code review agent\n");
	Help += TEXT("  /plan [prompt]                       Create implementation plan\n");
	Help += TEXT("  /research <topic>                    Deep research with web sources\n");
	Help += TEXT("  /diff                                Review changes in project\n");
	Help += TEXT("  /delegate [prompt]                   Send to Copilot to create PR\n");
	Help += TEXT("  /fleet [prompt]                      Fleet mode (parallel agents)\n");
	Help += TEXT("  /agent                               Browse available agents\n\n");

	Help += TEXT("--- Code Generation ---\n");
	Help += TEXT("  /generate <class|component|bp> [n]   Generate C++ class or Blueprint\n");
	Help += TEXT("  /blueprint [name]                    Create Blueprint Function Library\n\n");

	Help += TEXT("--- File Operations ---\n");
	Help += TEXT("  /open <path>                         Open file or asset in editor\n");
	Help += TEXT("  /patch <file>                        Preview patch for a file\n");
	Help += TEXT("  /rollback [file]                     Restore file from backup\n\n");

	Help += TEXT("--- Build & Test ---\n");
	Help += TEXT("  /compile, /build                     Trigger recompile\n");
	Help += TEXT("  /live-coding, /lc                    Trigger Live Coding\n");
	Help += TEXT("  /test [filter]                       Run automation tests\n\n");

	Help += TEXT("--- VR / Quest ---\n");
	Help += TEXT("  /quest                               Analyze Meta Quest readiness\n");
	Help += TEXT("  /vr, /xr                             Analyze VR/XR setup\n\n");

	Help += TEXT("--- PR & Git ---\n");
	Help += TEXT("  /pr [view|create|fix|auto]           Operate on pull requests\n");
	Help += TEXT("  /changelog [summarize]               Display/summarize changelog\n");
	Help += TEXT("  /chronicle <standup|tips|improve>    Session history tools\n\n");

	Help += TEXT("--- Share & Export ---\n");
	Help += TEXT("  /share [file|gist] [path]            Share session report\n");
	Help += TEXT("  /feedback                            Provide feedback\n\n");

	Help += TEXT("--- Skills & Extensions ---\n");
	Help += TEXT("  /skills [list|add|remove]            Manage skills\n");
	Help += TEXT("  /extensions                          Manage extensions\n");
	Help += TEXT("  /experimental [on|off|show]          Toggle experimental features\n\n");

	Help += TEXT("--- Knowledge ---\n");
	Help += TEXT("  /knowledge, /know                    Explore project & write project.md\n\n");

	Help += TEXT("--- Misc ---\n");
	Help += TEXT("  /allow-all, /yolo                    Enable all permissions\n");
	Help += TEXT("  /reset-allowed-tools                 Reset allowed tools\n");

	return Help;
}

bool FGitHubCopilotUESlashCommands::ExecuteSlashCommand(const FString& Input, FString& OutResponse)
{
	FString Trimmed = Input.TrimStartAndEnd();
	if (!Trimmed.StartsWith(TEXT("/")))
	{
		return false;
	}

	// Parse command and args
	Trimmed.RemoveAt(0); // Remove leading /
	FString Command, Args;
	if (!Trimmed.Split(TEXT(" "), &Command, &Args))
	{
		Command = Trimmed;
		Args = TEXT("");
	}
	Command = Command.ToLower().TrimStartAndEnd();
	Args = Args.TrimStartAndEnd();

	// Match command or alias
	FString MatchedName;
	for (const FCopilotSlashCommand& Cmd : Commands)
	{
		if (Cmd.Name == Command)
		{
			MatchedName = Cmd.Name;
			break;
		}
		for (const FString& Alias : Cmd.Aliases)
		{
			if (Alias == Command)
			{
				MatchedName = Cmd.Name;
				break;
			}
		}
		if (!MatchedName.IsEmpty()) break;
	}

	if (MatchedName.IsEmpty())
	{
		OutResponse = FString::Printf(TEXT("Unknown command: /%s\nType /help to see available commands."), *Command);
		return true;
	}

	Log(FString::Printf(TEXT("SlashCommands: Executing /%s %s"), *MatchedName, *Args));

	// Route to handler
	if (MatchedName == TEXT("help")) OutResponse = HandleHelp(Args);
	else if (MatchedName == TEXT("clear")) OutResponse = HandleClear(Args);
	else if (MatchedName == TEXT("copy")) OutResponse = HandleCopy(Args);
	else if (MatchedName == TEXT("context")) OutResponse = HandleContext(Args);
	else if (MatchedName == TEXT("model")) OutResponse = HandleModel(Args);
	else if (MatchedName == TEXT("login")) OutResponse = HandleLogin(Args);
	else if (MatchedName == TEXT("logout")) OutResponse = HandleLogout(Args);
	else if (MatchedName == TEXT("list-dirs")) OutResponse = HandleListDirs(Args);
	else if (MatchedName == TEXT("add-dir")) OutResponse = HandleAddDir(Args);
	else if (MatchedName == TEXT("plan")) OutResponse = HandlePlan(Args);
	else if (MatchedName == TEXT("review")) OutResponse = HandleReview(Args);
	else if (MatchedName == TEXT("diff")) OutResponse = HandleDiff(Args);
	else if (MatchedName == TEXT("research")) OutResponse = HandleResearch(Args);
	else if (MatchedName == TEXT("explain")) OutResponse = HandleExplain(Args);
	else if (MatchedName == TEXT("refactor")) OutResponse = HandleRefactor(Args);
	else if (MatchedName == TEXT("generate")) OutResponse = HandleGenerate(Args);
	else if (MatchedName == TEXT("compile")) OutResponse = HandleCompile(Args);
	else if (MatchedName == TEXT("live-coding")) OutResponse = HandleLiveCoding(Args);
	else if (MatchedName == TEXT("test")) OutResponse = HandleTest(Args);
	else if (MatchedName == TEXT("init")) OutResponse = HandleInit(Args);
	else if (MatchedName == TEXT("session")) OutResponse = HandleSession(Args);
	else if (MatchedName == TEXT("compact")) OutResponse = HandleCompact(Args);
	else if (MatchedName == TEXT("pr")) OutResponse = HandlePr(Args);
	else if (MatchedName == TEXT("share")) OutResponse = HandleShare(Args);
	else if (MatchedName == TEXT("fleet")) OutResponse = HandleFleet(Args);
	else if (MatchedName == TEXT("agent")) OutResponse = HandleAgent(Args);
	else if (MatchedName == TEXT("skills")) OutResponse = HandleSkills(Args);
	else if (MatchedName == TEXT("quest")) OutResponse = HandleQuest(Args);
	else if (MatchedName == TEXT("vr")) OutResponse = HandleVR(Args);
	else if (MatchedName == TEXT("open")) OutResponse = HandleOpen(Args);
	else if (MatchedName == TEXT("patch")) OutResponse = HandlePatch(Args);
	else if (MatchedName == TEXT("rollback")) OutResponse = HandleRollback(Args);
	else if (MatchedName == TEXT("blueprint")) OutResponse = HandleBlueprint(Args);
	else if (MatchedName == TEXT("knowledge")) OutResponse = HandleKnowledge(Args);
	else if (MatchedName == TEXT("changelog")){ OnSendPrompt.ExecuteIfBound(ECopilotCommandType::AnalyzeProject, TEXT("Summarize the recent changelog and changes for this project. ") + Args); OutResponse = TEXT("Requesting changelog analysis..."); }
	else if (MatchedName == TEXT("chronicle")) { OnSendPrompt.ExecuteIfBound(ECopilotCommandType::AnalyzeProject, TEXT("Session chronicle: ") + Args); OutResponse = TEXT("Requesting chronicle..."); }
	else if (MatchedName == TEXT("feedback")) OutResponse = TEXT("Feedback: Visit https://github.com/features/copilot to provide feedback on GitHub Copilot.");
	else if (MatchedName == TEXT("extensions")) OutResponse = TEXT("Extensions are managed through your GitHub Copilot subscription settings at https://github.com/settings/copilot");
	else if (MatchedName == TEXT("experimental")) OutResponse = TEXT("Experimental features are managed in Project Settings -> Plugins -> GitHub Copilot UE.");
	else if (MatchedName == TEXT("allow-all")) OutResponse = HandleAddDir(TEXT("*"));
	else if (MatchedName == TEXT("reset-allowed-tools")) OutResponse = TEXT("Allowed tools have been reset. Configure in Project Settings -> Plugins -> GitHub Copilot UE.");
	else if (MatchedName == TEXT("instructions")) OutResponse = HandleInit(TEXT("show"));
	else if (MatchedName == TEXT("delegate")) { OnSendPrompt.ExecuteIfBound(ECopilotCommandType::AnalyzeProject, TEXT("Create a PR with the following changes: ") + Args); OutResponse = TEXT("Delegating to Copilot for PR creation..."); }
	else
	{
		OutResponse = FString::Printf(TEXT("Command /%s recognized but not yet implemented."), *MatchedName);
	}

	return true;
}

// ============================================================================
// Command Handlers
// ============================================================================

FString FGitHubCopilotUESlashCommands::HandleHelp(const FString& Args)
{
	if (Args.IsEmpty())
	{
		return GetHelpText();
	}
	// Help for a specific command
	for (const FCopilotSlashCommand& Cmd : Commands)
	{
		if (Cmd.Name == Args.ToLower() || Cmd.Aliases.Contains(Args.ToLower()))
		{
			return FString::Printf(TEXT("/%s\nUsage: %s\n%s"), *Cmd.Name, *Cmd.Usage, *Cmd.Description);
		}
	}
	return FString::Printf(TEXT("No help for: %s"), *Args);
}

FString FGitHubCopilotUESlashCommands::HandleClear(const FString& Args)
{
	// Signal the panel to clear via delegate
	OnSendPrompt.ExecuteIfBound(ECopilotCommandType::Clear, TEXT(""));
	return TEXT("Conversation cleared.");
}

FString FGitHubCopilotUESlashCommands::HandleCopy(const FString& Args)
{
	OnSendPrompt.ExecuteIfBound(ECopilotCommandType::CopyResponse, TEXT(""));
	return TEXT("Response copied to clipboard.");
}

FString FGitHubCopilotUESlashCommands::HandleContext(const FString& Args)
{
	if (!ContextService.IsValid())
	{
		return TEXT("Context service not available.");
	}

	FCopilotProjectContext Ctx = ContextService->GatherProjectContext();

	FString Result;
	Result += TEXT("=== Project Context ===\n");
	Result += FString::Printf(TEXT("Project: %s\n"), *Ctx.ProjectName);
	Result += FString::Printf(TEXT("Engine: %s\n"), *Ctx.EngineVersion);
	Result += FString::Printf(TEXT("Map: %s\n"), *Ctx.CurrentMapName);
	Result += FString::Printf(TEXT("Platform: %s\n"), *Ctx.ActivePlatform);
	Result += FString::Printf(TEXT("\nSelected Assets (%d):\n"), Ctx.SelectedAssets.Num());
	for (const FString& A : Ctx.SelectedAssets) Result += FString::Printf(TEXT("  %s\n"), *A);
	Result += FString::Printf(TEXT("\nSelected Actors (%d):\n"), Ctx.SelectedActors.Num());
	for (const FString& A : Ctx.SelectedActors) Result += FString::Printf(TEXT("  %s\n"), *A);
	Result += FString::Printf(TEXT("\nEnabled Plugins: %d\n"), Ctx.EnabledPlugins.Num());
	Result += FString::Printf(TEXT("XR Plugins: %d\n"), Ctx.EnabledXRPlugins.Num());
	for (const FString& P : Ctx.EnabledXRPlugins) Result += FString::Printf(TEXT("  %s\n"), *P);
	Result += FString::Printf(TEXT("\nModules (%d):\n"), Ctx.ModuleNames.Num());
	for (const FString& M : Ctx.ModuleNames) Result += FString::Printf(TEXT("  %s\n"), *M);
	Result += FString::Printf(TEXT("\nSource Directories (%d):\n"), Ctx.ProjectSourceDirectories.Num());
	for (const FString& D : Ctx.ProjectSourceDirectories) Result += FString::Printf(TEXT("  %s\n"), *D);

	if (BridgeService.IsValid() && BridgeService->IsAuthenticated())
	{
		Result += FString::Printf(TEXT("\n=== Copilot ===\nUser: %s\nPlan: %s\nModel: %s\nAPI: %s\nModels available: %d\n"),
			*BridgeService->GetUsername(),
			*BridgeService->GetSubscriptionSku(),
			*BridgeService->GetActiveModel(),
			*BridgeService->GetAPIBase(),
			BridgeService->GetAvailableModels().Num());

		const FCopilotModel* ActiveInfo = BridgeService->GetActiveModelInfo();
		if (ActiveInfo)
		{
			Result += FString::Printf(TEXT("  Vendor: %s\n  Context: %dk tokens\n  Tools: %s\n  Cost: %.1fx\n"),
				*ActiveInfo->Vendor,
				ActiveInfo->MaxContextWindowTokens / 1000,
				ActiveInfo->bSupportsToolCalls ? TEXT("yes") : TEXT("no"),
				ActiveInfo->PremiumMultiplier);
		}
	}

	return Result;
}

FString FGitHubCopilotUESlashCommands::HandleModel(const FString& Args)
{
	if (!BridgeService.IsValid())
	{
		return TEXT("Bridge service not available.");
	}

	if (!BridgeService->IsAuthenticated())
	{
		return TEXT("Not signed in. Use /login first.");
	}

	if (Args.IsEmpty())
	{
		// List models
		const TArray<FCopilotModel>& Models = BridgeService->GetAvailableModels();
		if (Models.Num() == 0)
		{
			BridgeService->FetchAvailableModels();
			return TEXT("Fetching models from your Copilot subscription...");
		}

		FString Result = TEXT("=== Available Models ===\n");
		FString Active = BridgeService->GetActiveModel();
		for (const FCopilotModel& M : Models)
		{
			FString Marker = (M.Id == Active) ? TEXT(" ★") : TEXT("");
			FString Cost;
			if (M.PremiumMultiplier <= 0.0f)
				Cost = TEXT("free");
			else
				Cost = FString::Printf(TEXT("%.1fx"), M.PremiumMultiplier);

			FString Tools = M.bSupportsToolCalls ? TEXT("tools") : TEXT("");
			FString Vision = M.bSupportsVision ? TEXT("vision") : TEXT("");
			FString Caps;
			if (!Tools.IsEmpty()) Caps += Tools;
			if (!Vision.IsEmpty()) { if (!Caps.IsEmpty()) Caps += TEXT(","); Caps += Vision; }

			FString Ctx = M.MaxContextWindowTokens > 0
				? FString::Printf(TEXT("%dk"), M.MaxContextWindowTokens / 1000) : TEXT("?");

			Result += FString::Printf(TEXT("  %s%s | %s | %s | ctx:%s | %s\n"),
				*M.Id, *Marker, *M.Vendor, *Cost, *Ctx,
				Caps.IsEmpty() ? TEXT("chat") : *Caps);
		}
		Result += FString::Printf(TEXT("\nActive: %s | Plan: %s\nUse /model <id> to switch."),
			*Active, *BridgeService->GetSubscriptionSku());
		return Result;
	}

	// Set model
	BridgeService->SetActiveModel(Args);
	// Persist the choice so it survives restarts
	BridgeService->SaveTokenCache();
	return FString::Printf(TEXT("Model set to: %s"), *BridgeService->GetActiveModel());
}

FString FGitHubCopilotUESlashCommands::HandleLogin(const FString& Args)
{
	if (!BridgeService.IsValid())
	{
		return TEXT("Bridge service not available.");
	}
	if (BridgeService->IsAuthenticated())
	{
		return FString::Printf(TEXT("Already signed in as %s. Use /logout to switch accounts."), *BridgeService->GetUsername());
	}
	BridgeService->StartDeviceCodeAuth();
	return TEXT("Starting GitHub authentication... A browser window will open.");
}

FString FGitHubCopilotUESlashCommands::HandleLogout(const FString& Args)
{
	if (!BridgeService.IsValid())
	{
		return TEXT("Bridge service not available.");
	}
	BridgeService->SignOut();
	return TEXT("Signed out of GitHub Copilot.");
}

FString FGitHubCopilotUESlashCommands::HandleListDirs(const FString& Args)
{
	const UGitHubCopilotUESettings* Settings = UGitHubCopilotUESettings::Get();
	if (!Settings)
	{
		return TEXT("Settings not available.");
	}

	FString Result = TEXT("=== Allowed Write Directories ===\n");
	if (Settings->AllowedWriteRoots.Num() == 0)
	{
		Result += TEXT("  (none configured — defaults to project Source/ directory)\n");
	}
	else
	{
		for (const FString& Dir : Settings->AllowedWriteRoots)
		{
			Result += FString::Printf(TEXT("  %s\n"), *Dir);
		}
	}
	Result += TEXT("\nConfigure in: Edit -> Project Settings -> Plugins -> GitHub Copilot UE -> Allowed Write Roots");
	return Result;
}

FString FGitHubCopilotUESlashCommands::HandleAddDir(const FString& Args)
{
	if (Args.IsEmpty())
	{
		return TEXT("Usage: /add-dir <directory path>");
	}
	return FString::Printf(TEXT("To add '%s' as an allowed directory, go to:\nEdit -> Project Settings -> Plugins -> GitHub Copilot UE -> Allowed Write Roots\nand add the path there."), *Args);
}

FString FGitHubCopilotUESlashCommands::HandlePlan(const FString& Args)
{
	if (Args.IsEmpty())
	{
		return TEXT("Usage: /plan <describe what you want to build>\nCopilot will create an implementation plan.");
	}
	OnSendPrompt.ExecuteIfBound(ECopilotCommandType::AnalyzeProject,
		TEXT("Create a detailed implementation plan for an Unreal Engine project. Break it into steps with file names and code structure. Plan: ") + Args);
	return TEXT("Creating implementation plan...");
}

FString FGitHubCopilotUESlashCommands::HandleReview(const FString& Args)
{
	FString Prompt = Args.IsEmpty() ? TEXT("Review the recent code changes in this Unreal project. Look for bugs, performance issues, and style problems.") : Args;
	OnSendPrompt.ExecuteIfBound(ECopilotCommandType::AnalyzeSelection,
		TEXT("Act as a code review agent. ") + Prompt);
	return TEXT("Running code review...");
}

FString FGitHubCopilotUESlashCommands::HandleDiff(const FString& Args)
{
	OnSendPrompt.ExecuteIfBound(ECopilotCommandType::AnalyzeProject,
		TEXT("Review and summarize the recent file changes in this Unreal project directory. List modified files and describe each change."));
	return TEXT("Analyzing diffs...");
}

FString FGitHubCopilotUESlashCommands::HandleResearch(const FString& Args)
{
	if (Args.IsEmpty())
	{
		return TEXT("Usage: /research <topic>\nExample: /research best practices for UE5 multiplayer replication");
	}
	OnSendPrompt.ExecuteIfBound(ECopilotCommandType::AnalyzeProject,
		TEXT("Conduct a deep research investigation on this topic, including Unreal Engine best practices, documentation references, and examples: ") + Args);
	return FString::Printf(TEXT("Researching: %s ..."), *Args);
}

FString FGitHubCopilotUESlashCommands::HandleExplain(const FString& Args)
{
	FString Prompt = Args.IsEmpty() ? TEXT("Explain the currently selected code or asset.") : Args;
	OnSendPrompt.ExecuteIfBound(ECopilotCommandType::ExplainCode, Prompt);
	return TEXT("Explaining...");
}

FString FGitHubCopilotUESlashCommands::HandleRefactor(const FString& Args)
{
	FString Prompt = Args.IsEmpty() ? TEXT("Suggest refactoring improvements for the selected code.") : Args;
	OnSendPrompt.ExecuteIfBound(ECopilotCommandType::SuggestRefactor, Prompt);
	return TEXT("Analyzing for refactoring suggestions...");
}

FString FGitHubCopilotUESlashCommands::HandleGenerate(const FString& Args)
{
	if (Args.IsEmpty())
	{
		return TEXT("Usage: /generate <type> [name]\nTypes: class, component, blueprint, utility\nExample: /generate class MyNewActor");
	}

	FString Type, Name;
	if (!Args.Split(TEXT(" "), &Type, &Name))
	{
		Type = Args;
		Name = TEXT("");
	}
	Type = Type.ToLower();

	if (Type == TEXT("class"))
	{
		OnSendPrompt.ExecuteIfBound(ECopilotCommandType::CreateCppClass, Name);
		return FString::Printf(TEXT("Generating C++ class: %s"), *Name);
	}
	else if (Type == TEXT("component"))
	{
		OnSendPrompt.ExecuteIfBound(ECopilotCommandType::CreateActorComponent, Name);
		return FString::Printf(TEXT("Generating Actor Component: %s"), *Name);
	}
	else if (Type == TEXT("blueprint") || Type == TEXT("bp"))
	{
		OnSendPrompt.ExecuteIfBound(ECopilotCommandType::CreateBlueprintFunctionLibrary, Name);
		return FString::Printf(TEXT("Generating Blueprint Function Library: %s"), *Name);
	}
	else if (Type == TEXT("utility"))
	{
		OnSendPrompt.ExecuteIfBound(ECopilotCommandType::GenerateEditorUtilityHelper, Name);
		return FString::Printf(TEXT("Generating Editor Utility: %s"), *Name);
	}

	return FString::Printf(TEXT("Unknown type: %s. Use class, component, blueprint, or utility."), *Type);
}

FString FGitHubCopilotUESlashCommands::HandleCompile(const FString& Args)
{
	OnSendPrompt.ExecuteIfBound(ECopilotCommandType::TriggerCompile, TEXT(""));
	return TEXT("Triggering compile...");
}

FString FGitHubCopilotUESlashCommands::HandleLiveCoding(const FString& Args)
{
	OnSendPrompt.ExecuteIfBound(ECopilotCommandType::TriggerLiveCoding, TEXT(""));
	return TEXT("Triggering Live Coding...");
}

FString FGitHubCopilotUESlashCommands::HandleTest(const FString& Args)
{
	OnSendPrompt.ExecuteIfBound(ECopilotCommandType::RunAutomationTests, Args);
	return Args.IsEmpty() ? TEXT("Running all automation tests...") : FString::Printf(TEXT("Running tests matching: %s"), *Args);
}

FString FGitHubCopilotUESlashCommands::HandleInit(const FString& Args)
{
	FString InstructionsPath = FPaths::Combine(FPaths::ProjectDir(), TEXT(".github/copilot-instructions.md"));
	if (FPaths::FileExists(InstructionsPath))
	{
		FString Content;
		FFileHelper::LoadFileToString(Content, *InstructionsPath);
		return FString::Printf(TEXT("=== Copilot Instructions ===\nFile: %s\n\n%s"), *InstructionsPath, *Content);
	}
	return FString::Printf(TEXT("No Copilot instructions file found.\nCreate one at: %s\nThis file provides project-specific context to Copilot."), *InstructionsPath);
}

FString FGitHubCopilotUESlashCommands::HandleSession(const FString& Args)
{
	FString Result = TEXT("=== Session Info ===\n");
	Result += FString::Printf(TEXT("Project: %s\n"), FApp::GetProjectName());
	Result += FString::Printf(TEXT("Project Dir: %s\n"), *FPaths::ProjectDir());

	if (BridgeService.IsValid())
	{
		Result += FString::Printf(TEXT("Auth: %s\n"), BridgeService->IsAuthenticated() ? TEXT("Signed in") : TEXT("Not signed in"));
		if (BridgeService->IsAuthenticated())
		{
			Result += FString::Printf(TEXT("User: %s\n"), *BridgeService->GetUsername());
			Result += FString::Printf(TEXT("Model: %s\n"), *BridgeService->GetActiveModel());
		}
	}

	return Result;
}

FString FGitHubCopilotUESlashCommands::HandleCompact(const FString& Args)
{
	OnSendPrompt.ExecuteIfBound(ECopilotCommandType::AnalyzeProject,
		TEXT("Summarize the current conversation context into a compact form, preserving key decisions and code changes."));
	return TEXT("Compacting conversation context...");
}

FString FGitHubCopilotUESlashCommands::HandlePr(const FString& Args)
{
	FString Sub = Args.IsEmpty() ? TEXT("view") : Args.ToLower();
	OnSendPrompt.ExecuteIfBound(ECopilotCommandType::AnalyzeProject,
		FString::Printf(TEXT("PR operation: %s. Analyze the current branch changes and help with pull request management."), *Sub));
	return FString::Printf(TEXT("PR operation: %s ..."), *Sub);
}

FString FGitHubCopilotUESlashCommands::HandleShare(const FString& Args)
{
	return TEXT("Share: Session export is not yet implemented. Use /copy to copy the last response to clipboard.");
}

FString FGitHubCopilotUESlashCommands::HandleFleet(const FString& Args)
{
	if (Args.IsEmpty())
	{
		return TEXT("Usage: /fleet <prompt>\nFleet mode sends your prompt to multiple agents in parallel.");
	}
	OnSendPrompt.ExecuteIfBound(ECopilotCommandType::AnalyzeProject,
		TEXT("Execute in fleet/parallel agent mode: ") + Args);
	return TEXT("Fleet mode: dispatching...");
}

FString FGitHubCopilotUESlashCommands::HandleAgent(const FString& Args)
{
	return TEXT("=== Available Agents ===\n  code-review    - Analyze code for issues\n  refactor       - Suggest improvements\n  quest-audit    - VR/Quest readiness\n  generate       - Code generation\n\nUse /review, /refactor, /quest, or /generate to activate.");
}

FString FGitHubCopilotUESlashCommands::HandleSkills(const FString& Args)
{
	return TEXT("=== Copilot Skills ===\n  code-analysis   - Analyze UE5 C++ and Blueprints\n  code-generation - Generate classes and components\n  file-patching   - Edit and patch source files\n  project-context - Gather project state\n  vr-analysis     - Quest/XR readiness\n  compile         - Build and Live Coding\n\nAll skills are enabled by default.");
}

FString FGitHubCopilotUESlashCommands::HandleQuest(const FString& Args)
{
	OnSendPrompt.ExecuteIfBound(ECopilotCommandType::RunQuestAudit, Args);
	return TEXT("Running Meta Quest audit...");
}

FString FGitHubCopilotUESlashCommands::HandleVR(const FString& Args)
{
	OnSendPrompt.ExecuteIfBound(ECopilotCommandType::GatherVRContext, Args);
	return TEXT("Analyzing VR/XR setup...");
}

FString FGitHubCopilotUESlashCommands::HandleOpen(const FString& Args)
{
	if (Args.IsEmpty())
	{
		return TEXT("Usage: /open <file-path or asset-path>");
	}
	OnSendPrompt.ExecuteIfBound(ECopilotCommandType::OpenFile, Args);
	return FString::Printf(TEXT("Opening: %s"), *Args);
}

FString FGitHubCopilotUESlashCommands::HandlePatch(const FString& Args)
{
	if (Args.IsEmpty())
	{
		return TEXT("Usage: /patch <file-path>\nProvide the proposed content in the prompt area.");
	}
	OnSendPrompt.ExecuteIfBound(ECopilotCommandType::PatchFile, Args);
	return FString::Printf(TEXT("Previewing patch for: %s"), *Args);
}

FString FGitHubCopilotUESlashCommands::HandleRollback(const FString& Args)
{
	OnSendPrompt.ExecuteIfBound(ECopilotCommandType::RollbackPatch, Args);
	return Args.IsEmpty() ? TEXT("Rolling back last patch...") : FString::Printf(TEXT("Rolling back: %s"), *Args);
}

FString FGitHubCopilotUESlashCommands::HandleKnowledge(const FString& Args)
{
	// /knowledge — Scans the project folder locally and writes project.md.
	// No AI call. This works exactly like a human browsing the folder structure.

	if (!ContextService.IsValid())
	{
		return TEXT("Context service not available.");
	}

	FCopilotProjectContext Ctx = ContextService->GatherProjectContext();
	FString ProjectDir = FPaths::ProjectDir();

	// ── 1. Scan Source directory ──
	FString SourceDir = FPaths::Combine(ProjectDir, TEXT("Source"));
	TArray<FString> AllSourceFiles;
	TArray<FString> HeaderFiles;
	TArray<FString> CppFiles;
	TArray<FString> BuildCsFiles;
	if (FPaths::DirectoryExists(SourceDir))
	{
		IFileManager::Get().FindFilesRecursive(AllSourceFiles, *SourceDir, TEXT("*.*"), true, false);
		for (const FString& F : AllSourceFiles)
		{
			if (F.EndsWith(TEXT(".h")))           HeaderFiles.Add(F);
			else if (F.EndsWith(TEXT(".cpp")))     CppFiles.Add(F);
			else if (F.EndsWith(TEXT(".Build.cs")) || F.EndsWith(TEXT(".Target.cs"))) BuildCsFiles.Add(F);
		}
	}

	// ── 2. Scan Config directory ──
	FString ConfigDir = FPaths::Combine(ProjectDir, TEXT("Config"));
	TArray<FString> ConfigFiles;
	if (FPaths::DirectoryExists(ConfigDir))
	{
		IFileManager::Get().FindFilesRecursive(ConfigFiles, *ConfigDir, TEXT("*.ini"), true, false);
	}

	// ── 3. Read .uproject file ──
	FString UProjectContent;
	{
		TArray<FString> UProjectFiles;
		IFileManager::Get().FindFiles(UProjectFiles, *ProjectDir, TEXT("*.uproject"));
		if (UProjectFiles.Num() > 0)
		{
			FString UProjectPath = FPaths::Combine(ProjectDir, UProjectFiles[0]);
			FFileHelper::LoadFileToString(UProjectContent, *UProjectPath);
		}
	}

	// ── 4. Scan for plugin .uplugin files under Plugins/ ──
	FString PluginsDir = FPaths::Combine(ProjectDir, TEXT("Plugins"));
	TArray<FString> LocalPluginFiles;
	if (FPaths::DirectoryExists(PluginsDir))
	{
		IFileManager::Get().FindFilesRecursive(LocalPluginFiles, *PluginsDir, TEXT("*.uplugin"), true, false);
	}

	// ── 5. Parse headers for UCLASS / USTRUCT / UENUM / UFUNCTION macros ──
	struct FClassInfo
	{
		FString FileName;
		FString ClassName;
		FString ParentClass;
		FString Macro; // UCLASS, USTRUCT, etc.
	};
	TArray<FClassInfo> DiscoveredClasses;

	for (const FString& HFile : HeaderFiles)
	{
		FString Content;
		if (!FFileHelper::LoadFileToString(Content, *HFile)) continue;

		TArray<FString> Lines;
		Content.ParseIntoArrayLines(Lines);
		FString CleanName = FPaths::GetCleanFilename(HFile);

		for (int32 i = 0; i < Lines.Num(); ++i)
		{
			const FString& Line = Lines[i];
			FString Macro;
			if      (Line.Contains(TEXT("UCLASS(")))   Macro = TEXT("UCLASS");
			else if (Line.Contains(TEXT("USTRUCT(")))  Macro = TEXT("USTRUCT");
			else if (Line.Contains(TEXT("UENUM(")))    Macro = TEXT("UENUM");
			else continue;

			// Next non-empty line after UCLASS() usually has "class XXXX_API ClassName : public Parent"
			for (int32 j = i + 1; j < FMath::Min(i + 4, Lines.Num()); ++j)
			{
				FString ClassLine = Lines[j].TrimStartAndEnd();
				if (ClassLine.IsEmpty()) continue;

				// Try to extract "class [API] Name : public Parent"
				FString ClassName, ParentClass;
				if (ClassLine.StartsWith(TEXT("class ")) || ClassLine.StartsWith(TEXT("struct ")) || ClassLine.StartsWith(TEXT("enum ")))
				{
					// Remove "class " / "struct " / "enum "
					FString Rest = ClassLine;
					Rest.RemoveFromStart(TEXT("class "));
					Rest.RemoveFromStart(TEXT("struct "));
					Rest.RemoveFromStart(TEXT("enum "));

					// Skip API macro (e.g., DROPSHOTIN_API)
					if (Rest.Contains(TEXT("_API ")))
					{
						int32 ApiIdx;
						if (Rest.FindChar(TEXT(' '), ApiIdx))
						{
							Rest = Rest.Mid(ApiIdx + 1);
						}
					}

					// Split on " : " for parent
					int32 ColonIdx;
					if (Rest.FindChar(TEXT(':'), ColonIdx))
					{
						ClassName = Rest.Left(ColonIdx).TrimStartAndEnd();
						FString ParentPart = Rest.Mid(ColonIdx + 1).TrimStartAndEnd();
						ParentPart.RemoveFromStart(TEXT("public "));
						ParentPart.RemoveFromStart(TEXT("protected "));
						ParentPart.RemoveFromStart(TEXT("private "));
						// Trim trailing { or whitespace
						int32 BraceIdx;
						if (ParentPart.FindChar(TEXT('{'), BraceIdx))
						{
							ParentPart = ParentPart.Left(BraceIdx);
						}
						ParentClass = ParentPart.TrimStartAndEnd();
					}
					else
					{
						// No parent class
						int32 BraceIdx;
						if (Rest.FindChar(TEXT('{'), BraceIdx))
						{
							Rest = Rest.Left(BraceIdx);
						}
						ClassName = Rest.TrimStartAndEnd();
					}

					if (!ClassName.IsEmpty())
					{
						DiscoveredClasses.Add({CleanName, ClassName, ParentClass, Macro});
					}
				}
				break;
			}
		}
	}

	// ── 6. Read Build.cs files for module dependencies ──
	TArray<TPair<FString, FString>> ModuleDeps; // ModuleName -> dependencies snippet
	for (const FString& BcsFile : BuildCsFiles)
	{
		FString Content;
		if (!FFileHelper::LoadFileToString(Content, *BcsFile)) continue;
		FString ModName = FPaths::GetBaseFilename(BcsFile);
		ModName.RemoveFromEnd(TEXT(".Build"));

		// Extract PublicDependencyModuleNames and PrivateDependencyModuleNames lines
		TArray<FString> Lines;
		Content.ParseIntoArrayLines(Lines);
		FString Deps;
		for (const FString& Line : Lines)
		{
			if (Line.Contains(TEXT("DependencyModuleNames")) && Line.Contains(TEXT("Add")))
			{
				Deps += TEXT("  ") + Line.TrimStartAndEnd() + TEXT("\n");
			}
		}
		if (!Deps.IsEmpty())
		{
			ModuleDeps.Add(TPair<FString, FString>(ModName, Deps));
		}
	}

	// ── 7. Identify VR / XR plugins from enabled list ──
	TArray<FString> VRPlugins;
	for (const FString& P : Ctx.EnabledPlugins)
	{
		FString Lower = P.ToLower();
		if (Lower.Contains(TEXT("openxr")) || Lower.Contains(TEXT("oculus")) || Lower.Contains(TEXT("quest"))
			|| Lower.Contains(TEXT("meta")) || Lower.Contains(TEXT("xr")) || Lower.Contains(TEXT("vr"))
			|| Lower.Contains(TEXT("headmounted")) || Lower.Contains(TEXT("handtracking")))
		{
			VRPlugins.Add(P);
		}
	}

	// ── 8. Read DefaultEngine.ini for key config snippets ──
	FString DefaultEngineContent;
	FString DefaultEnginePath = FPaths::Combine(ConfigDir, TEXT("DefaultEngine.ini"));
	FFileHelper::LoadFileToString(DefaultEngineContent, *DefaultEnginePath);

	// ── 9. Read DefaultGame.ini for project description ──
	FString DefaultGameContent;
	FString DefaultGamePath = FPaths::Combine(ConfigDir, TEXT("DefaultGame.ini"));
	FFileHelper::LoadFileToString(DefaultGameContent, *DefaultGamePath);

	// ── 10. Count Content/ assets by extension ──
	FString ContentDir = FPaths::Combine(ProjectDir, TEXT("Content"));
	TMap<FString, int32> AssetCounts;
	int32 TotalAssets = 0;
	if (FPaths::DirectoryExists(ContentDir))
	{
		TArray<FString> ContentFiles;
		IFileManager::Get().FindFilesRecursive(ContentFiles, *ContentDir, TEXT("*.uasset"), true, false);
		TotalAssets = ContentFiles.Num();
		// Also count .umap files
		TArray<FString> MapFiles;
		IFileManager::Get().FindFilesRecursive(MapFiles, *ContentDir, TEXT("*.umap"), true, false);
		if (MapFiles.Num() > 0) AssetCounts.Add(TEXT(".umap"), MapFiles.Num());
		AssetCounts.Add(TEXT(".uasset"), TotalAssets);
	}

	// ════════════════════════════════════════════════════════════════
	// BUILD THE MARKDOWN DOCUMENT
	// ════════════════════════════════════════════════════════════════

	FString MD;

	MD += FString::Printf(TEXT("# %s — Project Knowledge\n\n"), *Ctx.ProjectName);
	MD += FString::Printf(TEXT("*Generated by GitHubCopilotUE /knowledge on %s*\n\n"), *FDateTime::Now().ToString());

	// --- Overview ---
	MD += TEXT("## Project Overview\n\n");
	MD += FString::Printf(TEXT("| Property | Value |\n"));
	MD += FString::Printf(TEXT("|----------|-------|\n"));
	MD += FString::Printf(TEXT("| **Project Name** | %s |\n"), *Ctx.ProjectName);
	MD += FString::Printf(TEXT("| **Engine Version** | %s |\n"), *Ctx.EngineVersion);
	MD += FString::Printf(TEXT("| **Current Map** | %s |\n"), *Ctx.CurrentMapName);
	MD += FString::Printf(TEXT("| **Active Platform** | %s |\n"), *Ctx.ActivePlatform);
	MD += FString::Printf(TEXT("| **Source Files** | %d headers, %d cpp files |\n"), HeaderFiles.Num(), CppFiles.Num());
	MD += FString::Printf(TEXT("| **Config Files** | %d |\n"), ConfigFiles.Num());
	MD += FString::Printf(TEXT("| **Content Assets** | %d |\n"), TotalAssets);
	MD += FString::Printf(TEXT("| **Project Plugins** | %d local |\n"), LocalPluginFiles.Num());
	MD += FString::Printf(TEXT("| **Engine Plugins Enabled** | %d |\n"), Ctx.EnabledPlugins.Num());
	MD += TEXT("\n");

	// --- .uproject ---
	if (!UProjectContent.IsEmpty())
	{
		MD += TEXT("## .uproject File\n\n");
		MD += TEXT("```json\n");
		MD += UProjectContent + TEXT("\n");
		MD += TEXT("```\n\n");
	}

	// --- Modules ---
	MD += TEXT("## Modules\n\n");
	if (Ctx.ModuleNames.Num() > 0)
	{
		for (const FString& M : Ctx.ModuleNames)
		{
			MD += FString::Printf(TEXT("- `%s`\n"), *M);
		}
	}
	else
	{
		MD += TEXT("No game modules detected.\n");
	}
	MD += TEXT("\n");

	// --- Module Dependencies (from Build.cs) ---
	if (ModuleDeps.Num() > 0)
	{
		MD += TEXT("## Module Dependencies\n\n");
		for (const auto& Dep : ModuleDeps)
		{
			MD += FString::Printf(TEXT("### %s\n\n"), *Dep.Key);
			MD += TEXT("```csharp\n");
			MD += Dep.Value;
			MD += TEXT("```\n\n");
		}
	}

	// --- Source File Inventory ---
	MD += TEXT("## Source File Inventory\n\n");
	MD += TEXT("### Headers (.h)\n\n");
	for (const FString& F : HeaderFiles)
	{
		FString Rel = F;
		FPaths::MakePathRelativeTo(Rel, *ProjectDir);
		MD += FString::Printf(TEXT("- `%s`\n"), *Rel);
	}
	MD += TEXT("\n### Implementation (.cpp)\n\n");
	for (const FString& F : CppFiles)
	{
		FString Rel = F;
		FPaths::MakePathRelativeTo(Rel, *ProjectDir);
		MD += FString::Printf(TEXT("- `%s`\n"), *Rel);
	}
	if (BuildCsFiles.Num() > 0)
	{
		MD += TEXT("\n### Build Scripts\n\n");
		for (const FString& F : BuildCsFiles)
		{
			MD += FString::Printf(TEXT("- `%s`\n"), *FPaths::GetCleanFilename(F));
		}
	}
	MD += TEXT("\n");

	// --- Discovered Classes ---
	if (DiscoveredClasses.Num() > 0)
	{
		MD += TEXT("## Discovered UE Classes & Structs\n\n");
		MD += TEXT("| Type | Class Name | Parent | Header |\n");
		MD += TEXT("|------|-----------|--------|--------|\n");
		for (const FClassInfo& CI : DiscoveredClasses)
		{
			MD += FString::Printf(TEXT("| %s | `%s` | `%s` | %s |\n"),
				*CI.Macro, *CI.ClassName,
				CI.ParentClass.IsEmpty() ? TEXT("—") : *CI.ParentClass,
				*CI.FileName);
		}
		MD += TEXT("\n");
	}

	// --- Project Plugins ---
	if (LocalPluginFiles.Num() > 0)
	{
		MD += TEXT("## Project Plugins (Local)\n\n");
		for (const FString& PFile : LocalPluginFiles)
		{
			FString Rel = PFile;
			FPaths::MakePathRelativeTo(Rel, *ProjectDir);
			FString PluginName = FPaths::GetBaseFilename(PFile);

			// Try to read the .uplugin for description
			FString PluginJson;
			FFileHelper::LoadFileToString(PluginJson, *PFile);
			FString Desc;
			TSharedPtr<FJsonObject> JObj;
			TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(PluginJson);
			if (FJsonSerializer::Deserialize(Reader, JObj) && JObj.IsValid())
			{
				Desc = JObj->GetStringField(TEXT("Description"));
			}
			MD += FString::Printf(TEXT("### %s\n\n"), *PluginName);
			if (!Desc.IsEmpty())
			{
				MD += FString::Printf(TEXT("> %s\n\n"), *Desc);
			}
			MD += FString::Printf(TEXT("Path: `%s`\n\n"), *Rel);
		}
	}

	// --- VR / XR Setup ---
	MD += TEXT("## VR / XR Configuration\n\n");
	if (VRPlugins.Num() > 0)
	{
		MD += TEXT("**VR/XR Plugins Detected:**\n\n");
		for (const FString& VP : VRPlugins)
		{
			MD += FString::Printf(TEXT("- %s\n"), *VP);
		}
		MD += TEXT("\n");
	}
	else
	{
		MD += TEXT("No VR/XR plugins detected among enabled plugins.\n\n");
	}
	if (!Ctx.QuestReadinessSummary.IsEmpty())
	{
		MD += TEXT("**Quest Readiness:**\n\n");
		MD += Ctx.QuestReadinessSummary + TEXT("\n\n");
	}

	// --- Config Files ---
	MD += TEXT("## Configuration Files\n\n");
	for (const FString& CF : ConfigFiles)
	{
		FString Rel = CF;
		FPaths::MakePathRelativeTo(Rel, *ProjectDir);
		MD += FString::Printf(TEXT("- `%s`\n"), *Rel);
	}
	MD += TEXT("\n");

	// --- Key Config Snippets ---
	// Extract interesting sections from DefaultEngine.ini
	if (!DefaultEngineContent.IsEmpty())
	{
		MD += TEXT("## Key Engine Settings (DefaultEngine.ini)\n\n");
		MD += TEXT("```ini\n");
		TArray<FString> EngLines;
		DefaultEngineContent.ParseIntoArrayLines(EngLines);
		// Only include sections with VR, Rendering, Platform, or Android
		bool bIncluding = false;
		int32 SnippetLines = 0;
		for (const FString& EL : EngLines)
		{
			if (EL.StartsWith(TEXT("[")))
			{
				FString Lower = EL.ToLower();
				bIncluding = (Lower.Contains(TEXT("render")) || Lower.Contains(TEXT("vr"))
					|| Lower.Contains(TEXT("xr")) || Lower.Contains(TEXT("android"))
					|| Lower.Contains(TEXT("platform")) || Lower.Contains(TEXT("quest"))
					|| Lower.Contains(TEXT("openxr")) || Lower.Contains(TEXT("/script/engine")));
				if (bIncluding)
				{
					MD += TEXT("\n") + EL + TEXT("\n");
					SnippetLines++;
				}
			}
			else if (bIncluding && !EL.IsEmpty() && SnippetLines < 200)
			{
				MD += EL + TEXT("\n");
				SnippetLines++;
			}
		}
		MD += TEXT("```\n\n");
	}

	// --- Content Assets Summary ---
	MD += TEXT("## Content Assets Summary\n\n");
	MD += FString::Printf(TEXT("- **Total .uasset files:** %d\n"), TotalAssets);
	for (const auto& AC : AssetCounts)
	{
		if (AC.Key != TEXT(".uasset"))
		{
			MD += FString::Printf(TEXT("- **%s files:** %d\n"), *AC.Key, AC.Value);
		}
	}
	MD += TEXT("\n");

	// --- Footer ---
	MD += TEXT("---\n\n");
	MD += TEXT("*This document was generated by scanning the project folder locally. No AI was used in its creation.*\n");
	MD += FString::Printf(TEXT("*Project root: `%s`*\n"), *FPaths::ConvertRelativePathToFull(ProjectDir));

	// ════════════════════════════════════════════════════════════════
	// SAVE TO DISK
	// ════════════════════════════════════════════════════════════════

	FString OutputPath = FPaths::Combine(ProjectDir, TEXT("project.md"));
	if (FFileHelper::SaveStringToFile(MD, *OutputPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		Log(FString::Printf(TEXT("project.md saved: %d chars, %d classes, %d source files"),
			MD.Len(), DiscoveredClasses.Num(), AllSourceFiles.Num()));

		return FString::Printf(
			TEXT("project.md saved to project root!\n")
			TEXT("  Location: %s\n")
			TEXT("  Size: %d characters\n")
			TEXT("  ──────────────────────────────\n")
			TEXT("  Headers:   %d\n")
			TEXT("  CPP files: %d\n")
			TEXT("  Classes:   %d (UCLASS/USTRUCT/UENUM)\n")
			TEXT("  Plugins:   %d local, %d engine\n")
			TEXT("  Configs:   %d\n")
			TEXT("  Assets:    %d\n")
			TEXT("  VR/XR:     %d plugins\n")
			TEXT("  ──────────────────────────────\n")
			TEXT("  Open project.md in any Markdown viewer to explore."),
			*OutputPath, MD.Len(),
			HeaderFiles.Num(), CppFiles.Num(), DiscoveredClasses.Num(),
			LocalPluginFiles.Num(), Ctx.EnabledPlugins.Num(),
			ConfigFiles.Num(), TotalAssets, VRPlugins.Num());
	}
	else
	{
		return FString::Printf(TEXT("ERROR: Failed to write project.md to %s"), *OutputPath);
	}
}

FString FGitHubCopilotUESlashCommands::HandleBlueprint(const FString& Args)
{
	OnSendPrompt.ExecuteIfBound(ECopilotCommandType::CreateBlueprintFunctionLibrary, Args);
	return FString::Printf(TEXT("Creating Blueprint Function Library: %s"), *Args);
}

void FGitHubCopilotUESlashCommands::Log(const FString& Message)
{
	UE_LOG(LogGitHubCopilotUE, Verbose, TEXT("%s"), *Message);
	OnLogMessage.Broadcast(Message);
}
