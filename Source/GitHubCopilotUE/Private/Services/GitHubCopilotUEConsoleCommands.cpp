// Copyright GitHub, Inc. All Rights Reserved.

#include "Services/GitHubCopilotUEConsoleCommands.h"
#include "Services/GitHubCopilotUESlashCommands.h"
#include "Services/GitHubCopilotUEBridgeService.h"
#include "Services/GitHubCopilotUECommandRouter.h"
#include "Services/GitHubCopilotUEContextService.h"
#include "Services/GitHubCopilotUEFileService.h"
#include "Services/GitHubCopilotUECompileService.h"
#include "Services/GitHubCopilotUEQuestService.h"
#include "Services/GitHubCopilotUEPatchService.h"
#include "Services/GitHubCopilotUETypes.h"
#include "GitHubCopilotUESettings.h"
#include "HAL/PlatformApplicationMisc.h"
#include "Misc/App.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Widgets/Docking/SDockTab.h"
#include "Framework/Docking/TabManager.h"

#define COPILOT_VERSION TEXT("1.0.0")

FGitHubCopilotUEConsoleCommands::FGitHubCopilotUEConsoleCommands()
{
}

FGitHubCopilotUEConsoleCommands::~FGitHubCopilotUEConsoleCommands()
{
	Shutdown();
}

void FGitHubCopilotUEConsoleCommands::Initialize(
	TSharedPtr<FGitHubCopilotUEBridgeService> InBridgeService,
	TSharedPtr<FGitHubCopilotUECommandRouter> InCommandRouter,
	TSharedPtr<FGitHubCopilotUEContextService> InContextService,
	TSharedPtr<FGitHubCopilotUEFileService> InFileService,
	TSharedPtr<FGitHubCopilotUECompileService> InCompileService,
	TSharedPtr<FGitHubCopilotUEQuestService> InQuestService,
	TSharedPtr<FGitHubCopilotUEPatchService> InPatchService,
	TSharedPtr<FGitHubCopilotUESlashCommands> InSlashCommands)
{
	BridgeService = InBridgeService;
	CommandRouter = InCommandRouter;
	ContextService = InContextService;
	FileService = InFileService;
	CompileService = InCompileService;
	QuestService = InQuestService;
	PatchService = InPatchService;
	SlashCommands = InSlashCommands;

	// Wire up async response handler so AI responses print to Output Log
	if (CommandRouter.IsValid())
	{
		ResponseDelegateHandle = CommandRouter->OnResponseReceived.AddRaw(
			this, &FGitHubCopilotUEConsoleCommands::OnAIResponseReceived);
	}

	RegisterAllCommands();
	UE_LOG(LogGitHubCopilotUE, Verbose, TEXT("Console commands registered. Type 'Copilot.Help' in console for usage."));
}

void FGitHubCopilotUEConsoleCommands::Shutdown()
{
	if (CommandRouter.IsValid())
	{
		CommandRouter->OnResponseReceived.Remove(ResponseDelegateHandle);
	}
	UnregisterAllCommands();
}

// ============================================================================
// Registration
// ============================================================================

