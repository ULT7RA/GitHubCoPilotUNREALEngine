// Copyright GitHub, Inc. All Rights Reserved.

#include "Services/GitHubCopilotUECommandRouter.h"
#include "Services/GitHubCopilotUEContextService.h"
#include "Services/GitHubCopilotUEFileService.h"
#include "Services/GitHubCopilotUEPatchService.h"
#include "Services/GitHubCopilotUEBridgeService.h"
#include "Services/GitHubCopilotUECompileService.h"
#include "Services/GitHubCopilotUEQuestService.h"
#include "GitHubCopilotUESettings.h"
#include "Misc/Guid.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/App.h"
#include "Editor.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "SourceCodeNavigation.h"
#include "UObject/SoftObjectPath.h"

FGitHubCopilotUECommandRouter::FGitHubCopilotUECommandRouter()
{
}

void FGitHubCopilotUECommandRouter::Initialize(
	TSharedPtr<FGitHubCopilotUEContextService> InContextService,
	TSharedPtr<FGitHubCopilotUEFileService> InFileService,
	TSharedPtr<FGitHubCopilotUEPatchService> InPatchService,
	TSharedPtr<FGitHubCopilotUEBridgeService> InBridgeService,
	TSharedPtr<FGitHubCopilotUECompileService> InCompileService,
	TSharedPtr<FGitHubCopilotUEQuestService> InQuestService)
{
	ContextService = InContextService;
	FileService = InFileService;
	PatchService = InPatchService;
	BridgeService = InBridgeService;
	CompileService = InCompileService;
	QuestService = InQuestService;

	// Wire up bridge response to our delegate
	if (BridgeService.IsValid())
	{
		BridgeService->OnResponseReceived.AddLambda([this](const FCopilotResponse& Response)
		{
			OnResponseReceived.Broadcast(Response);
		});
	}

	Log(TEXT("CommandRouter: Initialized with all services"));
}

FString FGitHubCopilotUECommandRouter::GenerateRequestId()
{
	return FGuid::NewGuid().ToString(EGuidFormats::Short);
}

bool FGitHubCopilotUECommandRouter::ValidateRequest(const FCopilotRequest& Request, FString& OutError) const
{
	if (Request.RequestId.IsEmpty())
	{
		OutError = TEXT("Request ID is empty");
		return false;
	}

	// Validate command-specific requirements
	switch (Request.CommandType)
	{
	case ECopilotCommandType::PatchFile:
	case ECopilotCommandType::InsertIntoFile:
	case ECopilotCommandType::CreateFile:
		if (Request.FileTargets.Num() == 0)
		{
			OutError = TEXT("File operation requires at least one file target");
			return false;
		}
		break;
	case ECopilotCommandType::OpenAsset:
		if (Request.CommandArguments.Num() == 0 && Request.UserPrompt.IsEmpty())
		{
			OutError = TEXT("OpenAsset requires an asset path");
			return false;
		}
		break;
	default:
		break;
	}

	return true;
}

bool FGitHubCopilotUECommandRouter::RequiresBackend(ECopilotCommandType CommandType) const
{
	switch (CommandType)
	{
	// These commands are handled locally
	case ECopilotCommandType::GatherProjectContext:
	case ECopilotCommandType::GatherVRContext:
	case ECopilotCommandType::TriggerCompile:
	case ECopilotCommandType::TriggerLiveCoding:
	case ECopilotCommandType::RunAutomationTests:
	case ECopilotCommandType::RunQuestAudit:
	case ECopilotCommandType::OpenAsset:
	case ECopilotCommandType::OpenFile:
	case ECopilotCommandType::CopyResponse:
	case ECopilotCommandType::Clear:
		return false;

	// These may create files locally if content provided, or need backend for generation
	case ECopilotCommandType::CreateCppClass:
	case ECopilotCommandType::CreateActorComponent:
	case ECopilotCommandType::CreateBlueprintFunctionLibrary:
	case ECopilotCommandType::CreateFile:
	case ECopilotCommandType::PatchFile:
	case ECopilotCommandType::InsertIntoFile:
	case ECopilotCommandType::ApproveAndApplyPatch:
	case ECopilotCommandType::RollbackPatch:
		return false; // Local execution with template generation

	// These always need the AI backend
	case ECopilotCommandType::AnalyzeProject:
	case ECopilotCommandType::AnalyzeSelection:
	case ECopilotCommandType::ExplainCode:
	case ECopilotCommandType::SuggestRefactor:
	case ECopilotCommandType::GenerateEditorUtilityHelper:
		return true;

	default:
		return true;
	}
}

