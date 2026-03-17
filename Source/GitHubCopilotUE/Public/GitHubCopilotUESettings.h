// Copyright GitHub, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "Services/GitHubCopilotUETypes.h"
#include "GitHubCopilotUESettings.generated.h"

/**
 * Settings for the GitHub Copilot UE plugin.
 * Accessible from Edit -> Project Settings -> Plugins -> GitHub Copilot UE
 */
UCLASS(config = Editor, defaultconfig, meta = (DisplayName = "GitHub Copilot UE"))
class GITHUBCOPILOTUE_API UGitHubCopilotUESettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	UGitHubCopilotUESettings();

	/** Backend connection type */
	UPROPERTY(config, EditAnywhere, Category = "Connection", meta = (DisplayName = "Backend Type"))
	ECopilotBackendType BackendType;

	/** Endpoint URL for the AI backend service */
	UPROPERTY(config, EditAnywhere, Category = "Connection", meta = (DisplayName = "Endpoint URL"))
	FString EndpointURL;

	/** API key or token for authentication */
	UPROPERTY(config, EditAnywhere, Category = "Connection", meta = (DisplayName = "API Key"))
	FString ApiKey;

	/** Model name to request from the backend */
	UPROPERTY(config, EditAnywhere, Category = "Connection", meta = (DisplayName = "Model Name"))
	FString ModelName;

	/** Display handle used in the panel chat transcript */
	UPROPERTY(config, EditAnywhere, Category = "Chat", meta = (DisplayName = "User Handle"))
	FString UserHandle;

	/** Request timeout in seconds */
	UPROPERTY(config, EditAnywhere, Category = "Connection", meta = (DisplayName = "Timeout (Seconds)", ClampMin = "5", ClampMax = "300"))
	int32 TimeoutSeconds;

	/** Max tool-call loop iterations per request (0 = unlimited) */
	UPROPERTY(config, EditAnywhere, Category = "Execution", meta = (DisplayName = "Max Tool-Call Iterations (0 = Unlimited)", ClampMin = "0", ClampMax = "5000"))
	int32 MaxToolCallIterations;

	/** Enable verbose logging for debugging */
	UPROPERTY(config, EditAnywhere, Category = "Logging", meta = (DisplayName = "Enable Verbose Logging"))
	bool bEnableVerboseLogging;

	/** Enable Quest workflow analysis features */
	UPROPERTY(config, EditAnywhere, Category = "XR / Quest", meta = (DisplayName = "Enable Quest Workflow Analysis"))
	bool bEnableQuestWorkflowAnalysis;

	/** Enable OpenXR context collection */
	UPROPERTY(config, EditAnywhere, Category = "XR / Quest", meta = (DisplayName = "Enable OpenXR Context Collection"))
	bool bEnableOpenXRContextCollection;

	/** Require diff preview before applying patches */
	UPROPERTY(config, EditAnywhere, Category = "Execution", meta = (DisplayName = "Require Patch Preview"))
	bool bEnablePatchPreviewRequired;

	/** Allowed write root directories (relative to project). Files outside these roots will be rejected. */
	UPROPERTY(config, EditAnywhere, Category = "Safety", meta = (DisplayName = "Allowed Write Roots"))
	TArray<FString> AllowedWriteRoots;

	/** Enable compile commands from the plugin */
	UPROPERTY(config, EditAnywhere, Category = "Compile", meta = (DisplayName = "Enable Compile Commands"))
	bool bEnableCompileCommands;

	/** Enable Live Coding commands from the plugin */
	UPROPERTY(config, EditAnywhere, Category = "Compile", meta = (DisplayName = "Enable Live Coding Commands"))
	bool bEnableLiveCodingCommands;

	/** Enable Blueprint context collection */
	UPROPERTY(config, EditAnywhere, Category = "Context", meta = (DisplayName = "Enable Blueprint Context Collection"))
	bool bEnableBlueprintContextCollection;

	/** Default target platform hint */
	UPROPERTY(config, EditAnywhere, Category = "Context", meta = (DisplayName = "Default Target Platform"))
	FString DefaultTargetPlatform;

	// UDeveloperSettings interface
	virtual FName GetCategoryName() const override { return FName(TEXT("Plugins")); }
	virtual FName GetSectionName() const override { return FName(TEXT("GitHub Copilot UE")); }

#if WITH_EDITOR
	virtual FText GetSectionText() const override { return NSLOCTEXT("GitHubCopilotUE", "SettingsSection", "GitHub Copilot UE"); }
	virtual FText GetSectionDescription() const override { return NSLOCTEXT("GitHubCopilotUE", "SettingsDesc", "Configure the GitHub Copilot UE AI assistant plugin."); }
#endif

	/** Get the singleton settings object */
	static const UGitHubCopilotUESettings* Get();
};