void FGitHubCopilotUEConsoleCommands::RegisterAllCommands()
{
	IConsoleManager& CM = IConsoleManager::Get();

	// Helper macro to reduce boilerplate
#define REGISTER_COPILOT_CMD(Name, Help, Handler) \
	RegisteredCommands.Add(CM.RegisterConsoleCommand( \
		TEXT("Copilot." Name), \
		TEXT(Help), \
		FConsoleCommandWithArgsDelegate::CreateRaw(this, &FGitHubCopilotUEConsoleCommands::Handler), \
		ECVF_Default));

	// === Core Chat ===
	REGISTER_COPILOT_CMD("Ask",         "Send a prompt to GitHub Copilot. Usage: Copilot.Ask <your question>", HandleAsk)

	// === Auth ===
	REGISTER_COPILOT_CMD("Login",       "Log in to GitHub Copilot (opens browser for device code auth)", HandleLogin)
	REGISTER_COPILOT_CMD("Logout",      "Log out of GitHub Copilot", HandleLogout)

	// === Model ===
	REGISTER_COPILOT_CMD("Model",       "Set active model. Usage: Copilot.Model <model-id>", HandleModel)
	REGISTER_COPILOT_CMD("Models",      "List all available models from your Copilot subscription", HandleModels)

	// === Info ===
	REGISTER_COPILOT_CMD("Help",        "Show all available Copilot console commands", HandleHelp)
	REGISTER_COPILOT_CMD("Status",      "Show connection and auth status", HandleStatus)
	REGISTER_COPILOT_CMD("Context",     "Show project context, selected assets, and editor state", HandleContext)
	REGISTER_COPILOT_CMD("Version",     "Show GitHubCopilotUE plugin version", HandleVersion)
	REGISTER_COPILOT_CMD("Usage",       "Show session usage and statistics", HandleUsage)
	REGISTER_COPILOT_CMD("User",        "Show the authenticated GitHub user", HandleUser)

	// === Code Intelligence ===
	REGISTER_COPILOT_CMD("Explain",     "Explain code. Usage: Copilot.Explain <code or file description>", HandleExplain)
	REGISTER_COPILOT_CMD("Review",      "Run code review. Usage: Copilot.Review [prompt]", HandleReview)
	REGISTER_COPILOT_CMD("Plan",        "Create implementation plan. Usage: Copilot.Plan <what to build>", HandlePlan)
	REGISTER_COPILOT_CMD("Research",    "Deep research. Usage: Copilot.Research <topic>", HandleResearch)
	REGISTER_COPILOT_CMD("Refactor",    "Suggest refactoring. Usage: Copilot.Refactor [prompt]", HandleRefactor)
	REGISTER_COPILOT_CMD("Diff",        "Review recent changes in the project", HandleDiff)

	// === Code Generation ===
	REGISTER_COPILOT_CMD("Generate",    "Generate code. Usage: Copilot.Generate <class|component|blueprint|utility> [name]", HandleGenerate)
	REGISTER_COPILOT_CMD("Blueprint",   "Create Blueprint Function Library. Usage: Copilot.Blueprint [name]", HandleBlueprint)

	// === File Operations ===
	REGISTER_COPILOT_CMD("Open",        "Open a file or asset. Usage: Copilot.Open <path>", HandleOpen)
	REGISTER_COPILOT_CMD("Patch",       "Preview/apply a patch. Usage: Copilot.Patch <file-path>", HandlePatch)
	REGISTER_COPILOT_CMD("Rollback",    "Rollback last patch. Usage: Copilot.Rollback [file-path]", HandleRollback)

	// === Build & Test ===
	REGISTER_COPILOT_CMD("Compile",     "Trigger project recompile", HandleCompile)
	REGISTER_COPILOT_CMD("Build",       "Trigger project recompile (alias for Compile)", HandleCompile)
	REGISTER_COPILOT_CMD("LiveCoding",  "Trigger Live Coding patch", HandleLiveCoding)
	REGISTER_COPILOT_CMD("LC",          "Trigger Live Coding (alias)", HandleLiveCoding)
	REGISTER_COPILOT_CMD("Test",        "Run automation tests. Usage: Copilot.Test [filter]", HandleTest)

	// === VR / Quest ===
	REGISTER_COPILOT_CMD("Quest",       "Analyze Meta Quest readiness", HandleQuest)
	REGISTER_COPILOT_CMD("VR",          "Analyze VR/XR setup and plugin state", HandleVR)
	REGISTER_COPILOT_CMD("XR",          "Analyze VR/XR setup (alias)", HandleVR)

	// === Session ===
	REGISTER_COPILOT_CMD("Session",     "Show session info and workspace summary", HandleSession)
	REGISTER_COPILOT_CMD("Clear",       "Clear conversation history", HandleClear)
	REGISTER_COPILOT_CMD("Copy",        "Copy the last response to clipboard", HandleCopy)
	REGISTER_COPILOT_CMD("Compact",     "Summarize conversation to reduce context", HandleCompact)

	// === PR & Git ===
	REGISTER_COPILOT_CMD("PR",          "Pull request operations. Usage: Copilot.PR [view|create|fix|auto]", HandlePR)
	REGISTER_COPILOT_CMD("Changelog",   "Display or summarize changelog", HandleChangelog)
	REGISTER_COPILOT_CMD("Chronicle",   "Session history tools. Usage: Copilot.Chronicle <standup|tips|improve>", HandleChronicle)

	// === Share & Export ===
	REGISTER_COPILOT_CMD("Share",       "Share session report to file or gist", HandleShare)

	// === Config ===
	REGISTER_COPILOT_CMD("Init",        "Show/create Copilot instructions for this project", HandleInit)
	REGISTER_COPILOT_CMD("ListDirs",    "Display all allowed write directories", HandleListDirs)
	REGISTER_COPILOT_CMD("AddDir",      "Add directory to allowed list. Usage: Copilot.AddDir <path>", HandleAddDir)

	// === Skills & Agents ===
	REGISTER_COPILOT_CMD("Fleet",       "Fleet mode (parallel agents). Usage: Copilot.Fleet <prompt>", HandleFleet)
	REGISTER_COPILOT_CMD("Agent",       "Browse available agents", HandleAgent)
	REGISTER_COPILOT_CMD("Skills",      "Manage skills for enhanced capabilities", HandleSkills)

	// === UI ===
	REGISTER_COPILOT_CMD("Panel",       "Open the GitHub Copilot dockable panel", HandlePanel)

	// === Knowledge ===
	REGISTER_COPILOT_CMD("Knowledge",   "Explore project and generate project.md synopsis. Usage: Copilot.Knowledge", HandleKnowledge)
	REGISTER_COPILOT_CMD("Know",        "Alias for Knowledge", HandleKnowledge)

#undef REGISTER_COPILOT_CMD

	// === Short aliases — type these directly in console ===
	// "ai" is the default chat command: just type "ai how do I..." 
	RegisteredCommands.Add(CM.RegisterConsoleCommand(
		TEXT("ai"),
		TEXT("Ask GitHub Copilot anything. Just type: ai <your question>"),
		FConsoleCommandWithArgsDelegate::CreateRaw(this, &FGitHubCopilotUEConsoleCommands::HandleAsk),
		ECVF_Default));

	// Single-letter shortcut for quick questions
	RegisteredCommands.Add(CM.RegisterConsoleCommand(
		TEXT("c"),
		TEXT("Quick Copilot chat shortcut. Usage: c <your question>"),
		FConsoleCommandWithArgsDelegate::CreateRaw(this, &FGitHubCopilotUEConsoleCommands::HandleAsk),
		ECVF_Default));

	Print(TEXT(""));
	Print(TEXT("=== GitHub Copilot UE ==="));
	Print(TEXT("Just type 'ai <your question>' in console to chat with Copilot."));
	Print(TEXT("Type 'Copilot.Help' for all commands, 'Copilot.Login' to sign in."));
	Print(TEXT(""));
}

