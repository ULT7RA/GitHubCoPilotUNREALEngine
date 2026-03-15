// Copyright GitHub, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GitHubCopilotUETypes.h"

/**
 * Service for triggering compile, Live Coding, and automation test workflows
 * from within the plugin. Isolates engine-version-sensitive compile hooks.
 */
class GITHUBCOPILOTUE_API FGitHubCopilotUECompileService
{
public:
	FGitHubCopilotUECompileService();

	/** Request a full editor recompile. Returns result info. */
	FCopilotResponse RequestCompile();

	/** Request a Live Coding patch (if available). Returns result info. */
	FCopilotResponse RequestLiveCodingPatch();

	/** Run automation tests matching a filter string. */
	FCopilotResponse RunAutomationTests(const FString& TestFilter);

	/** Check if Live Coding is available in this engine build */
	bool IsLiveCodingAvailable() const;

	/** Check if compile is currently in progress */
	bool IsCompiling() const;

	/** Delegate for log messages */
	FOnCopilotLogMessage OnLogMessage;

private:
	void Log(const FString& Message);
};
