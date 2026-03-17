// Copyright GitHub, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UObject/NoExportTypes.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"
#include "GitHubCopilotUETypes.generated.h"

DECLARE_LOG_CATEGORY_EXTERN(LogGitHubCopilotUE, Log, All);

// ============================================================================
// Enums
// ============================================================================

UENUM(BlueprintType)
enum class ECopilotExecutionMode : uint8
{
	SuggestOnly        UMETA(DisplayName = "Suggest Only"),
	PreviewOnly        UMETA(DisplayName = "Preview Only"),
	ApplyWithApproval  UMETA(DisplayName = "Apply With Approval"),
	ApplyImmediately   UMETA(DisplayName = "Apply Immediately")
};

UENUM(BlueprintType)
enum class ECopilotBackendType : uint8
{
	HTTP       UMETA(DisplayName = "HTTP REST"),
	WebSocket  UMETA(DisplayName = "WebSocket")
};

UENUM(BlueprintType)
enum class ECopilotConnectionStatus : uint8
{
	Disconnected    UMETA(DisplayName = "Disconnected"),
	Connecting      UMETA(DisplayName = "Connecting"),
	Connected       UMETA(DisplayName = "Connected"),
	Error           UMETA(DisplayName = "Error")
};

UENUM(BlueprintType)
enum class ECopilotResultStatus : uint8
{
	Success     UMETA(DisplayName = "Success"),
	Failure     UMETA(DisplayName = "Failure"),
	Pending     UMETA(DisplayName = "Pending"),
	Cancelled   UMETA(DisplayName = "Cancelled"),
	Timeout     UMETA(DisplayName = "Timeout")
};

UENUM(BlueprintType)
enum class ECopilotCommandType : uint8
{
	Ask,
	AnalyzeProject,
	AnalyzeSelection,
	ExplainCode,
	CreateCppClass,
	CreateActorComponent,
	CreateBlueprintFunctionLibrary,
	CreateFile,
	PatchFile,
	InsertIntoFile,
	OpenAsset,
	OpenFile,
	TriggerCompile,
	TriggerLiveCoding,
	RunAutomationTests,
	RunQuestAudit,
	GatherVRContext,
	GatherProjectContext,
	SuggestRefactor,
	GenerateEditorUtilityHelper,
	ApproveAndApplyPatch,
	RollbackPatch,
	CopyResponse,
	Clear
};

// ============================================================================
// Data Structs
// ============================================================================

USTRUCT(BlueprintType)
struct FCopilotProjectContext
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Copilot") FString ProjectName;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Copilot") FString EngineVersion;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Copilot") FString CurrentMapName;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Copilot") TArray<FString> SelectedAssets;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Copilot") TArray<FString> SelectedActors;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Copilot") TArray<FString> EnabledPlugins;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Copilot") TArray<FString> EnabledXRPlugins;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Copilot") FString ActivePlatform;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Copilot") TArray<FString> ProjectSourceDirectories;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Copilot") TArray<FString> ModuleNames;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Copilot") FString QuestReadinessSummary;

	TSharedPtr<FJsonObject> ToJson() const;
	static FCopilotProjectContext FromJson(const TSharedPtr<FJsonObject>& JsonObject);
};

USTRUCT(BlueprintType)
struct FCopilotFileTarget
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Copilot") FString FilePath;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Copilot") int32 LineStart = 0;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Copilot") int32 LineEnd = 0;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Copilot") FString SelectedText;

	TSharedPtr<FJsonObject> ToJson() const;
};

USTRUCT(BlueprintType)
struct FCopilotDiffPreview
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Copilot") FString OriginalFilePath;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Copilot") FString OriginalContent;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Copilot") FString ProposedContent;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Copilot") FString UnifiedDiff;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Copilot") bool bIsValid = false;
};

USTRUCT(BlueprintType)
struct FCopilotRequest
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Copilot") FString RequestId;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Copilot") ECopilotCommandType CommandType = ECopilotCommandType::AnalyzeProject;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Copilot") ECopilotExecutionMode ExecutionMode = ECopilotExecutionMode::SuggestOnly;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Copilot") FString UserPrompt;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Copilot") FCopilotProjectContext ProjectContext;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Copilot") TArray<FCopilotFileTarget> FileTargets;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Copilot") FString Timestamp;
	// Non-UPROPERTY members for flexible command arguments
	TMap<FString, FString> CommandArguments;

	TSharedPtr<FJsonObject> ToJson() const;
};

USTRUCT(BlueprintType)
struct FCopilotResponse
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Copilot") FString RequestId;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Copilot") ECopilotResultStatus ResultStatus = ECopilotResultStatus::Pending;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Copilot") ECopilotCommandType CommandType = ECopilotCommandType::AnalyzeSelection;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Copilot") FString ResponseText;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Copilot") bool bSuccess = false;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Copilot") FString ErrorMessage;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Copilot") FCopilotDiffPreview DiffPreview;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Copilot") FString Timestamp;
	// Provider metadata
	TMap<FString, FString> ProviderMetadata;

	static FCopilotResponse FromJson(const TSharedPtr<FJsonObject>& JsonObject);
};

USTRUCT(BlueprintType)
struct FCopilotQuestReadiness
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Copilot") bool bOpenXRPluginEnabled = false;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Copilot") bool bMetaXRPluginEnabled = false;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Copilot") bool bAndroidPlatformConfigured = false;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Copilot") TArray<FString> XRRelevantPlugins;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Copilot") TArray<FString> VRRelevantActors;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Copilot") FString Summary;
};

// ============================================================================
// Patch step tracking
// ============================================================================

UENUM(BlueprintType)
enum class ECopilotPatchStep : uint8
{
	ValidatingPath,
	ReadingOriginal,
	ValidatingRoots,
	GeneratingDiff,
	CreatingBackup,
	WritingFile,
	VerifyingWrite,
	RollingBack,
	Complete,
	Failed
};

USTRUCT(BlueprintType)
struct FCopilotPatchResult
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Copilot") FString FilePath;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Copilot") FString BackupPath;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Copilot") ECopilotPatchStep LastStep = ECopilotPatchStep::ValidatingPath;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Copilot") bool bSuccess = false;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Copilot") FString ErrorMessage;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Copilot") TArray<FString> StepLog;

	void LogStep(ECopilotPatchStep Step, const FString& Message)
	{
		LastStep = Step;
		StepLog.Add(FString::Printf(TEXT("[%d] %s"), (int32)Step, *Message));
	}

	FString GetStepLogText() const
	{
		return FString::Join(StepLog, TEXT("\n"));
	}
};

DECLARE_MULTICAST_DELEGATE_TwoParams(FOnCopilotPatchStepProgress, const FString& /*FilePath*/, ECopilotPatchStep /*Step*/);

// ============================================================================
// Delegates
// ============================================================================

DECLARE_MULTICAST_DELEGATE_OneParam(FOnCopilotResponseReceived, const FCopilotResponse&);
DECLARE_MULTICAST_DELEGATE_OneParam(FOnCopilotConnectionStatusChanged, ECopilotConnectionStatus);
DECLARE_MULTICAST_DELEGATE_OneParam(FOnCopilotLogMessage, const FString&);