void FGitHubCopilotUEConsoleCommands::UnregisterAllCommands()
{
	IConsoleManager& CM = IConsoleManager::Get();
	for (IConsoleObject* Obj : RegisteredCommands)
	{
		if (Obj)
		{
			CM.UnregisterConsoleObject(Obj);
		}
	}
	RegisteredCommands.Empty();
}

// ============================================================================
// Helpers
// ============================================================================

FString FGitHubCopilotUEConsoleCommands::JoinArgs(const TArray<FString>& Args, int32 StartIndex) const
{
	FString Result;
	for (int32 i = StartIndex; i < Args.Num(); ++i)
	{
		if (i > StartIndex) Result += TEXT(" ");
		Result += Args[i];
	}
	return Result;
}

void FGitHubCopilotUEConsoleCommands::Print(const FString& Message) const
{
	// Display level logs show in Output Log with visible formatting
	UE_LOG(LogGitHubCopilotUE, Display, TEXT("%s"), *Message);
}

void FGitHubCopilotUEConsoleCommands::PrintMultiLine(const FString& Message) const
{
	TArray<FString> Lines;
	Message.ParseIntoArrayLines(Lines);
	for (const FString& Line : Lines)
	{
		Print(Line);
	}
}

void FGitHubCopilotUEConsoleCommands::RouteSlashCommand(const FString& SlashInput)
{
	if (SlashCommands.IsValid())
	{
		FString Response;
		if (SlashCommands->ExecuteSlashCommand(SlashInput, Response))
		{
			PrintMultiLine(Response);
		}
		else
		{
			Print(FString::Printf(TEXT("Unknown command: %s"), *SlashInput));
		}
	}
}

