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
	FString Tool_ReadFile(const TSharedPtr<FJsonObject>& Args);
	FString Tool_WriteFile(const TSharedPtr<FJsonObject>& Args);
	FString Tool_EditFile(const TSharedPtr<FJsonObject>& Args);
	FString Tool_ListDirectory(const TSharedPtr<FJsonObject>& Args);
	FString Tool_SearchFiles(const TSharedPtr<FJsonObject>& Args);
	FString Tool_GetProjectStructure(const TSharedPtr<FJsonObject>& Args);
	FString Tool_CreateCppClass(const TSharedPtr<FJsonObject>& Args);
	FString Tool_Compile(const TSharedPtr<FJsonObject>& Args);
	FString Tool_GetFileInfo(const TSharedPtr<FJsonObject>& Args);
	FString Tool_DeleteFile(const TSharedPtr<FJsonObject>& Args);

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
