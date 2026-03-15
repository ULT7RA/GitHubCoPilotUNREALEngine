// Copyright GitHub, Inc. All Rights Reserved.

#include "GitHubCopilotUESettings.h"

UGitHubCopilotUESettings::UGitHubCopilotUESettings()
	: BackendType(ECopilotBackendType::HTTP)
	, EndpointURL(TEXT("http://localhost:8080/api/copilot"))
	, ApiKey(TEXT(""))
	, ModelName(TEXT("gpt-4"))
	, TimeoutSeconds(120)
	, bEnableVerboseLogging(false)
	, bEnableQuestWorkflowAnalysis(true)
	, bEnableOpenXRContextCollection(true)
	, bEnablePatchPreviewRequired(true)
	, bEnableCompileCommands(true)
	, bEnableLiveCodingCommands(true)
	, bEnableBlueprintContextCollection(true)
	, DefaultTargetPlatform(TEXT("Windows"))
{
	AllowedWriteRoots.Add(TEXT("Source/"));
	AllowedWriteRoots.Add(TEXT("Plugins/"));
	AllowedWriteRoots.Add(TEXT("Content/"));
}

const UGitHubCopilotUESettings* UGitHubCopilotUESettings::Get()
{
	return GetDefault<UGitHubCopilotUESettings>();
}