void FGitHubCopilotUEConsoleCommands::OnAIResponseReceived(const FCopilotResponse& Response)
{
	if (Response.bSuccess)
	{
		// Always report API-returned model only; never guess.
		FString ModelName = TEXT("API:model-missing");
		if (const FString* Model = Response.ProviderMetadata.Find(TEXT("model")))
		{
			const FString TrimmedModel = Model->TrimStartAndEnd();
			if (!TrimmedModel.IsEmpty())
			{
				ModelName = TrimmedModel;
			}
		}

		Print(TEXT(""));
		Print(FString::Printf(TEXT("[%s]"), *ModelName));
		PrintMultiLine(Response.ResponseText);
		Print(TEXT(""));
	}
	else
	{
		Print(TEXT(""));
		Print(FString::Printf(TEXT("ERROR: %s"), *Response.ErrorMessage));
		Print(TEXT(""));
	}
}

// ============================================================================
// Command Handlers
// ============================================================================

void FGitHubCopilotUEConsoleCommands::HandleAsk(const TArray<FString>& Args)
{
	if (Args.Num() == 0)
	{
		Print(TEXT("Usage: Copilot.Ask <your question or prompt>"));
		Print(TEXT("Example: Copilot.Ask how do I replicate an actor in UE5?"));
		return;
	}

	FString Prompt = JoinArgs(Args);

	if (!BridgeService.IsValid() || !BridgeService->IsAuthenticated())
	{
		Print(TEXT("Not signed in. Use 'Copilot.Login' first."));
		return;
	}

	if (!CommandRouter.IsValid())
	{
		Print(TEXT("ERROR: Command router not available."));
		return;
	}

	Print(FString::Printf(TEXT("> %s"), *Prompt));
	Print(TEXT("Thinking..."));

	FCopilotRequest Request;
	Request.RequestId = FGuid::NewGuid().ToString();
	Request.CommandType = ECopilotCommandType::AnalyzeSelection;
	Request.UserPrompt = Prompt;
	Request.Timestamp = FDateTime::Now().ToString();

	// Use persistent conversation ID for multi-turn REPL chat
	if (ConsoleConversationId.IsEmpty())
	{
		ConsoleConversationId = FGuid::NewGuid().ToString(EGuidFormats::Short);
	}
	Request.ConversationId = ConsoleConversationId;

	// Attach project context
	if (ContextService.IsValid())
	{
		Request.ProjectContext = ContextService->GatherProjectContext();
	}

	CommandRouter->RouteCommand(Request);
}

void FGitHubCopilotUEConsoleCommands::HandleLogin(const TArray<FString>& Args)
{
	RouteSlashCommand(TEXT("/login"));
}

void FGitHubCopilotUEConsoleCommands::HandleLogout(const TArray<FString>& Args)
{
	RouteSlashCommand(TEXT("/logout"));
}

void FGitHubCopilotUEConsoleCommands::HandleModel(const TArray<FString>& Args)
{
	FString ModelArg = JoinArgs(Args);
	RouteSlashCommand(FString::Printf(TEXT("/model %s"), *ModelArg));
}

void FGitHubCopilotUEConsoleCommands::HandleModels(const TArray<FString>& Args)
{
	RouteSlashCommand(TEXT("/model"));
}

