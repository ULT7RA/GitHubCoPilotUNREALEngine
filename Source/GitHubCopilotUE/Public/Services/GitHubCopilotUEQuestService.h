// Copyright GitHub, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GitHubCopilotUETypes.h"

/**
 * Service for Meta Quest / OpenXR / VR workflow analysis.
 * Inspects enabled XR plugins, detects Android target relevance,
 * gathers VR-relevant actors, and summarizes Quest readiness.
 */
class GITHUBCOPILOTUE_API FGitHubCopilotUEQuestService
{
public:
	FGitHubCopilotUEQuestService();

	/** Run a full Quest readiness audit */
	FCopilotQuestReadiness RunQuestAudit() const;

	/** Check if OpenXR plugin is enabled */
	bool IsOpenXREnabled() const;

	/** Check if MetaXR / OculusXR plugin is enabled */
	bool IsMetaXREnabled() const;

	/** Detect if Android platform is configured as a target */
	bool IsAndroidConfigured() const;

	/** Get VR-relevant actors in the current level */
	TArray<FString> GetVRRelevantActors() const;

	/** Get all enabled XR-related plugin names */
	TArray<FString> GetXRPlugins() const;

	/** Generate a human-readable readiness summary */
	FString GenerateReadinessSummary(const FCopilotQuestReadiness& Readiness) const;

	/** Delegate for log messages */
	mutable FOnCopilotLogMessage OnLogMessage;

private:
	void Log(const FString& Message) const;
};