FString FGitHubCopilotUECommandRouter::RouteCommand(const FCopilotRequest& Request)
{
	FString Error;
	if (!ValidateRequest(Request, Error))
	{
		FCopilotResponse ErrorResponse;
		ErrorResponse.RequestId = Request.RequestId;
		ErrorResponse.ResultStatus = ECopilotResultStatus::Failure;
		ErrorResponse.ErrorMessage = Error;
		Log(FString::Printf(TEXT("CommandRouter: Validation failed for %s: %s"), *Request.RequestId, *Error));
		OnResponseReceived.Broadcast(ErrorResponse);
		return Request.RequestId;
	}

	Log(FString::Printf(TEXT("CommandRouter: Routing command %d (ID: %s)"), (int32)Request.CommandType, *Request.RequestId));

	if (RequiresBackend(Request.CommandType))
	{
		HandleBackendCommand(Request);
	}
	else
	{
		FCopilotResponse Response = ExecuteLocal(Request);
		OnResponseReceived.Broadcast(Response);
	}

	return Request.RequestId;
}

FCopilotResponse FGitHubCopilotUECommandRouter::ExecuteLocal(const FCopilotRequest& Request)
{
	switch (Request.CommandType)
	{
	case ECopilotCommandType::GatherProjectContext:
		return HandleGatherProjectContext(Request);
	case ECopilotCommandType::GatherVRContext:
		return HandleGatherVRContext(Request);
	case ECopilotCommandType::CreateCppClass:
		return HandleCreateCppClass(Request);
	case ECopilotCommandType::CreateActorComponent:
		return HandleCreateActorComponent(Request);
	case ECopilotCommandType::CreateBlueprintFunctionLibrary:
		return HandleCreateBlueprintFunctionLibrary(Request);
	case ECopilotCommandType::CreateFile:
		return HandleCreateFile(Request);
	case ECopilotCommandType::PatchFile:
		return HandlePatchFile(Request);
	case ECopilotCommandType::InsertIntoFile:
		return HandleInsertIntoFile(Request);
	case ECopilotCommandType::OpenAsset:
		return HandleOpenAsset(Request);
	case ECopilotCommandType::OpenFile:
		return HandleOpenFile(Request);
	case ECopilotCommandType::TriggerCompile:
		return HandleTriggerCompile(Request);
	case ECopilotCommandType::TriggerLiveCoding:
		return HandleTriggerLiveCoding(Request);
	case ECopilotCommandType::RunAutomationTests:
		return HandleRunAutomationTests(Request);
	case ECopilotCommandType::RunQuestAudit:
		return HandleRunQuestAudit(Request);
	case ECopilotCommandType::ApproveAndApplyPatch:
		return HandleApproveAndApplyPatch(Request);
	case ECopilotCommandType::RollbackPatch:
		return HandleRollbackPatch(Request);
	default:
	{
		FCopilotResponse Response;
		Response.RequestId = Request.RequestId;
		Response.ResultStatus = ECopilotResultStatus::Failure;
		Response.ErrorMessage = TEXT("Unknown local command type");
		return Response;
	}
	}
}

void FGitHubCopilotUECommandRouter::HandleBackendCommand(const FCopilotRequest& Request)
{
	if (!BridgeService.IsValid())
	{
		FCopilotResponse Response;
		Response.RequestId = Request.RequestId;
		Response.ResultStatus = ECopilotResultStatus::Failure;
		Response.ErrorMessage = TEXT("Backend bridge service not available");
		OnResponseReceived.Broadcast(Response);
		return;
	}

	if (BridgeService->GetConnectionStatus() != ECopilotConnectionStatus::Connected)
	{
		Log(TEXT("CommandRouter: Backend not connected, attempting connection..."));
		BridgeService->Connect();
	}

	// Enrich request with project context
	FCopilotRequest EnrichedRequest = Request;
	if (ContextService.IsValid())
	{
		EnrichedRequest.ProjectContext = ContextService->GatherProjectContext();
	}
	EnrichedRequest.Timestamp = FDateTime::Now().ToString();

	BridgeService->SendRequest(EnrichedRequest);
}

// ============================================================================
// Local command handlers
// ============================================================================