void FGitHubCopilotUEConsoleCommands::HandleStatus(const TArray<FString>& Args)
{
	Print(TEXT("=== GitHub Copilot UE Status ==="));

	if (BridgeService.IsValid())
	{
		ECopilotAuthState AuthState = BridgeService->GetAuthState();
		ECopilotConnectionStatus ConnStatus = BridgeService->GetConnectionStatus();

		FString AuthStr;
		switch (AuthState)
		{
			case ECopilotAuthState::NotAuthenticated:  AuthStr = TEXT("Not Authenticated"); break;
			case ECopilotAuthState::WaitingForUserCode: AuthStr = TEXT("Waiting for User Code"); break;
			case ECopilotAuthState::PollingForToken:    AuthStr = TEXT("Polling for Token..."); break;
			case ECopilotAuthState::Authenticated:      AuthStr = TEXT("Authenticated"); break;
			case ECopilotAuthState::Error:              AuthStr = TEXT("Error"); break;
		}

		FString ConnStr;
		switch (ConnStatus)
		{
			case ECopilotConnectionStatus::Connected:    ConnStr = TEXT("Connected"); break;
			case ECopilotConnectionStatus::Connecting:   ConnStr = TEXT("Connecting..."); break;
			case ECopilotConnectionStatus::Disconnected: ConnStr = TEXT("Disconnected"); break;
			case ECopilotConnectionStatus::Error:        ConnStr = TEXT("Error"); break;
		}

		Print(FString::Printf(TEXT("  Auth:       %s"), *AuthStr));
		Print(FString::Printf(TEXT("  Connection: %s"), *ConnStr));
		Print(FString::Printf(TEXT("  User:       %s"), *BridgeService->GetUsername()));
		Print(FString::Printf(TEXT("  Model:      %s"), *BridgeService->GetActiveModel()));
		Print(FString::Printf(TEXT("  Models:     %d available"), BridgeService->GetAvailableModels().Num()));
	}
	else
	{
		Print(TEXT("  Bridge service not available."));
	}
}

void FGitHubCopilotUEConsoleCommands::HandleHelp(const TArray<FString>& Args)
{
	Print(TEXT(""));
	Print(TEXT("╔══════════════════════════════════════════════════════════════════╗"));
	Print(TEXT("║              GitHub Copilot UE — Console Commands               ║"));
	Print(TEXT("╚══════════════════════════════════════════════════════════════════╝"));
	Print(TEXT(""));
	Print(TEXT("  QUICK START — just type and go:"));
	Print(TEXT("  ────────────────────────────────────────────────────────────"));
	Print(TEXT("  ai <prompt>                       Chat with Copilot (default)"));
	Print(TEXT("  c <prompt>                        Same thing, even shorter"));
	Print(TEXT(""));
	Print(TEXT("  CHAT"));
	Print(TEXT("  ────────────────────────────────────────────────────────────"));
	Print(TEXT("  Copilot.Ask <prompt>              Send a prompt to Copilot"));
	Print(TEXT(""));
	Print(TEXT("  AUTH & ACCOUNT"));
	Print(TEXT("  ────────────────────────────────────────────────────────────"));
	Print(TEXT("  Copilot.Login                     Log in with GitHub"));
	Print(TEXT("  Copilot.Logout                    Log out"));
	Print(TEXT("  Copilot.Status                    Show connection/auth status"));
	Print(TEXT("  Copilot.User                      Show authenticated user"));
	Print(TEXT(""));
	Print(TEXT("  MODEL"));
	Print(TEXT("  ────────────────────────────────────────────────────────────"));
	Print(TEXT("  Copilot.Models                    List available models"));
	Print(TEXT("  Copilot.Model <id>                Set active model"));
	Print(TEXT(""));
	Print(TEXT("  CODE INTELLIGENCE"));
	Print(TEXT("  ────────────────────────────────────────────────────────────"));
	Print(TEXT("  Copilot.Explain <code/desc>       Explain code"));
	Print(TEXT("  Copilot.Review [prompt]            Code review agent"));
	Print(TEXT("  Copilot.Plan <description>         Implementation plan"));
	Print(TEXT("  Copilot.Research <topic>           Deep research"));
	Print(TEXT("  Copilot.Refactor [prompt]          Suggest refactoring"));
	Print(TEXT("  Copilot.Diff                       Review project changes"));
	Print(TEXT(""));
	Print(TEXT("  CODE GENERATION"));
	Print(TEXT("  ────────────────────────────────────────────────────────────"));
	Print(TEXT("  Copilot.Generate <type> [name]     class|component|bp|utility"));
	Print(TEXT("  Copilot.Blueprint [name]           BP Function Library"));
	Print(TEXT(""));
	Print(TEXT("  FILE OPERATIONS"));
	Print(TEXT("  ────────────────────────────────────────────────────────────"));
	Print(TEXT("  Copilot.Open <path>                Open file or asset"));
	Print(TEXT("  Copilot.Patch <file>               Preview/apply patch"));
	Print(TEXT("  Copilot.Rollback [file]            Restore from backup"));
	Print(TEXT(""));
	Print(TEXT("  BUILD & TEST"));
	Print(TEXT("  ────────────────────────────────────────────────────────────"));
	Print(TEXT("  Copilot.Compile / Copilot.Build    Trigger recompile"));
	Print(TEXT("  Copilot.LiveCoding / Copilot.LC    Trigger Live Coding"));
	Print(TEXT("  Copilot.Test [filter]              Run automation tests"));
	Print(TEXT(""));
	Print(TEXT("  VR / QUEST"));
	Print(TEXT("  ────────────────────────────────────────────────────────────"));
	Print(TEXT("  Copilot.Quest                      Meta Quest audit"));
	Print(TEXT("  Copilot.VR / Copilot.XR            VR/XR setup analysis"));
	Print(TEXT(""));
	Print(TEXT("  SESSION"));
	Print(TEXT("  ────────────────────────────────────────────────────────────"));
	Print(TEXT("  Copilot.Session                    Session info"));
	Print(TEXT("  Copilot.Clear                      Clear history"));
	Print(TEXT("  Copilot.Copy                       Copy last response"));
	Print(TEXT("  Copilot.Compact                    Summarize context"));
	Print(TEXT(""));
	Print(TEXT("  PR & GIT"));
	Print(TEXT("  ────────────────────────────────────────────────────────────"));
	Print(TEXT("  Copilot.PR [view|create|fix]       Pull request ops"));
	Print(TEXT("  Copilot.Changelog [summarize]      Changelog"));
	Print(TEXT("  Copilot.Chronicle <sub>            Session history"));
	Print(TEXT(""));
	Print(TEXT("  CONFIG"));
	Print(TEXT("  ────────────────────────────────────────────────────────────"));
	Print(TEXT("  Copilot.Init                       Copilot instructions"));
	Print(TEXT("  Copilot.ListDirs                   Allowed directories"));
	Print(TEXT("  Copilot.AddDir <path>              Add write directory"));
	Print(TEXT(""));
	Print(TEXT("  AGENTS & SKILLS"));
	Print(TEXT("  ────────────────────────────────────────────────────────────"));
	Print(TEXT("  Copilot.Fleet <prompt>             Parallel agents"));
	Print(TEXT("  Copilot.Agent                      Browse agents"));
	Print(TEXT("  Copilot.Skills                     Manage skills"));
	Print(TEXT(""));
	Print(TEXT("  KNOWLEDGE"));
	Print(TEXT("  ────────────────────────────────────────────────────────────"));
	Print(TEXT("  Copilot.Knowledge                  Explore project & write project.md"));
	Print(TEXT(""));
	Print(TEXT("  MISC"));
	Print(TEXT("  ────────────────────────────────────────────────────────────"));
	Print(TEXT("  Copilot.Help                       This help"));
	Print(TEXT("  Copilot.Version                    Plugin version"));
	Print(TEXT("  Copilot.Usage                      Session stats"));
	Print(TEXT("  Copilot.Panel                      Open the UI panel"));
	Print(TEXT("  Copilot.Share                      Share session"));
	Print(TEXT(""));
	Print(TEXT("  Tip: Open Output Log (Window > Output Log) to see responses."));
	Print(TEXT(""));
}

