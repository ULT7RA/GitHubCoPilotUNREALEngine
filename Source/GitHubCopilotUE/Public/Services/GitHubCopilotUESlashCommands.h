// Copyright GitHub, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GitHubCopilotUETypes.h"

class FGitHubCopilotUECommandRouter;
class FGitHubCopilotUEBridgeService;
class FGitHubCopilotUEContextService;
class FGitHubCopilotUEFileService;

/** Describes a single slash command */
struct FCopilotSlashCommand
{
	FString Name;
	TArray<FString> Aliases;
	FString Usage;
	FString Description;
	bool bRequiresAuth;
	bool bLocalOnly; // Runs without backend
};

/**
 * Parses and routes slash commands typed in the prompt input.
 * Maps GitHub Copilot CLI commands to Unreal-appropriate equivalents.
 */
class GITHUBCOPILOTUE_API FGitHubCopilotUESlashCommands
{
public:
	FGitHubCopilotUESlashCommands();

	/** Initialize with service references */
	void Initialize(
		TSharedPtr<FGitHubCopilotUECommandRouter> InCommandRouter,
		TSharedPtr<FGitHubCopilotUEBridgeService> InBridgeService,
		TSharedPtr<FGitHubCopilotUEContextService> InContextService,
		TSharedPtr<FGitHubCopilotUEFileService> InFileService
	);

	/** Check if input starts with a slash command */
	bool IsSlashCommand(const FString& Input) const;

	/** Parse and execute a slash command. Returns true if handled. */
	bool ExecuteSlashCommand(const FString& Input, FString& OutResponse);

	/** Get all available slash commands for autocomplete/help */
	const TArray<FCopilotSlashCommand>& GetAllCommands() const { return Commands; }

	/** Get matching commands for autocomplete */
	TArray<FCopilotSlashCommand> GetMatchingCommands(const FString& Partial) const;

	/** Get the help text for all commands */
	FString GetHelpText() const;

	/** Delegate for log messages */
	FOnCopilotLogMessage OnLogMessage;

	/** Delegate to request sending a prompt to the backend (for commands that need AI) */
	DECLARE_DELEGATE_TwoParams(FOnSlashCommandSendPrompt, ECopilotCommandType /*Type*/, const FString& /*Prompt*/);
	FOnSlashCommandSendPrompt OnSendPrompt;

	/** Delegate for immediate UI responses */
	DECLARE_DELEGATE_OneParam(FOnSlashCommandResponse, const FString& /*Response*/);
	FOnSlashCommandResponse OnResponse;

private:
	void RegisterCommands();

	// Command handlers
	FString HandleHelp(const FString& Args);
	FString HandleClear(const FString& Args);
	FString HandleCopy(const FString& Args);
	FString HandleContext(const FString& Args);
	FString HandleModel(const FString& Args);
	FString HandleLogin(const FString& Args);
	FString HandleLogout(const FString& Args);
	FString HandleListDirs(const FString& Args);
	FString HandleAddDir(const FString& Args);
	FString HandlePlan(const FString& Args);
	FString HandleReview(const FString& Args);
	FString HandleDiff(const FString& Args);
	FString HandleResearch(const FString& Args);
	FString HandleExplain(const FString& Args);
	FString HandleRefactor(const FString& Args);
	FString HandleGenerate(const FString& Args);
	FString HandleCompile(const FString& Args);
	FString HandleLiveCoding(const FString& Args);
	FString HandleTest(const FString& Args);
	FString HandleInit(const FString& Args);
	FString HandleSession(const FString& Args);
	FString HandleCompact(const FString& Args);
	FString HandlePr(const FString& Args);
	FString HandleShare(const FString& Args);
	FString HandleFleet(const FString& Args);
	FString HandleAgent(const FString& Args);
	FString HandleSkills(const FString& Args);
	FString HandleQuest(const FString& Args);
	FString HandleVR(const FString& Args);
	FString HandleOpen(const FString& Args);
	FString HandlePatch(const FString& Args);
	FString HandleRollback(const FString& Args);
	FString HandleBlueprint(const FString& Args);
	FString HandleKnowledge(const FString& Args);

	void Log(const FString& Message);

	TArray<FCopilotSlashCommand> Commands;
	TSharedPtr<FGitHubCopilotUECommandRouter> CommandRouter;
	TSharedPtr<FGitHubCopilotUEBridgeService> BridgeService;
	TSharedPtr<FGitHubCopilotUEContextService> ContextService;
	TSharedPtr<FGitHubCopilotUEFileService> FileService;
};