FCopilotResponse FGitHubCopilotUECommandRouter::HandleGatherProjectContext(const FCopilotRequest& Request)
{
	FCopilotResponse Response;
	Response.RequestId = Request.RequestId;
	Response.Timestamp = FDateTime::Now().ToString();

	if (!ContextService.IsValid())
	{
		Response.ResultStatus = ECopilotResultStatus::Failure;
		Response.ErrorMessage = TEXT("Context service not available");
		return Response;
	}

	FCopilotProjectContext Ctx = ContextService->GatherProjectContext();

	FString ContextText;
	ContextText += FString::Printf(TEXT("Project: %s\n"), *Ctx.ProjectName);
	ContextText += FString::Printf(TEXT("Engine: %s\n"), *Ctx.EngineVersion);
	ContextText += FString::Printf(TEXT("Map: %s\n"), *Ctx.CurrentMapName);
	ContextText += FString::Printf(TEXT("Platform: %s\n"), *Ctx.ActivePlatform);
	ContextText += FString::Printf(TEXT("Selected Assets: %d\n"), Ctx.SelectedAssets.Num());
	for (const FString& Asset : Ctx.SelectedAssets)
	{
		ContextText += FString::Printf(TEXT("  - %s\n"), *Asset);
	}
	ContextText += FString::Printf(TEXT("Selected Actors: %d\n"), Ctx.SelectedActors.Num());
	for (const FString& Actor : Ctx.SelectedActors)
	{
		ContextText += FString::Printf(TEXT("  - %s\n"), *Actor);
	}
	ContextText += FString::Printf(TEXT("Enabled Plugins: %d\n"), Ctx.EnabledPlugins.Num());
	ContextText += FString::Printf(TEXT("XR Plugins: %d\n"), Ctx.EnabledXRPlugins.Num());
	ContextText += FString::Printf(TEXT("Source Dirs: %d\n"), Ctx.ProjectSourceDirectories.Num());
	ContextText += FString::Printf(TEXT("Modules: %d\n"), Ctx.ModuleNames.Num());
	for (const FString& Module : Ctx.ModuleNames)
	{
		ContextText += FString::Printf(TEXT("  - %s\n"), *Module);
	}

	Response.ResultStatus = ECopilotResultStatus::Success;
	Response.ResponseText = ContextText;
	return Response;
}

FCopilotResponse FGitHubCopilotUECommandRouter::HandleGatherVRContext(const FCopilotRequest& Request)
{
	FCopilotResponse Response;
	Response.RequestId = Request.RequestId;
	Response.Timestamp = FDateTime::Now().ToString();

	if (!QuestService.IsValid())
	{
		Response.ResultStatus = ECopilotResultStatus::Failure;
		Response.ErrorMessage = TEXT("Quest service not available");
		return Response;
	}

	FCopilotQuestReadiness Readiness = QuestService->RunQuestAudit();
	Response.ResultStatus = ECopilotResultStatus::Success;
	Response.ResponseText = Readiness.Summary;
	return Response;
}