void FGitHubCopilotUEConsoleCommands::HandleContext(const TArray<FString>& Args)
{
	RouteSlashCommand(TEXT("/context"));
}

void FGitHubCopilotUEConsoleCommands::HandleExplain(const TArray<FString>& Args)
{
	if (Args.Num() == 0)
	{
		Print(TEXT("Usage: Copilot.Explain <code or description>"));
		Print(TEXT("Example: Copilot.Explain what does AActor::BeginPlay do?"));
		return;
	}
	HandleAsk(TArray<FString>{TEXT("Explain this:"), JoinArgs(Args)});
}

void FGitHubCopilotUEConsoleCommands::HandleReview(const TArray<FString>& Args)
{
	FString Prompt = Args.Num() > 0 ? JoinArgs(Args) :
		TEXT("Review the current code changes and find bugs, performance issues, and improvements.");
	HandleAsk(TArray<FString>{TEXT("Code review:"), Prompt});
}

void FGitHubCopilotUEConsoleCommands::HandlePlan(const TArray<FString>& Args)
{
	if (Args.Num() == 0)
	{
		Print(TEXT("Usage: Copilot.Plan <what to build>"));
		Print(TEXT("Example: Copilot.Plan a multiplayer lobby system with Steam integration"));
		return;
	}
	HandleAsk(TArray<FString>{TEXT("Create an implementation plan for:"), JoinArgs(Args)});
}

