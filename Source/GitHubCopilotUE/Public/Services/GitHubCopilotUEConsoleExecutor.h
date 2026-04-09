// Copyright GitHub, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

// Forward-declare to avoid coupling to OutputLog internals.
// The actual executor is implemented in the .cpp via IConsoleCommandExecutor.
class FGitHubCopilotUEBridgeService;
class FGitHubCopilotUESlashCommands;
class FGitHubCopilotUEConsoleCommands;
class FGitHubCopilotUECommandRouter;
class FGitHubCopilotUEContextService;

/**
 * Registers a "Copilot" entry in the Output Log console dropdown
 * (alongside "Cmd" and "Python (REPL)").
 *
 * When the user selects "Copilot", everything they type goes to the AI.
 * Slash commands (/help, /model, /knowledge, etc.) still work.
 * No prefix needed — just type naturally.
 */
class GITHUBCOPILOTUE_API FGitHubCopilotUEConsoleExecutor
{
public:
	FGitHubCopilotUEConsoleExecutor();
	~FGitHubCopilotUEConsoleExecutor();

	void Initialize(
		TSharedPtr<FGitHubCopilotUEBridgeService> InBridgeService,
		TSharedPtr<FGitHubCopilotUECommandRouter> InCommandRouter,
		TSharedPtr<FGitHubCopilotUEContextService> InContextService,
		TSharedPtr<FGitHubCopilotUESlashCommands> InSlashCommands,
		TSharedPtr<FGitHubCopilotUEConsoleCommands> InConsoleCommands
	);

	void Shutdown();

private:
	// The actual IConsoleCommandExecutor implementation lives in the .cpp
	// to avoid leaking the OutputLog header into this public header.
	class FExecutorImpl;
	TSharedPtr<FExecutorImpl> Impl;
	bool bRegistered = false;
};