FCopilotResponse FGitHubCopilotUECommandRouter::HandleCreateCppClass(const FCopilotRequest& Request)
{
	FCopilotResponse Response;
	Response.RequestId = Request.RequestId;
	Response.Timestamp = FDateTime::Now().ToString();

	if (!FileService.IsValid())
	{
		Response.ResultStatus = ECopilotResultStatus::Failure;
		Response.ErrorMessage = TEXT("File service not available");
		return Response;
	}

	// Extract class name from arguments or prompt
	FString ClassName;
	const FString* ClassNameArg = Request.CommandArguments.Find(TEXT("ClassName"));
	if (ClassNameArg)
	{
		ClassName = *ClassNameArg;
	}
	else if (!Request.UserPrompt.IsEmpty())
	{
		ClassName = Request.UserPrompt;
		ClassName.ReplaceInline(TEXT(" "), TEXT(""));
	}

	if (ClassName.IsEmpty())
	{
		Response.ResultStatus = ECopilotResultStatus::Failure;
		Response.ErrorMessage = TEXT("ClassName not specified");
		return Response;
	}

	// Determine parent class
	FString ParentClass = TEXT("AActor");
	const FString* ParentArg = Request.CommandArguments.Find(TEXT("ParentClass"));
	if (ParentArg) ParentClass = *ParentArg;

	// Get module/path
	FString ModuleName;
	const FString* ModuleArg = Request.CommandArguments.Find(TEXT("ModuleName"));
	if (ModuleArg)
	{
		ModuleName = *ModuleArg;
	}
	else
	{
		// Use project name as default module
		ModuleName = FApp::GetProjectName();
	}

	FString SourceDir = FString::Printf(TEXT("Source/%s"), *ModuleName);
	FString HeaderPath = FString::Printf(TEXT("%s/Public/%s.h"), *SourceDir, *ClassName);
	FString CppPath = FString::Printf(TEXT("%s/Private/%s.cpp"), *SourceDir, *ClassName);

	// Determine API macro
	FString APIMacro = ModuleName.ToUpper() + TEXT("_API");

	// Generate header
	FString HeaderContent;
	HeaderContent += TEXT("// Auto-generated by GitHub Copilot UE\n\n");
	HeaderContent += TEXT("#pragma once\n\n");
	HeaderContent += TEXT("#include \"CoreMinimal.h\"\n");

	// Include appropriate parent header
	if (ParentClass == TEXT("AActor"))
		HeaderContent += TEXT("#include \"GameFramework/Actor.h\"\n");
	else if (ParentClass == TEXT("UActorComponent"))
		HeaderContent += TEXT("#include \"Components/ActorComponent.h\"\n");
	else if (ParentClass == TEXT("USceneComponent"))
		HeaderContent += TEXT("#include \"Components/SceneComponent.h\"\n");
	else
		HeaderContent += FString::Printf(TEXT("// TODO: Include header for %s\n"), *ParentClass);

	HeaderContent += FString::Printf(TEXT("#include \"%s.generated.h\"\n\n"), *ClassName);
	HeaderContent += FString::Printf(TEXT("UCLASS()\nclass %s %s : public %s\n{\n\tGENERATED_BODY()\n\n"), *APIMacro, *ClassName, *ParentClass);
	HeaderContent += TEXT("public:\n");
	HeaderContent += FString::Printf(TEXT("\t%s();\n\n"), *ClassName);
	HeaderContent += TEXT("protected:\n");
	HeaderContent += TEXT("\tvirtual void BeginPlay() override;\n\n");
	HeaderContent += TEXT("public:\n");
	HeaderContent += TEXT("\tvirtual void Tick(float DeltaTime) override;\n");
	HeaderContent += TEXT("};\n");

	// Generate cpp
	FString CppContent;
	CppContent += TEXT("// Auto-generated by GitHub Copilot UE\n\n");
	CppContent += FString::Printf(TEXT("#include \"%s.h\"\n\n"), *ClassName);
	CppContent += FString::Printf(TEXT("%s::%s()\n{\n\tPrimaryActorTick.bCanEverTick = true;\n}\n\n"), *ClassName, *ClassName);
	CppContent += FString::Printf(TEXT("void %s::BeginPlay()\n{\n\tSuper::BeginPlay();\n}\n\n"), *ClassName);
	CppContent += FString::Printf(TEXT("void %s::Tick(float DeltaTime)\n{\n\tSuper::Tick(DeltaTime);\n}\n"), *ClassName);

	// Create files
	FString Error;
	bool bHeaderOk = FileService->CreateNewFile(HeaderPath, HeaderContent, Error);
	if (!bHeaderOk)
	{
		Response.ResultStatus = ECopilotResultStatus::Failure;
		Response.ErrorMessage = FString::Printf(TEXT("Failed to create header: %s"), *Error);
		return Response;
	}

	bool bCppOk = FileService->CreateNewFile(CppPath, CppContent, Error);
	if (!bCppOk)
	{
		Response.ResultStatus = ECopilotResultStatus::Failure;
		Response.ErrorMessage = FString::Printf(TEXT("Header created but failed to create cpp: %s"), *Error);
		return Response;
	}

	Response.ResultStatus = ECopilotResultStatus::Success;
	Response.ResponseText = FString::Printf(TEXT("Created C++ class '%s' (parent: %s):\n  %s\n  %s\n\nRecompile to see the class in-editor."), *ClassName, *ParentClass, *HeaderPath, *CppPath);

	// Show diff preview of generated files
	Response.DiffPreview.OriginalFilePath = HeaderPath;
	Response.DiffPreview.ProposedContent = HeaderContent;
	Response.DiffPreview.bIsValid = true;
	Response.DiffPreview.UnifiedDiff = FString::Printf(TEXT("--- /dev/null\n+++ b/%s\n%s\n\n--- /dev/null\n+++ b/%s\n%s"), *HeaderPath, *HeaderContent, *CppPath, *CppContent);

	Log(FString::Printf(TEXT("CommandRouter: Created C++ class %s"), *ClassName));
	return Response;
}

FCopilotResponse FGitHubCopilotUECommandRouter::HandleCreateActorComponent(const FCopilotRequest& Request)
{
	// Reuse CreateCppClass with ActorComponent parent
	FCopilotRequest ModifiedRequest = Request;
	ModifiedRequest.CommandArguments.Add(TEXT("ParentClass"), TEXT("UActorComponent"));
	return HandleCreateCppClass(ModifiedRequest);
}