void FGitHubCopilotUEConsoleCommands::HandleResearch(const TArray<FString>& Args)
{
	if (Args.Num() == 0)
	{
		Print(TEXT("Usage: Copilot.Research <topic>"));
		Print(TEXT("Example: Copilot.Research UE5 Nanite best practices for VR"));
		return;
	}
	HandleAsk(TArray<FString>{TEXT("Deep research on:"), JoinArgs(Args)});
}

void FGitHubCopilotUEConsoleCommands::HandleRefactor(const TArray<FString>& Args)
{
	FString Prompt = Args.Num() > 0 ? JoinArgs(Args) : TEXT("the selected code");
	HandleAsk(TArray<FString>{TEXT("Suggest refactoring for:"), Prompt});
}

void FGitHubCopilotUEConsoleCommands::HandleGenerate(const TArray<FString>& Args)
{
	RouteSlashCommand(TEXT("/generate ") + JoinArgs(Args));
}

void FGitHubCopilotUEConsoleCommands::HandleCompile(const TArray<FString>& Args)
{
	RouteSlashCommand(TEXT("/compile"));
}

void FGitHubCopilotUEConsoleCommands::HandleLiveCoding(const TArray<FString>& Args)
{
	RouteSlashCommand(TEXT("/live-coding"));
}

void FGitHubCopilotUEConsoleCommands::HandleTest(const TArray<FString>& Args)
{
	RouteSlashCommand(TEXT("/test ") + JoinArgs(Args));
}

void FGitHubCopilotUEConsoleCommands::HandleQuest(const TArray<FString>& Args)
{
	RouteSlashCommand(TEXT("/quest"));
}

void FGitHubCopilotUEConsoleCommands::HandleVR(const TArray<FString>& Args)
{
	RouteSlashCommand(TEXT("/vr"));
}

void FGitHubCopilotUEConsoleCommands::HandleOpen(const TArray<FString>& Args)
{
	if (Args.Num() == 0)
	{
		Print(TEXT("Usage: Copilot.Open <file-path or asset-path>"));
		return;
	}
	RouteSlashCommand(TEXT("/open ") + JoinArgs(Args));
}

void FGitHubCopilotUEConsoleCommands::HandlePatch(const TArray<FString>& Args)
{
	if (Args.Num() == 0)
	{
		Print(TEXT("Usage: Copilot.Patch <file-path>"));
		return;
	}
	RouteSlashCommand(TEXT("/patch ") + JoinArgs(Args));
}

void FGitHubCopilotUEConsoleCommands::HandleRollback(const TArray<FString>& Args)
{
	RouteSlashCommand(TEXT("/rollback ") + JoinArgs(Args));
}

void FGitHubCopilotUEConsoleCommands::HandleBlueprint(const TArray<FString>& Args)
{
	RouteSlashCommand(TEXT("/blueprint ") + JoinArgs(Args));
}

void FGitHubCopilotUEConsoleCommands::HandleDiff(const TArray<FString>& Args)
{
	RouteSlashCommand(TEXT("/diff"));
}

void FGitHubCopilotUEConsoleCommands::HandlePR(const TArray<FString>& Args)
{
	RouteSlashCommand(TEXT("/pr ") + JoinArgs(Args));
}

void FGitHubCopilotUEConsoleCommands::HandleSession(const TArray<FString>& Args)
{
	RouteSlashCommand(TEXT("/session ") + JoinArgs(Args));
}

void FGitHubCopilotUEConsoleCommands::HandleCopy(const TArray<FString>& Args)
{
	RouteSlashCommand(TEXT("/copy"));
}

void FGitHubCopilotUEConsoleCommands::HandleClear(const TArray<FString>& Args)
{
	RouteSlashCommand(TEXT("/clear"));
}

void FGitHubCopilotUEConsoleCommands::HandleInit(const TArray<FString>& Args)
{
	RouteSlashCommand(TEXT("/init ") + JoinArgs(Args));
}

