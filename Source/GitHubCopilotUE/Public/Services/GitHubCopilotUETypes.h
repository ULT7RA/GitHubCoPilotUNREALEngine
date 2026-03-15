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

	UPROPERTY(EditAnywhere, BlueprintReadWrite) FString ProjectName;
	UPROPERTY(EditAnywhere, BlueprintReadWrite) FString EngineVersion;
	UPROPERTY(EditAnywhere, BlueprintReadWrite) FString CurrentMapName;
	UPROPERTY(EditAnywhere, BlueprintReadWrite) TArray<FString> SelectedAssets;
	UPROPERTY(EditAnywhere, BlueprintReadWrite) TArray<FString> SelectedActors;
	UPROPERTY(EditAnywhere, BlueprintReadWrite) TArray<FString> EnabledPlugins;
	UPROPERTY(EditAnywhere, BlueprintReadWrite) TArray<FString> EnabledXRPlugins;
	UPROPERTY(EditAnywhere, BlueprintReadWrite) FString ActivePlatform;
	UPROPERTY(EditAnywhere, BlueprintReadWrite) TArray<FString> ProjectSourceDirectories;
	UPROPERTY(EditAnywhere, BlueprintReadWrite) TArray<FString> ModuleNames;
	UPROPERTY(EditAnywhere, BlueprintReadWrite) FString QuestReadinessSummary;

	TSharedPtr<FJsonObject> ToJson() const;
	static FCopilotProjectContext FromJson(const TSharedPtr<FJsonObject>& JsonObject);
};

USTRUCT(BlueprintType)
struct FCopilotFileTarget
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite) FString FilePath;
	UPROPERTY(EditAnywhere, BlueprintReadWrite) int32 LineStart = 0;
	UPROPERTY(EditAnywhere, BlueprintReadWrite) int32 LineEnd = 0;
	UPROPERTY(EditAnywhere, BlueprintReadWrite) FString SelectedText;

	TSharedPtr<FJsonObject> ToJson() const;
};

USTRUCT(BlueprintType)
struct FCopilotDiffPreview
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite) FString OriginalFilePath;
	UPROPERTY(EditAnywhere, BlueprintReadWrite) FString OriginalContent;
	UPROPERTY(EditAnywhere, BlueprintReadWrite) FString ProposedContent;
	UPROPERTY(EditAnywhere, BlueprintReadWrite) FString UnifiedDiff;
	UPROPERTY(EditAnywhere, BlueprintReadWrite) bool bIsValid = false;
};

USTRUCT(BlueprintType)
struct FCopilotRequest
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite) FString RequestId;
	UPROPERTY(EditAnywhere, BlueprintReadWrite) ECopilotCommandType CommandType = ECopilotCommandType::AnalyzeProject;
	UPROPERTY(EditAnywhere, BlueprintReadWrite) ECopilotExecutionMode ExecutionMode = ECopilotExecutionMode::SuggestOnly;
	UPROPERTY(EditAnywhere, BlueprintReadWrite) FString UserPrompt;
	UPROPERTY(EditAnywhere, BlueprintReadWrite) FCopilotProjectContext ProjectContext;
	UPROPERTY(EditAnywhere, BlueprintReadWrite) TArray<FCopilotFileTarget> FileTargets;
	UPROPERTY(EditAnywhere, BlueprintReadWrite) FString Timestamp;
	// Non-UPROPERTY members for flexible command arguments
	TMap<FString, FString> CommandArguments;

	TSharedPtr<FJsonObject> ToJson() const;
};

USTRUCT(BlueprintType)
struct FCopilotResponse
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite) FString RequestId;
	UPROPERTY(EditAnywhere, BlueprintReadWrite) ECopilotResultStatus ResultStatus = ECopilotResultStatus::Pending;
	UPROPERTY(EditAnywhere, BlueprintReadWrite) ECopilotCommandType CommandType = ECopilotCommandType::AnalyzeSelection;
	UPROPERTY(EditAnywhere, BlueprintReadWrite) FString ResponseText;
	UPROPERTY(EditAnywhere, BlueprintReadWrite) bool bSuccess = false;
	UPROPERTY(EditAnywhere, BlueprintReadWrite) FString ErrorMessage;
	UPROPERTY(EditAnywhere, BlueprintReadWrite) FCopilotDiffPreview DiffPreview;
	UPROPERTY(EditAnywhere, BlueprintReadWrite) FString Timestamp;
	// Provider metadata
	TMap<FString, FString> ProviderMetadata;

	static FCopilotResponse FromJson(const TSharedPtr<FJsonObject>& JsonObject);
};

USTRUCT(BlueprintType)
struct FCopilotQuestReadiness
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite) bool bOpenXRPluginEnabled = false;
	UPROPERTY(EditAnywhere, BlueprintReadWrite) bool bMetaXRPluginEnabled = false;
	UPROPERTY(EditAnywhere, BlueprintReadWrite) bool bAndroidPlatformConfigured = false;
	UPROPERTY(EditAnywhere, BlueprintReadWrite) TArray<FString> XRRelevantPlugins;
	UPROPERTY(EditAnywhere, BlueprintReadWrite) TArray<FString> VRRelevantActors;
	UPROPERTY(EditAnywhere, BlueprintReadWrite) FString Summary;
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

	UPROPERTY(EditAnywhere, BlueprintReadWrite) FString FilePath;
	UPROPERTY(EditAnywhere, BlueprintReadWrite) FString BackupPath;
	UPROPERTY(EditAnywhere, BlueprintReadWrite) ECopilotPatchStep LastStep = ECopilotPatchStep::ValidatingPath;
	UPROPERTY(EditAnywhere, BlueprintReadWrite) bool bSuccess = false;
	UPROPERTY(EditAnywhere, BlueprintReadWrite) FString ErrorMessage;
	UPROPERTY(EditAnywhere, BlueprintReadWrite) TArray<FString> StepLog;

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