FCopilotResponse FGitHubCopilotUECommandRouter::HandleCreateBlueprintFunctionLibrary(const FCopilotRequest& Request)
{
	FCopilotResponse Response;
	Response.RequestId = Request.RequestId;
	Response.Timestamp = FDateTime::Now().ToString();

	if (!FileService.IsValid())
	{
		Response.ResultStatus = ECopilotResultStatus::Failure;
		Response.ErrorMessage = TEXT("File service not available");
		return Response;
	}

	FString ClassName;
	const FString* ClassNameArg = Request.CommandArguments.Find(TEXT("ClassName"));
	if (ClassNameArg)
		ClassName = *ClassNameArg;
	else if (!Request.UserPrompt.IsEmpty())
	{
		ClassName = Request.UserPrompt;
		ClassName.ReplaceInline(TEXT(" "), TEXT(""));
	}

	if (ClassName.IsEmpty())
	{
		ClassName = TEXT("UMyBlueprintFunctionLibrary");
	}

	if (!ClassName.StartsWith(TEXT("U")))
	{
		ClassName = TEXT("U") + ClassName;
	}

	FString ModuleName = FApp::GetProjectName();
	FString SourceDir = FString::Printf(TEXT("Source/%s"), *ModuleName);
	FString HeaderPath = FString::Printf(TEXT("%s/Public/%s.h"), *SourceDir, *ClassName);
	FString CppPath = FString::Printf(TEXT("%s/Private/%s.cpp"), *SourceDir, *ClassName);
	FString APIMacro = ModuleName.ToUpper() + TEXT("_API");

	FString HeaderContent;
	HeaderContent += TEXT("// Auto-generated by GitHub Copilot UE\n\n#pragma once\n\n");
	HeaderContent += TEXT("#include \"CoreMinimal.h\"\n#include \"Kismet/BlueprintFunctionLibrary.h\"\n");
	HeaderContent += FString::Printf(TEXT("#include \"%s.generated.h\"\n\n"), *ClassName);
	HeaderContent += FString::Printf(TEXT("UCLASS()\nclass %s %s : public UBlueprintFunctionLibrary\n{\n\tGENERATED_BODY()\n\npublic:\n"), *APIMacro, *ClassName);
	HeaderContent += TEXT("\tUFUNCTION(BlueprintCallable, Category = \"CopilotGenerated\")\n");
	HeaderContent += TEXT("\tstatic void ExampleFunction();\n");
	HeaderContent += TEXT("};\n");

	FString CppContent;
	CppContent += TEXT("// Auto-generated by GitHub Copilot UE\n\n");
	CppContent += FString::Printf(TEXT("#include \"%s.h\"\n\n"), *ClassName);
	CppContent += FString::Printf(TEXT("void %s::ExampleFunction()\n{\n\t// TODO: Implement\n}\n"), *ClassName);

	FString Error;
	bool bOk = FileService->CreateNewFile(HeaderPath, HeaderContent, Error);
	if (bOk) bOk = FileService->CreateNewFile(CppPath, CppContent, Error);

	if (bOk)
	{
		Response.ResultStatus = ECopilotResultStatus::Success;
		Response.ResponseText = FString::Printf(TEXT("Created Blueprint Function Library '%s':\n  %s\n  %s"), *ClassName, *HeaderPath, *CppPath);
	}
	else
	{
		Response.ResultStatus = ECopilotResultStatus::Failure;
		Response.ErrorMessage = FString::Printf(TEXT("Failed: %s"), *Error);
	}

	return Response;
}

FCopilotResponse FGitHubCopilotUECommandRouter::HandleCreateFile(const FCopilotRequest& Request)
{
	FCopilotResponse Response;
	Response.RequestId = Request.RequestId;
	Response.Timestamp = FDateTime::Now().ToString();

	if (!FileService.IsValid() || Request.FileTargets.Num() == 0)
	{
		Response.ResultStatus = ECopilotResultStatus::Failure;
		Response.ErrorMessage = TEXT("File service not available or no file targets specified");
		return Response;
	}

	const FCopilotFileTarget& Target = Request.FileTargets[0];
	FString Error;
	if (FileService->CreateNewFile(Target.FilePath, Target.SelectedText, Error))
	{
		Response.ResultStatus = ECopilotResultStatus::Success;
		Response.ResponseText = FString::Printf(TEXT("Created file: %s"), *Target.FilePath);
	}
	else
	{
		Response.ResultStatus = ECopilotResultStatus::Failure;
		Response.ErrorMessage = Error;
	}

	return Response;
}

