// Copyright GitHub, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

class FGitHubCopilotUEFileService;
class FGitHubCopilotUEContextService;
class FGitHubCopilotUECompileService;

/**
 * Executes tool calls from the AI model.
 * This is what makes the AI actually work inside Unreal Engine —
 * it can read files, write files, edit code, list directories, search,
 * compile, etc. Just like a human developer would.
 */
class GITHUBCOPILOTUE_API FGitHubCopilotUEToolExecutor
{
public:
	FGitHubCopilotUEToolExecutor();

	void Initialize(
		TSharedPtr<FGitHubCopilotUEFileService> InFileService,
		TSharedPtr<FGitHubCopilotUEContextService> InContextService,
		TSharedPtr<FGitHubCopilotUECompileService> InCompileService
	);

	/** Execute a single tool call. Returns the result string. */
	FString ExecuteTool(const FString& ToolName, const TSharedPtr<FJsonObject>& Arguments);

	/** Build the tools array for the chat completion request. */
	static TArray<TSharedPtr<FJsonValue>> BuildToolDefinitions();

private:
	// Individual tool implementations
	FString Tool_ViewPath(const TSharedPtr<FJsonObject>& Args);
	FString Tool_GlobFiles(const TSharedPtr<FJsonObject>& Args);
	FString Tool_ReadFile(const TSharedPtr<FJsonObject>& Args);
	FString Tool_WriteFile(const TSharedPtr<FJsonObject>& Args);
	FString Tool_EditFile(const TSharedPtr<FJsonObject>& Args);
	FString Tool_ListDirectory(const TSharedPtr<FJsonObject>& Args);
	FString Tool_SearchFiles(const TSharedPtr<FJsonObject>& Args);
	FString Tool_CreateDirectory(const TSharedPtr<FJsonObject>& Args);
	FString Tool_CopyFile(const TSharedPtr<FJsonObject>& Args);
	FString Tool_MoveFile(const TSharedPtr<FJsonObject>& Args);
	FString Tool_GetProjectStructure(const TSharedPtr<FJsonObject>& Args);
	FString Tool_CreateCppClass(const TSharedPtr<FJsonObject>& Args);
	FString Tool_CreateBlueprintAsset(const TSharedPtr<FJsonObject>& Args);
	FString Tool_Compile(const TSharedPtr<FJsonObject>& Args);
	FString Tool_LiveCodingPatch(const TSharedPtr<FJsonObject>& Args);
	FString Tool_RunAutomationTests(const TSharedPtr<FJsonObject>& Args);
	FString Tool_GetFileInfo(const TSharedPtr<FJsonObject>& Args);
	FString Tool_DeleteFile(const TSharedPtr<FJsonObject>& Args);
	FString Tool_SpawnActor(const TSharedPtr<FJsonObject>& Args);
	FString Tool_CreateMaterialAsset(const TSharedPtr<FJsonObject>& Args);
	FString Tool_CreateDataTable(const TSharedPtr<FJsonObject>& Args);
	FString Tool_CreateNiagaraSystem(const TSharedPtr<FJsonObject>& Args);
	FString Tool_WebSearch(const TSharedPtr<FJsonObject>& Args);
	FString Tool_CaptureViewport(const TSharedPtr<FJsonObject>& Args);

	// Tier 1 — core autonomy tools
	FString Tool_ExecutePython(const TSharedPtr<FJsonObject>& Args);
	FString Tool_ExecuteConsoleCommand(const TSharedPtr<FJsonObject>& Args);
	FString Tool_ExecuteShell(const TSharedPtr<FJsonObject>& Args);
	FString Tool_PlayInEditor(const TSharedPtr<FJsonObject>& Args);
	FString Tool_StopPIE(const TSharedPtr<FJsonObject>& Args);
	FString Tool_PackageProject(const TSharedPtr<FJsonObject>& Args);

	// Tier 2 — scene & asset tools
	FString Tool_ListActors(const TSharedPtr<FJsonObject>& Args);
	FString Tool_GetActorProperties(const TSharedPtr<FJsonObject>& Args);
	FString Tool_SetActorProperties(const TSharedPtr<FJsonObject>& Args);
	FString Tool_DeleteActors(const TSharedPtr<FJsonObject>& Args);
	FString Tool_ImportAsset(const TSharedPtr<FJsonObject>& Args);
	FString Tool_OpenLevel(const TSharedPtr<FJsonObject>& Args);
	FString Tool_RenameAsset(const TSharedPtr<FJsonObject>& Args);
	FString Tool_GetSelectedActors(const TSharedPtr<FJsonObject>& Args);

	// Tier 3 — specialized tools
	FString Tool_RetargetAnimations(const TSharedPtr<FJsonObject>& Args);
	FString Tool_CreateAnimMontage(const TSharedPtr<FJsonObject>& Args);
	FString Tool_CreateLevelSequence(const TSharedPtr<FJsonObject>& Args);
	FString Tool_BuildLighting(const TSharedPtr<FJsonObject>& Args);
	FString Tool_GetProjectSettings(const TSharedPtr<FJsonObject>& Args);
	FString Tool_SetProjectSettings(const TSharedPtr<FJsonObject>& Args);
	FString Tool_CreateWidgetBlueprint(const TSharedPtr<FJsonObject>& Args);
	FString Tool_GetBlueprintGraph(const TSharedPtr<FJsonObject>& Args);
	FString Tool_AddBlueprintNode(const TSharedPtr<FJsonObject>& Args);
	FString Tool_GitCommit(const TSharedPtr<FJsonObject>& Args);
	FString Tool_GitPush(const TSharedPtr<FJsonObject>& Args);

	/** Resolve a path — if relative, treat it as relative to project root */
	FString ResolvePath(const FString& InputPath) const;

	/** Check if a path is within allowed write roots */
	bool IsPathAllowed(const FString& FullPath) const;

	TSharedPtr<FGitHubCopilotUEFileService> FileService;
	TSharedPtr<FGitHubCopilotUEContextService> ContextService;
	TSharedPtr<FGitHubCopilotUECompileService> CompileService;

	/** Helper to build a single tool definition JSON */
	static TSharedPtr<FJsonValue> MakeToolDef(
		const FString& Name,
		const FString& Description,
		const TSharedPtr<FJsonObject>& Parameters);
};