void FGitHubCopilotUEConsoleCommands::HandleListDirs(const TArray<FString>& Args)
{
	RouteSlashCommand(TEXT("/list-dirs"));
}

void FGitHubCopilotUEConsoleCommands::HandleAddDir(const TArray<FString>& Args)
{
	if (Args.Num() == 0)
	{
		Print(TEXT("Usage: Copilot.AddDir <directory-path>"));
		return;
	}
	RouteSlashCommand(TEXT("/add-dir ") + JoinArgs(Args));
}

void FGitHubCopilotUEConsoleCommands::HandleCompact(const TArray<FString>& Args)
{
	RouteSlashCommand(TEXT("/compact"));
}

void FGitHubCopilotUEConsoleCommands::HandleFleet(const TArray<FString>& Args)
{
	if (Args.Num() == 0)
	{
		Print(TEXT("Usage: Copilot.Fleet <prompt>"));
		return;
	}
	RouteSlashCommand(TEXT("/fleet ") + JoinArgs(Args));
}

void FGitHubCopilotUEConsoleCommands::HandleAgent(const TArray<FString>& Args)
{
	RouteSlashCommand(TEXT("/agent"));
}

void FGitHubCopilotUEConsoleCommands::HandleSkills(const TArray<FString>& Args)
{
	RouteSlashCommand(TEXT("/skills ") + JoinArgs(Args));
}

void FGitHubCopilotUEConsoleCommands::HandleShare(const TArray<FString>& Args)
{
	RouteSlashCommand(TEXT("/share ") + JoinArgs(Args));
}

void FGitHubCopilotUEConsoleCommands::HandleChronicle(const TArray<FString>& Args)
{
	RouteSlashCommand(TEXT("/chronicle ") + JoinArgs(Args));
}

void FGitHubCopilotUEConsoleCommands::HandleChangelog(const TArray<FString>& Args)
{
	RouteSlashCommand(TEXT("/changelog ") + JoinArgs(Args));
}

void FGitHubCopilotUEConsoleCommands::HandleVersion(const TArray<FString>& Args)
{
	Print(TEXT(""));
	Print(TEXT("  GitHub Copilot UE"));
	Print(FString::Printf(TEXT("  Version: %s"), COPILOT_VERSION));
	Print(FString::Printf(TEXT("  Engine:  %s"), *FEngineVersion::Current().ToString()));
	Print(FString::Printf(TEXT("  Project: %s"), FApp::GetProjectName()));
	Print(TEXT(""));
}

void FGitHubCopilotUEConsoleCommands::HandleUsage(const TArray<FString>& Args)
{
	Print(TEXT("=== Session Usage ==="));

	if (BridgeService.IsValid())
	{
		Print(FString::Printf(TEXT("  Authenticated: %s"), BridgeService->IsAuthenticated() ? TEXT("Yes") : TEXT("No")));
		Print(FString::Printf(TEXT("  Active Model:  %s"), *BridgeService->GetActiveModel()));
		Print(FString::Printf(TEXT("  Models Loaded: %d"), BridgeService->GetAvailableModels().Num()));
	}

	Print(TEXT("  (Detailed usage metrics coming in a future update)"));
}

void FGitHubCopilotUEConsoleCommands::HandleUser(const TArray<FString>& Args)
{
	if (BridgeService.IsValid() && BridgeService->IsAuthenticated())
	{
		Print(FString::Printf(TEXT("  GitHub User: %s"), *BridgeService->GetUsername()));
	}
	else
	{
		Print(TEXT("  Not signed in. Use 'Copilot.Login' to authenticate."));
	}
}

void FGitHubCopilotUEConsoleCommands::HandlePanel(const TArray<FString>& Args)
{
	// Open the dockable tab
	FGlobalTabmanager::Get()->TryInvokeTab(FName(TEXT("GitHubCopilotUE")));
	Print(TEXT("Opening GitHub Copilot panel..."));
}

void FGitHubCopilotUEConsoleCommands::HandleKnowledge(const TArray<FString>& Args)
{
	Print(TEXT(""));
	Print(TEXT("=== /knowledge — Project Explorer ==="));
	Print(TEXT("Scanning project folder locally..."));
	Print(TEXT(""));

	// Route through slash command system — it scans and writes project.md directly, no AI
	RouteSlashCommand(TEXT("/knowledge"));
}
