// Copyright GitHub, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GitHubCopilotUETypes.h"

class FGitHubCopilotUEContextService;
class FGitHubCopilotUEFileService;
class FGitHubCopilotUEPatchService;
class FGitHubCopilotUEBridgeService;
class FGitHubCopilotUECompileService;
class FGitHubCopilotUEQuestService;

/**
 * Central command router. Receives commands from the UI, validates them,
 * dispatches to appropriate services, and routes results back.
 */
class GITHUBCOPILOTUE_API FGitHubCopilotUECommandRouter
{
public:
	FGitHubCopilotUECommandRouter();

	/** Initialize with service references */
	void Initialize(
		TSharedPtr<FGitHubCopilotUEContextService> InContextService,
		TSharedPtr<FGitHubCopilotUEFileService> InFileService,
		TSharedPtr<FGitHubCopilotUEPatchService> InPatchService,
		TSharedPtr<FGitHubCopilotUEBridgeService> InBridgeService,
		TSharedPtr<FGitHubCopilotUECompileService> InCompileService,
		TSharedPtr<FGitHubCopilotUEQuestService> InQuestService
	);

	/** Route a command request. Returns a request ID for tracking. */
	FString RouteCommand(const FCopilotRequest& Request);

	/** Execute a command locally (for commands that don't need the backend) */
	FCopilotResponse ExecuteLocal(const FCopilotRequest& Request);

	/** Check if a command type requires the backend */
	bool RequiresBackend(ECopilotCommandType CommandType) const;

	/** Validate a request before routing */
	bool ValidateRequest(const FCopilotRequest& Request, FString& OutError) const;

	/** Generate a unique request ID */
	static FString GenerateRequestId();

	/** Delegate for when a response is ready */
	FOnCopilotResponseReceived OnResponseReceived;

	/** Delegate for log messages */
	FOnCopilotLogMessage OnLogMessage;

private:
	/** Handle local commands that don't need the backend */
	FCopilotResponse HandleGatherProjectContext(const FCopilotRequest& Request);
	FCopilotResponse HandleGatherVRContext(const FCopilotRequest& Request);
	FCopilotResponse HandleCreateCppClass(const FCopilotRequest& Request);
	FCopilotResponse HandleCreateActorComponent(const FCopilotRequest& Request);
	FCopilotResponse HandleCreateBlueprintFunctionLibrary(const FCopilotRequest& Request);
	FCopilotResponse HandlePatchFile(const FCopilotRequest& Request);
	FCopilotResponse HandleInsertIntoFile(const FCopilotRequest& Request);
	FCopilotResponse HandleOpenAsset(const FCopilotRequest& Request);
	FCopilotResponse HandleOpenFile(const FCopilotRequest& Request);
	FCopilotResponse HandleTriggerCompile(const FCopilotRequest& Request);
	FCopilotResponse HandleTriggerLiveCoding(const FCopilotRequest& Request);
	FCopilotResponse HandleRunAutomationTests(const FCopilotRequest& Request);
	FCopilotResponse HandleRunQuestAudit(const FCopilotRequest& Request);
	FCopilotResponse HandleCreateFile(const FCopilotRequest& Request);
	FCopilotResponse HandleApproveAndApplyPatch(const FCopilotRequest& Request);
	FCopilotResponse HandleRollbackPatch(const FCopilotRequest& Request);

	/** Handle backend-dependent commands */
	void HandleBackendCommand(const FCopilotRequest& Request);

	void Log(const FString& Message);

	TSharedPtr<FGitHubCopilotUEContextService> ContextService;
	TSharedPtr<FGitHubCopilotUEFileService> FileService;
	TSharedPtr<FGitHubCopilotUEPatchService> PatchService;
	TSharedPtr<FGitHubCopilotUEBridgeService> BridgeService;
	TSharedPtr<FGitHubCopilotUECompileService> CompileService;
	TSharedPtr<FGitHubCopilotUEQuestService> QuestService;
};
