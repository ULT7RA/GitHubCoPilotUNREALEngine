// Copyright GitHub, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GitHubCopilotUETypes.h"

/**
 * Service that gathers Unreal Editor context: project info, selected assets/actors,
 * enabled plugins, map name, platform hints, and Blueprint info.
 */
class GITHUBCOPILOTUE_API FGitHubCopilotUEContextService
{
public:
	FGitHubCopilotUEContextService();

	/** Gather full project context snapshot */
	FCopilotProjectContext GatherProjectContext() const;

	/** Get current project name */
	FString GetProjectName() const;

	/** Get engine version string */
	FString GetEngineVersion() const;

	/** Get the currently loaded map/level name */
	FString GetCurrentMapName() const;

	/** Get currently selected Content Browser assets as path strings */
	TArray<FString> GetSelectedAssets() const;

	/** Get currently selected Level Editor actors as name strings */
	TArray<FString> GetSelectedActors() const;

	/** Get list of enabled plugin names */
	TArray<FString> GetEnabledPlugins() const;

	/** Get list of enabled XR-related plugins */
	TArray<FString> GetEnabledXRPlugins() const;

	/** Get project source directories */
	TArray<FString> GetProjectSourceDirectories() const;

	/** Get module names from the project's .uproject */
	TArray<FString> GetModuleNames() const;

	/** Get active/default platform hint */
	FString GetActivePlatform() const;

	/** Get selected Blueprint asset names (if bEnableBlueprintContextCollection) */
	TArray<FString> GetSelectedBlueprintAssets() const;

	/** Get recent output log lines (best-effort, limited count) */
	TArray<FString> GetRecentOutputLogLines(int32 MaxLines = 20) const;
};