FCopilotResponse FGitHubCopilotUECommandRouter::HandlePatchFile(const FCopilotRequest& Request)
{
	FCopilotResponse Response;
	Response.RequestId = Request.RequestId;
	Response.Timestamp = FDateTime::Now().ToString();

	if (!PatchService.IsValid() || !FileService.IsValid())
	{
		Response.ResultStatus = ECopilotResultStatus::Failure;
		Response.ErrorMessage = TEXT("Patch or File service not available");
		return Response;
	}

	if (Request.FileTargets.Num() == 0)
	{
		Response.ResultStatus = ECopilotResultStatus::Failure;
		Response.ErrorMessage = TEXT("No file targets provided for patch");
		return Response;
	}

	const FCopilotFileTarget& Target = Request.FileTargets[0];
	Log(FString::Printf(TEXT("CommandRouter: PatchFile starting for %s"), *Target.FilePath));

	// Step 1: Validate allowed roots before any work
	if (!FileService->IsPathWithinAllowedRoots(Target.FilePath))
	{
		Response.ResultStatus = ECopilotResultStatus::Failure;
		Response.ErrorMessage = FString::Printf(TEXT("REJECTED: '%s' is outside allowed write roots. Check Project Settings -> Plugins -> GitHub Copilot UE -> Allowed Write Roots."), *Target.FilePath);
		Log(Response.ErrorMessage);
		return Response;
	}

	// Step 2: Read original content
	FString OriginalContent;
	bool bFileExists = FileService->FileExists(Target.FilePath);
	if (bFileExists)
	{
		if (!FileService->ReadFile(Target.FilePath, OriginalContent))
		{
			Response.ResultStatus = ECopilotResultStatus::Failure;
			Response.ErrorMessage = FString::Printf(TEXT("Cannot read file: %s"), *Target.FilePath);
			return Response;
		}
		Log(FString::Printf(TEXT("CommandRouter: Read %d chars from %s"), OriginalContent.Len(), *Target.FilePath));
	}
	else
	{
		Log(FString::Printf(TEXT("CommandRouter: File %s does not exist (will be created)"), *Target.FilePath));
	}

	// Step 3: Get proposed content
	FString ProposedContent = Target.SelectedText;
	if (ProposedContent.IsEmpty())
	{
		Response.ResultStatus = ECopilotResultStatus::Failure;
		Response.ErrorMessage = TEXT("No proposed content provided for patch");
		return Response;
	}

	// Step 4: Generate diff preview (also registers as pending for approval)
	FCopilotDiffPreview Preview = PatchService->CreateDiffPreview(Target.FilePath, OriginalContent, ProposedContent);
	Response.DiffPreview = Preview;

	// Step 5: Decide whether to apply or just preview based on execution mode
	const UGitHubCopilotUESettings* Settings = UGitHubCopilotUESettings::Get();
	bool bRequirePreview = Settings ? Settings->bEnablePatchPreviewRequired : true;

	bool bShouldApplyNow = false;
	if (Request.ExecutionMode == ECopilotExecutionMode::ApplyImmediately)
	{
		bShouldApplyNow = true;
	}
	else if (Request.ExecutionMode == ECopilotExecutionMode::ApplyWithApproval && !bRequirePreview)
	{
		bShouldApplyNow = true;
	}
	// SuggestOnly and PreviewOnly: never auto-apply

	if (bShouldApplyNow)
	{
		Log(TEXT("CommandRouter: Applying patch immediately (execution mode permits)"));
		FCopilotPatchResult PatchResult = PatchService->ApplyPatchWithSteps(Preview);

		Response.ResponseText = PatchResult.GetStepLogText();

		if (PatchResult.bSuccess)
		{
			Response.ResultStatus = ECopilotResultStatus::Success;
			Response.ResponseText += FString::Printf(TEXT("\n\nPatch applied to %s"), *Target.FilePath);
			if (!PatchResult.BackupPath.IsEmpty())
			{
				Response.ResponseText += FString::Printf(TEXT("\nBackup: %s"), *PatchResult.BackupPath);
			}
		}
		else
		{
			Response.ResultStatus = ECopilotResultStatus::Failure;
			Response.ErrorMessage = PatchResult.ErrorMessage;
			if (!PatchResult.BackupPath.IsEmpty())
			{
				Response.ErrorMessage += FString::Printf(TEXT(" (backup at: %s)"), *PatchResult.BackupPath);
			}
		}
	}
	else
	{
		// Preview only — patch is registered as pending, user must click "Apply Patch" to approve
		Response.ResultStatus = ECopilotResultStatus::Success;
		Response.ResponseText = FString::Printf(
			TEXT("Diff preview generated for: %s\n")
			TEXT("Original: %d chars | Proposed: %d chars\n\n")
			TEXT("Review the diff below, then click 'Apply Patch' to approve or 'Rollback' to cancel."),
			*Target.FilePath, OriginalContent.Len(), ProposedContent.Len());
		Log(FString::Printf(TEXT("CommandRouter: Preview generated for %s (awaiting approval)"), *Target.FilePath));
	}

	return Response;
}

