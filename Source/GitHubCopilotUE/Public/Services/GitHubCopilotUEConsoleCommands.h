// Copyright GitHub, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "HAL/IConsoleManager.h"
#include "Services/GitHubCopilotUETypes.h"

class FGitHubCopilotUEBridgeService;
class FGitHubCopilotUECommandRouter;
class FGitHubCopilotUEContextService;
class FGitHubCopilotUEFileService;
class FGitHubCopilotUECompileService;
class FGitHubCopilotUEQuestService;
class FGitHubCopilotUEPatchService;
class FGitHubCopilotUESlashCommands;

/**
 * Registers all GitHub Copilot commands as Unreal Engine console commands.
 * Allows the user to interact with Copilot entirely through the ~ console
 * and Output Log, providing a CLI-like experience inside Unreal Editor.
 *
 * Usage: Open console (~), type "Copilot.Help" for all commands.
 * Responses appear in the Output Log (Window -> Output Log).
 */
class GITHUBCOPILOTUE_API FGitHubCopilotUEConsoleCommands
{
public:
	FGitHubCopilotUEConsoleCommands();
	~FGitHubCopilotUEConsoleCommands();

	/** Register all console commands. Call after services are initialized. */
	void Initialize(
		TSharedPtr<FGitHubCopilotUEBridgeService> InBridgeService,
		TSharedPtr<FGitHubCopilotUECommandRouter> InCommandRouter,
		TSharedPtr<FGitHubCopilotUEContextService> InContextService,
		TSharedPtr<FGitHubCopilotUEFileService> InFileService,
		TSharedPtr<FGitHubCopilotUECompileService> InCompileService,
		TSharedPtr<FGitHubCopilotUEQuestService> InQuestService,
		TSharedPtr<FGitHubCopilotUEPatchService> InPatchService,
		TSharedPtr<FGitHubCopilotUESlashCommands> InSlashCommands
	);

	/** Unregister all console commands */
	void Shutdown();

private:
	void RegisterAllCommands();
	void UnregisterAllCommands();

	// Console command handlers
	void HandleAsk(const TArray<FString>& Args);
	void HandleLogin(const TArray<FString>& Args);
	void HandleLogout(const TArray<FString>& Args);
	void HandleModel(const TArray<FString>& Args);
	void HandleModels(const TArray<FString>& Args);
	void HandleStatus(const TArray<FString>& Args);
	void HandleHelp(const TArray<FString>& Args);
	void HandleContext(const TArray<FString>& Args);
	void HandleExplain(const TArray<FString>& Args);
	void HandleReview(const TArray<FString>& Args);
	void HandlePlan(const TArray<FString>& Args);
	void HandleResearch(const TArray<FString>& Args);
	void HandleRefactor(const TArray<FString>& Args);
	void HandleGenerate(const TArray<FString>& Args);
	void HandleCompile(const TArray<FString>& Args);
	void HandleLiveCoding(const TArray<FString>& Args);
	void HandleTest(const TArray<FString>& Args);
	void HandleQuest(const TArray<FString>& Args);
	void HandleVR(const TArray<FString>& Args);
	void HandleOpen(const TArray<FString>& Args);
	void HandlePatch(const TArray<FString>& Args);
	void HandleRollback(const TArray<FString>& Args);
	void HandleBlueprint(const TArray<FString>& Args);
	void HandleDiff(const TArray<FString>& Args);
	void HandlePR(const TArray<FString>& Args);
	void HandleSession(const TArray<FString>& Args);
	void HandleCopy(const TArray<FString>& Args);
	void HandleClear(const TArray<FString>& Args);
	void HandleInit(const TArray<FString>& Args);
	void HandleListDirs(const TArray<FString>& Args);
	void HandleAddDir(const TArray<FString>& Args);
	void HandleCompact(const TArray<FString>& Args);
	void HandleFleet(const TArray<FString>& Args);
	void HandleAgent(const TArray<FString>& Args);
	void HandleSkills(const TArray<FString>& Args);
	void HandleShare(const TArray<FString>& Args);
	void HandleChronicle(const TArray<FString>& Args);
	void HandleChangelog(const TArray<FString>& Args);
	void HandleVersion(const TArray<FString>& Args);
	void HandleUsage(const TArray<FString>& Args);
	void HandleUser(const TArray<FString>& Args);
	void HandlePanel(const TArray<FString>& Args);
	void HandleKnowledge(const TArray<FString>& Args);

	// Helper to join args into a single prompt string
	FString JoinArgs(const TArray<FString>& Args, int32 StartIndex = 0) const;

	// Log output to both UE_LOG and console (appears in Output Log)
	void Print(const FString& Message) const;
	void PrintMultiLine(const FString& Message) const;

	// Route a slash command through the slash command system
	void RouteSlashCommand(const FString& SlashInput);

	// Callback for async AI responses
	void OnAIResponseReceived(const FCopilotResponse& Response);
	FDelegateHandle ResponseDelegateHandle;

	// Service references
	TSharedPtr<FGitHubCopilotUEBridgeService> BridgeService;
	TSharedPtr<FGitHubCopilotUECommandRouter> CommandRouter;
	TSharedPtr<FGitHubCopilotUEContextService> ContextService;
	TSharedPtr<FGitHubCopilotUEFileService> FileService;
	TSharedPtr<FGitHubCopilotUECompileService> CompileService;
	TSharedPtr<FGitHubCopilotUEQuestService> QuestService;
	TSharedPtr<FGitHubCopilotUEPatchService> PatchService;
	TSharedPtr<FGitHubCopilotUESlashCommands> SlashCommands;

	// Registered command objects (for cleanup)
	TArray<IConsoleObject*> RegisteredCommands;
};