FCopilotResponse FGitHubCopilotUECommandRouter::HandleApproveAndApplyPatch(const FCopilotRequest& Request)
{
	FCopilotResponse Response;
	Response.RequestId = Request.RequestId;
	Response.Timestamp = FDateTime::Now().ToString();

	if (!PatchService.IsValid())
	{
		Response.ResultStatus = ECopilotResultStatus::Failure;
		Response.ErrorMessage = TEXT("Patch service not available");
		return Response;
	}

	// Determine which file to approve
	FString FilePath;
	if (Request.FileTargets.Num() > 0)
	{
		FilePath = Request.FileTargets[0].FilePath;
	}

	// Try to find a pending preview
	FCopilotDiffPreview Preview;
	bool bFound = false;

	if (!FilePath.IsEmpty())
	{
		bFound = PatchService->GetPendingPreview(FilePath, Preview);
	}

	// Fallback: use the last generated preview
	if (!bFound)
	{
		bFound = PatchService->GetLastPreview(Preview);
		if (bFound)
		{
			FilePath = Preview.OriginalFilePath;
		}
	}

	if (!bFound)
	{
		Response.ResultStatus = ECopilotResultStatus::Failure;
		Response.ErrorMessage = TEXT("No pending patch preview found. Run 'Preview Patch' first to generate a diff.");
		return Response;
	}

	Log(FString::Printf(TEXT("CommandRouter: Approving and applying pending patch for %s"), *FilePath));

	// Execute the patch with full step tracking
	FCopilotPatchResult PatchResult = PatchService->ApplyPatchWithSteps(Preview);

	Response.DiffPreview = Preview;
	Response.ResponseText = PatchResult.GetStepLogText();

	if (PatchResult.bSuccess)
	{
		Response.ResultStatus = ECopilotResultStatus::Success;
		Response.ResponseText += FString::Printf(TEXT("\n\nApproved and applied patch to: %s"), *FilePath);
		if (!PatchResult.BackupPath.IsEmpty())
		{
			Response.ResponseText += FString::Printf(TEXT("\nBackup saved at: %s"), *PatchResult.BackupPath);
		}
	}
	else
	{
		Response.ResultStatus = ECopilotResultStatus::Failure;
		Response.ErrorMessage = PatchResult.ErrorMessage;
		Response.ResponseText += FString::Printf(TEXT("\n\nPatch FAILED for: %s\n%s"), *FilePath, *PatchResult.ErrorMessage);
	}

	return Response;
}

FCopilotResponse FGitHubCopilotUECommandRouter::HandleRollbackPatch(const FCopilotRequest& Request)
{
	FCopilotResponse Response;
	Response.RequestId = Request.RequestId;
	Response.Timestamp = FDateTime::Now().ToString();

	if (!PatchService.IsValid())
	{
		Response.ResultStatus = ECopilotResultStatus::Failure;
		Response.ErrorMessage = TEXT("Patch service not available");
		return Response;
	}

	// Determine which file to rollback
	FString FilePath;
	if (Request.FileTargets.Num() > 0)
	{
		FilePath = Request.FileTargets[0].FilePath;
	}
	else if (!Request.UserPrompt.IsEmpty())
	{
		FilePath = Request.UserPrompt;
	}
	else
	{
		// Try to use the last preview's file
		FCopilotDiffPreview LastPreview;
		if (PatchService->GetLastPreview(LastPreview))
		{
			FilePath = LastPreview.OriginalFilePath;
		}
	}

	if (FilePath.IsEmpty())
	{
		Response.ResultStatus = ECopilotResultStatus::Failure;
		Response.ErrorMessage = TEXT("No file path specified for rollback. Provide a file target or enter the path in the prompt.");
		return Response;
	}

	Log(FString::Printf(TEXT("CommandRouter: Rolling back %s"), *FilePath));

	FString Error;
	if (PatchService->Rollback(FilePath, Error))
	{
		Response.ResultStatus = ECopilotResultStatus::Success;
		Response.ResponseText = FString::Printf(TEXT("Successfully rolled back: %s\nOriginal file content has been restored from backup."), *FilePath);
	}
	else
	{
		Response.ResultStatus = ECopilotResultStatus::Failure;
		Response.ErrorMessage = FString::Printf(TEXT("Rollback failed for %s: %s"), *FilePath, *Error);
	}

	return Response;
}

FCopilotResponse FGitHubCopilotUECommandRouter::HandleInsertIntoFile(const FCopilotRequest& Request)
{
	FCopilotResponse Response;
	Response.RequestId = Request.RequestId;
	Response.Timestamp = FDateTime::Now().ToString();

	if (!PatchService.IsValid() || Request.FileTargets.Num() == 0)
	{
		Response.ResultStatus = ECopilotResultStatus::Failure;
		Response.ErrorMessage = TEXT("Patch service not available or no file targets");
		return Response;
	}

	const FCopilotFileTarget& Target = Request.FileTargets[0];
	FString Error;

	if (PatchService->InsertAtLine(Target.FilePath, Target.LineStart, Target.SelectedText, Error))
	{
		Response.ResultStatus = ECopilotResultStatus::Success;
		Response.ResponseText = FString::Printf(TEXT("Inserted content at line %d in %s"), Target.LineStart, *Target.FilePath);
	}
	else
	{
		Response.ResultStatus = ECopilotResultStatus::Failure;
		Response.ErrorMessage = Error;
	}

	return Response;
}

FCopilotResponse FGitHubCopilotUECommandRouter::HandleOpenAsset(const FCopilotRequest& Request)
{
	FCopilotResponse Response;
	Response.RequestId = Request.RequestId;
	Response.Timestamp = FDateTime::Now().ToString();

	FString AssetPath;
	const FString* AssetArg = Request.CommandArguments.Find(TEXT("AssetPath"));
	if (AssetArg)
		AssetPath = *AssetArg;
	else
		AssetPath = Request.UserPrompt;

	if (AssetPath.IsEmpty())
	{
		Response.ResultStatus = ECopilotResultStatus::Failure;
		Response.ErrorMessage = TEXT("No asset path specified");
		return Response;
	}

	// Try to open the asset in the editor
	FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
	FAssetData AssetData = AssetRegistryModule.Get().GetAssetByObjectPath(FSoftObjectPath(AssetPath));

	if (AssetData.IsValid())
	{
		if (GEditor)
		{
			TArray<UObject*> Assets;
			UObject* Asset = AssetData.GetAsset();
			if (Asset)
			{
				Assets.Add(Asset);
				GEditor->GetEditorSubsystem<UAssetEditorSubsystem>()->OpenEditorForAssets(Assets);
				Response.ResultStatus = ECopilotResultStatus::Success;
				Response.ResponseText = FString::Printf(TEXT("Opened asset: %s"), *AssetPath);
			}
			else
			{
				Response.ResultStatus = ECopilotResultStatus::Failure;
				Response.ErrorMessage = FString::Printf(TEXT("Failed to load asset: %s"), *AssetPath);
			}
		}
	}
	else
	{
		Response.ResultStatus = ECopilotResultStatus::Failure;
		Response.ErrorMessage = FString::Printf(TEXT("Asset not found: %s"), *AssetPath);
	}

	return Response;
}

FCopilotResponse FGitHubCopilotUECommandRouter::HandleOpenFile(const FCopilotRequest& Request)
{
	FCopilotResponse Response;
	Response.RequestId = Request.RequestId;
	Response.Timestamp = FDateTime::Now().ToString();

	FString FilePath;
	const FString* FileArg = Request.CommandArguments.Find(TEXT("FilePath"));
	if (FileArg)
		FilePath = *FileArg;
	else if (Request.FileTargets.Num() > 0)
		FilePath = Request.FileTargets[0].FilePath;
	else
		FilePath = Request.UserPrompt;

	if (FilePath.IsEmpty())
	{
		Response.ResultStatus = ECopilotResultStatus::Failure;
		Response.ErrorMessage = TEXT("No file path specified");
		return Response;
	}

	FString FullPath = FPaths::IsRelative(FilePath) ?
		FPaths::Combine(FPaths::ProjectDir(), FilePath) : FilePath;

	if (FPaths::FileExists(FullPath))
	{
		FSourceCodeNavigation::OpenSourceFile(FullPath);
		Response.ResultStatus = ECopilotResultStatus::Success;
		Response.ResponseText = FString::Printf(TEXT("Opened file: %s"), *FullPath);
	}
	else
	{
		Response.ResultStatus = ECopilotResultStatus::Failure;
		Response.ErrorMessage = FString::Printf(TEXT("File not found: %s"), *FullPath);
	}

	return Response;
}

FCopilotResponse FGitHubCopilotUECommandRouter::HandleTriggerCompile(const FCopilotRequest& Request)
{
	if (!CompileService.IsValid())
	{
		FCopilotResponse Response;
		Response.RequestId = Request.RequestId;
		Response.ResultStatus = ECopilotResultStatus::Failure;
		Response.ErrorMessage = TEXT("Compile service not available");
		return Response;
	}

	FCopilotResponse Response = CompileService->RequestCompile();
	Response.RequestId = Request.RequestId;
	return Response;
}

FCopilotResponse FGitHubCopilotUECommandRouter::HandleTriggerLiveCoding(const FCopilotRequest& Request)
{
	if (!CompileService.IsValid())
	{
		FCopilotResponse Response;
		Response.RequestId = Request.RequestId;
		Response.ResultStatus = ECopilotResultStatus::Failure;
		Response.ErrorMessage = TEXT("Compile service not available");
		return Response;
	}

	FCopilotResponse Response = CompileService->RequestLiveCodingPatch();
	Response.RequestId = Request.RequestId;
	return Response;
}

FCopilotResponse FGitHubCopilotUECommandRouter::HandleRunAutomationTests(const FCopilotRequest& Request)
{
	if (!CompileService.IsValid())
	{
		FCopilotResponse Response;
		Response.RequestId = Request.RequestId;
		Response.ResultStatus = ECopilotResultStatus::Failure;
		Response.ErrorMessage = TEXT("Compile service not available");
		return Response;
	}

	FString Filter = Request.UserPrompt;
	const FString* FilterArg = Request.CommandArguments.Find(TEXT("TestFilter"));
	if (FilterArg) Filter = *FilterArg;

	FCopilotResponse Response = CompileService->RunAutomationTests(Filter);
	Response.RequestId = Request.RequestId;
	return Response;
}

FCopilotResponse FGitHubCopilotUECommandRouter::HandleRunQuestAudit(const FCopilotRequest& Request)
{
	return HandleGatherVRContext(Request);
}

void FGitHubCopilotUECommandRouter::Log(const FString& Message)
{
	UE_LOG(LogGitHubCopilotUE, Verbose, TEXT("%s"), *Message);
}
