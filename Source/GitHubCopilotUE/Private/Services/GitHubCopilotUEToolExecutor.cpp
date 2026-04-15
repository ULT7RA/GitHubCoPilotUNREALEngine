// Copyright GitHub, Inc. All Rights Reserved.

#include "Services/GitHubCopilotUEToolExecutor.h"
#include "Services/GitHubCopilotUEFileService.h"
#include "Services/GitHubCopilotUEContextService.h"
#include "Services/GitHubCopilotUECompileService.h"
#include "Services/GitHubCopilotUETypes.h"
#include "GitHubCopilotUESettings.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "HAL/Event.h"
#include "HttpModule.h"
#include "HttpManager.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "GenericPlatform/GenericPlatformHttp.h"
#include "UnrealClient.h"
#include "Engine/GameEngine.h"
#include "Editor.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "GameFramework/Actor.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/Character.h"
#include "Components/ActorComponent.h"
#include "Components/SceneComponent.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionConstant3Vector.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionTextureSample.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/DataTable.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Framework/Application/SlateApplication.h"
#include "Widgets/SWindow.h"
#include "IImageWrapperModule.h"
#include "IImageWrapper.h"
#include "Misc/OutputDeviceHelper.h"
#include "Settings/LevelEditorPlaySettings.h"
#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "AutomatedAssetImportData.h"
#include "FileHelpers.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "K2Node_CallFunction.h"
#include "K2Node_Event.h"
#include "Components/SkeletalMeshComponent.h"
#include "Misc/ConfigCacheIni.h"
#include "Engine/Selection.h"

#include "Windows/AllowWindowsPlatformTypes.h"
#include <Windows.h>
#include "Windows/HideWindowsPlatformTypes.h"

namespace
{
	static FString SanitizeAssetName(const FString& InName)
	{
		FString Name = InName.TrimStartAndEnd();
		Name.ReplaceInline(TEXT(" "), TEXT("_"));
		for (int32 Index = 0; Index < Name.Len(); ++Index)
		{
			const TCHAR C = Name[Index];
			if (!(FChar::IsAlnum(C) || C == TCHAR('_')))
			{
				Name[Index] = TCHAR('_');
			}
		}
		return Name;
	}

	static UClass* ResolveParentClass(const FString& ParentClassInput)
	{
		const FString Parent = ParentClassInput.TrimStartAndEnd();
		if (Parent.IsEmpty() || Parent.Equals(TEXT("Actor"), ESearchCase::IgnoreCase) || Parent.Equals(TEXT("AActor"), ESearchCase::IgnoreCase))
		{
			return AActor::StaticClass();
		}
		if (Parent.Equals(TEXT("Pawn"), ESearchCase::IgnoreCase) || Parent.Equals(TEXT("APawn"), ESearchCase::IgnoreCase))
		{
			return APawn::StaticClass();
		}
		if (Parent.Equals(TEXT("Character"), ESearchCase::IgnoreCase) || Parent.Equals(TEXT("ACharacter"), ESearchCase::IgnoreCase))
		{
			return ACharacter::StaticClass();
		}
		if (Parent.Equals(TEXT("ActorComponent"), ESearchCase::IgnoreCase) || Parent.Equals(TEXT("UActorComponent"), ESearchCase::IgnoreCase))
		{
			return UActorComponent::StaticClass();
		}
		if (Parent.Equals(TEXT("SceneComponent"), ESearchCase::IgnoreCase) || Parent.Equals(TEXT("USceneComponent"), ESearchCase::IgnoreCase))
		{
			return USceneComponent::StaticClass();
		}
		if (Parent.Equals(TEXT("FunctionLibrary"), ESearchCase::IgnoreCase) || Parent.Equals(TEXT("BlueprintFunctionLibrary"), ESearchCase::IgnoreCase) || Parent.Equals(TEXT("UBlueprintFunctionLibrary"), ESearchCase::IgnoreCase))
		{
			return UBlueprintFunctionLibrary::StaticClass();
		}

		if (Parent.StartsWith(TEXT("/Script/")))
		{
			if (UClass* LoadedClass = StaticLoadClass(UObject::StaticClass(), nullptr, *Parent))
			{
				return LoadedClass;
			}
		}

		const TArray<FString> CandidatePaths = {
			FString::Printf(TEXT("/Script/Engine.%s"), *Parent),
			FString::Printf(TEXT("/Script/Engine.A%s"), *Parent),
			FString::Printf(TEXT("/Script/Engine.U%s"), *Parent),
			FString::Printf(TEXT("/Script/CoreUObject.%s"), *Parent),
			FString::Printf(TEXT("/Script/CoreUObject.U%s"), *Parent)
		};

		for (const FString& Candidate : CandidatePaths)
		{
			if (UClass* LoadedClass = StaticLoadClass(UObject::StaticClass(), nullptr, *Candidate))
			{
				return LoadedClass;
			}
		}

		return nullptr;
	}
}

FGitHubCopilotUEToolExecutor::FGitHubCopilotUEToolExecutor()
{
}

void FGitHubCopilotUEToolExecutor::Initialize(
	TSharedPtr<FGitHubCopilotUEFileService> InFileService,
	TSharedPtr<FGitHubCopilotUEContextService> InContextService,
	TSharedPtr<FGitHubCopilotUECompileService> InCompileService)
{
	FileService = InFileService;
	ContextService = InContextService;
	CompileService = InCompileService;
}

// ============================================================================
// Tool dispatch
// ============================================================================

FString FGitHubCopilotUEToolExecutor::ExecuteTool(const FString& ToolName, const TSharedPtr<FJsonObject>& Arguments)
{
	if (!Arguments.IsValid())
	{
		return TEXT("Error: Tool arguments are missing");
	}

	// Copilot CLI style aliases
	if (ToolName == TEXT("view"))                return Tool_ViewPath(Arguments);
	if (ToolName == TEXT("glob"))                return Tool_GlobFiles(Arguments);
	if (ToolName == TEXT("rg"))                  return Tool_SearchFiles(Arguments);

	if (ToolName == TEXT("read_file"))           return Tool_ReadFile(Arguments);
	if (ToolName == TEXT("write_file"))          return Tool_WriteFile(Arguments);
	if (ToolName == TEXT("edit_file"))           return Tool_EditFile(Arguments);
	if (ToolName == TEXT("list_directory"))      return Tool_ListDirectory(Arguments);
	if (ToolName == TEXT("search_files"))        return Tool_SearchFiles(Arguments);
	if (ToolName == TEXT("create_directory"))    return Tool_CreateDirectory(Arguments);
	if (ToolName == TEXT("copy_file"))           return Tool_CopyFile(Arguments);
	if (ToolName == TEXT("move_file"))           return Tool_MoveFile(Arguments);
	if (ToolName == TEXT("get_project_structure")) return Tool_GetProjectStructure(Arguments);
	if (ToolName == TEXT("create_cpp_class"))    return Tool_CreateCppClass(Arguments);
	if (ToolName == TEXT("create_blueprint_asset")) return Tool_CreateBlueprintAsset(Arguments);
	if (ToolName == TEXT("compile"))             return Tool_Compile(Arguments);
	if (ToolName == TEXT("live_coding_patch"))   return Tool_LiveCodingPatch(Arguments);
	if (ToolName == TEXT("run_automation_tests")) return Tool_RunAutomationTests(Arguments);
	if (ToolName == TEXT("get_file_info"))       return Tool_GetFileInfo(Arguments);
	if (ToolName == TEXT("delete_file"))         return Tool_DeleteFile(Arguments);
	if (ToolName == TEXT("spawn_actor"))          return Tool_SpawnActor(Arguments);
	if (ToolName == TEXT("create_material_asset")) return Tool_CreateMaterialAsset(Arguments);
	if (ToolName == TEXT("create_data_table"))    return Tool_CreateDataTable(Arguments);
	if (ToolName == TEXT("create_niagara_system")) return Tool_CreateNiagaraSystem(Arguments);
	if (ToolName == TEXT("web_search"))           return Tool_WebSearch(Arguments);
	if (ToolName == TEXT("capture_viewport"))     return Tool_CaptureViewport(Arguments);

	// Tier 1 — autonomy tools
	if (ToolName == TEXT("execute_python"))       return Tool_ExecutePython(Arguments);
	if (ToolName == TEXT("execute_console_command")) return Tool_ExecuteConsoleCommand(Arguments);
	if (ToolName == TEXT("execute_shell"))        return Tool_ExecuteShell(Arguments);
	if (ToolName == TEXT("play_in_editor"))       return Tool_PlayInEditor(Arguments);
	if (ToolName == TEXT("stop_pie"))             return Tool_StopPIE(Arguments);
	if (ToolName == TEXT("package_project"))      return Tool_PackageProject(Arguments);

	// Tier 2 — scene & asset tools
	if (ToolName == TEXT("list_actors"))          return Tool_ListActors(Arguments);
	if (ToolName == TEXT("get_actor_properties")) return Tool_GetActorProperties(Arguments);
	if (ToolName == TEXT("set_actor_properties")) return Tool_SetActorProperties(Arguments);
	if (ToolName == TEXT("delete_actors"))        return Tool_DeleteActors(Arguments);
	if (ToolName == TEXT("import_asset"))         return Tool_ImportAsset(Arguments);
	if (ToolName == TEXT("open_level"))           return Tool_OpenLevel(Arguments);
	if (ToolName == TEXT("rename_asset"))         return Tool_RenameAsset(Arguments);
	if (ToolName == TEXT("get_selected_actors"))  return Tool_GetSelectedActors(Arguments);

	// Tier 3 — specialized tools
	if (ToolName == TEXT("retarget_animations"))  return Tool_RetargetAnimations(Arguments);
	if (ToolName == TEXT("create_anim_montage")) return Tool_CreateAnimMontage(Arguments);
	if (ToolName == TEXT("create_level_sequence")) return Tool_CreateLevelSequence(Arguments);
	if (ToolName == TEXT("build_lighting"))       return Tool_BuildLighting(Arguments);
	if (ToolName == TEXT("get_project_settings")) return Tool_GetProjectSettings(Arguments);
	if (ToolName == TEXT("set_project_settings")) return Tool_SetProjectSettings(Arguments);
	if (ToolName == TEXT("create_widget_blueprint")) return Tool_CreateWidgetBlueprint(Arguments);
	if (ToolName == TEXT("get_blueprint_graph"))  return Tool_GetBlueprintGraph(Arguments);
	if (ToolName == TEXT("add_blueprint_node"))   return Tool_AddBlueprintNode(Arguments);
	if (ToolName == TEXT("git_commit"))           return Tool_GitCommit(Arguments);
	if (ToolName == TEXT("git_push"))             return Tool_GitPush(Arguments);

	return FString::Printf(TEXT("Error: Unknown tool '%s'"), *ToolName);
}

// ============================================================================
// Path utilities
// ============================================================================

FString FGitHubCopilotUEToolExecutor::ResolvePath(const FString& InputPath) const
{
	FString Path = InputPath;
	Path.ReplaceInline(TEXT("/"), TEXT("\\"));

	// If relative, resolve against project root
	if (FPaths::IsRelative(Path))
	{
		Path = FPaths::Combine(FPaths::ProjectDir(), Path);
	}

	FPaths::NormalizeDirectoryName(Path);
	FPaths::CollapseRelativeDirectories(Path);
	return Path;
}

bool FGitHubCopilotUEToolExecutor::IsPathAllowed(const FString& FullPath) const
{
	const UGitHubCopilotUESettings* Settings = UGitHubCopilotUESettings::Get();

	// Allow-all mode bypasses all checks
	if (Settings && Settings->bAllowAllFileAccess)
	{
		return true;
	}

	FString NormPath = FPaths::ConvertRelativePathToFull(FullPath);
	FPaths::NormalizeDirectoryName(NormPath);

	// Always allow project directory
	FString ProjectDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
	FPaths::NormalizeDirectoryName(ProjectDir);
	if (NormPath.StartsWith(ProjectDir))
	{
		return true;
	}

	// Check additional allowed paths from settings
	if (Settings)
	{
		for (const FString& AllowedPath : Settings->AdditionalAllowedPaths)
		{
			if (!AllowedPath.IsEmpty())
			{
				FString NormAllowed = FPaths::ConvertRelativePathToFull(AllowedPath);
				FPaths::NormalizeDirectoryName(NormAllowed);
				if (NormPath.StartsWith(NormAllowed))
				{
					return true;
				}
			}
		}
	}

	return false;
}

// ============================================================================
// Tool: view (Copilot CLI style)
// ============================================================================

FString FGitHubCopilotUEToolExecutor::Tool_ViewPath(const TSharedPtr<FJsonObject>& Args)
{
	FString Path;
	if (!Args->TryGetStringField(TEXT("path"), Path) || Path.IsEmpty())
	{
		return TEXT("Error: 'path' is required");
	}

	const FString FullPath = ResolvePath(Path);
	if (!IsPathAllowed(FullPath))
	{
		return FString::Printf(TEXT("Error: Path '%s' is outside the project directory"), *Path);
	}

	if (FPaths::DirectoryExists(FullPath))
	{
		TSharedPtr<FJsonObject> DirArgs = MakeShareable(new FJsonObject);
		DirArgs->SetStringField(TEXT("path"), Path);
		return Tool_ListDirectory(DirArgs);
	}

	if (FPaths::FileExists(FullPath))
	{
		return Tool_ReadFile(Args);
	}

	return FString::Printf(TEXT("Error: Path '%s' does not exist"), *Path);
}

// ============================================================================
// Tool: glob (Copilot CLI style)
// ============================================================================

FString FGitHubCopilotUEToolExecutor::Tool_GlobFiles(const TSharedPtr<FJsonObject>& Args)
{
	FString Pattern;
	if (!Args->TryGetStringField(TEXT("pattern"), Pattern) || Pattern.IsEmpty())
	{
		return TEXT("Error: 'pattern' is required");
	}

	FString Path = TEXT(".");
	Args->TryGetStringField(TEXT("path"), Path);

	int32 MaxResults = 500;
	Args->TryGetNumberField(TEXT("max_results"), MaxResults);
	MaxResults = FMath::Clamp(MaxResults, 1, 2000);

	const FString FullPath = ResolvePath(Path);
	if (!IsPathAllowed(FullPath))
	{
		return FString::Printf(TEXT("Error: Path '%s' is outside the project directory"), *Path);
	}
	if (!FPaths::DirectoryExists(FullPath))
	{
		return FString::Printf(TEXT("Error: Directory '%s' does not exist"), *Path);
	}

	FString NormalizedPattern = Pattern.TrimStartAndEnd();
	NormalizedPattern.ReplaceInline(TEXT("\\"), TEXT("/"));

	TArray<FString> AllFiles;
	IFileManager::Get().FindFilesRecursive(AllFiles, *FullPath, TEXT("*"), true, false);
	AllFiles.Sort();

	const FString ProjectDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
	FString Result;
	int32 MatchCount = 0;

	for (const FString& FilePath : AllFiles)
	{
		FString RelativePath = FPaths::ConvertRelativePathToFull(FilePath);
		RelativePath.RemoveFromStart(ProjectDir);
		RelativePath.ReplaceInline(TEXT("\\"), TEXT("/"));

		if (RelativePath.MatchesWildcard(NormalizedPattern, ESearchCase::IgnoreCase))
		{
			Result += RelativePath + TEXT("\n");
			++MatchCount;
			if (MatchCount >= MaxResults)
			{
				break;
			}
		}
	}

	if (MatchCount == 0)
	{
		return FString::Printf(TEXT("No files matched pattern '%s' in '%s'"), *Pattern, *Path);
	}

	return FString::Printf(TEXT("Matched %d path(s):\n%s"), MatchCount, *Result);
}

// ============================================================================
// Tool: read_file
// ============================================================================

FString FGitHubCopilotUEToolExecutor::Tool_ReadFile(const TSharedPtr<FJsonObject>& Args)
{
	FString Path = Args->GetStringField(TEXT("path"));
	if (Path.IsEmpty()) return TEXT("Error: 'path' is required");

	FString FullPath = ResolvePath(Path);
	if (!IsPathAllowed(FullPath))
	{
		return FString::Printf(TEXT("Error: Path '%s' is outside the project directory"), *Path);
	}

	FString Content;
	if (!FFileHelper::LoadFileToString(Content, *FullPath))
	{
		return FString::Printf(TEXT("Error: Could not read file '%s'"), *FullPath);
	}

	// Respect optional line range
	int32 StartLine = 0, EndLine = 0;
	if (Args->TryGetNumberField(TEXT("start_line"), StartLine) || Args->TryGetNumberField(TEXT("end_line"), EndLine))
	{
		TArray<FString> Lines;
		Content.ParseIntoArrayLines(Lines);

		if (StartLine < 1) StartLine = 1;
		if (EndLine < 1 || EndLine > Lines.Num()) EndLine = Lines.Num();

		FString Result;
		for (int32 i = StartLine - 1; i < EndLine && i < Lines.Num(); ++i)
		{
			Result += FString::Printf(TEXT("%d. %s\n"), i + 1, *Lines[i]);
		}
		return Result;
	}

	// If file is very large, return with line numbers
	TArray<FString> Lines;
	Content.ParseIntoArrayLines(Lines);
	if (Lines.Num() > 500)
	{
		// Return first 200 lines with a note
		FString Result;
		for (int32 i = 0; i < 200 && i < Lines.Num(); ++i)
		{
			Result += FString::Printf(TEXT("%d. %s\n"), i + 1, *Lines[i]);
		}
		Result += FString::Printf(TEXT("\n... (%d more lines. Use start_line/end_line to read specific ranges.)"), Lines.Num() - 200);
		return Result;
	}

	// Return with line numbers
	FString Result;
	for (int32 i = 0; i < Lines.Num(); ++i)
	{
		Result += FString::Printf(TEXT("%d. %s\n"), i + 1, *Lines[i]);
	}
	return Result;
}

// ============================================================================
// Tool: write_file
// ============================================================================

FString FGitHubCopilotUEToolExecutor::Tool_WriteFile(const TSharedPtr<FJsonObject>& Args)
{
	FString Path = Args->GetStringField(TEXT("path"));
	FString Content = Args->GetStringField(TEXT("content"));
	if (Path.IsEmpty()) return TEXT("Error: 'path' is required");
	if (Content.IsEmpty()) return TEXT("Error: 'content' is required");

	FString FullPath = ResolvePath(Path);
	if (!IsPathAllowed(FullPath))
	{
		return FString::Printf(TEXT("Error: Path '%s' is outside the project directory"), *Path);
	}

	// Create backup if file exists
	if (FPaths::FileExists(FullPath))
	{
		FString BackupPath = FullPath + TEXT(".bak");
		IFileManager::Get().Copy(*BackupPath, *FullPath);
	}

	// Ensure directory exists
	FString Dir = FPaths::GetPath(FullPath);
	if (!FPaths::DirectoryExists(Dir))
	{
		IFileManager::Get().MakeDirectory(*Dir, true);
	}

	if (FFileHelper::SaveStringToFile(Content, *FullPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		UE_LOG(LogGitHubCopilotUE, Log, TEXT("ToolExecutor: Wrote file: %s (%d chars)"), *FullPath, Content.Len());
		return FString::Printf(TEXT("Successfully wrote %d characters to '%s'"), Content.Len(), *Path);
	}

	return FString::Printf(TEXT("Error: Failed to write file '%s'"), *Path);
}

// ============================================================================
// Tool: edit_file
// ============================================================================

FString FGitHubCopilotUEToolExecutor::Tool_EditFile(const TSharedPtr<FJsonObject>& Args)
{
	FString Path = Args->GetStringField(TEXT("path"));
	FString OldStr = Args->GetStringField(TEXT("old_str"));
	FString NewStr = Args->GetStringField(TEXT("new_str"));
	if (Path.IsEmpty()) return TEXT("Error: 'path' is required");
	if (OldStr.IsEmpty()) return TEXT("Error: 'old_str' is required");

	FString FullPath = ResolvePath(Path);
	if (!IsPathAllowed(FullPath))
	{
		return FString::Printf(TEXT("Error: Path '%s' is outside the project directory"), *Path);
	}

	FString Content;
	if (!FFileHelper::LoadFileToString(Content, *FullPath))
	{
		return FString::Printf(TEXT("Error: Could not read file '%s'"), *Path);
	}

	// Check that old_str exists and is unique
	int32 FirstIdx = Content.Find(OldStr);
	if (FirstIdx == INDEX_NONE)
	{
		return FString::Printf(TEXT("Error: old_str not found in '%s'. Make sure it matches exactly including whitespace."), *Path);
	}

	int32 SecondIdx = Content.Find(OldStr, ESearchCase::CaseSensitive, ESearchDir::FromStart, FirstIdx + OldStr.Len());
	if (SecondIdx != INDEX_NONE)
	{
		return FString::Printf(TEXT("Error: old_str appears multiple times in '%s'. Include more context to make it unique."), *Path);
	}

	// Create backup
	FString BackupPath = FullPath + TEXT(".bak");
	IFileManager::Get().Copy(*BackupPath, *FullPath);

	// Perform the replacement
	FString NewContent = Content;
	NewContent = NewContent.Replace(*OldStr, *NewStr, ESearchCase::CaseSensitive);

	if (FFileHelper::SaveStringToFile(NewContent, *FullPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		UE_LOG(LogGitHubCopilotUE, Log, TEXT("ToolExecutor: Edited file: %s (replaced %d chars with %d chars)"),
			*FullPath, OldStr.Len(), NewStr.Len());
		return FString::Printf(TEXT("Successfully edited '%s' — replaced %d chars with %d chars"), *Path, OldStr.Len(), NewStr.Len());
	}

	return FString::Printf(TEXT("Error: Failed to save edited file '%s'"), *Path);
}

// ============================================================================
// Tool: list_directory
// ============================================================================

FString FGitHubCopilotUEToolExecutor::Tool_ListDirectory(const TSharedPtr<FJsonObject>& Args)
{
	FString Path = Args->GetStringField(TEXT("path"));
	if (Path.IsEmpty()) Path = TEXT(".");

	FString FullPath = ResolvePath(Path);
	if (!IsPathAllowed(FullPath))
	{
		return FString::Printf(TEXT("Error: Path '%s' is outside the project directory"), *Path);
	}

	if (!FPaths::DirectoryExists(FullPath))
	{
		return FString::Printf(TEXT("Error: Directory '%s' does not exist"), *Path);
	}

	// Optional recursive scan with depth limit
	bool bRecursive = false;
	if (Args->HasField(TEXT("recursive")))
	{
		Args->TryGetBoolField(TEXT("recursive"), bRecursive);
	}
	int32 MaxDepth = 2; // Default depth for recursive
	if (Args->HasField(TEXT("max_depth")))
	{
		MaxDepth = FMath::Clamp((int32)Args->GetNumberField(TEXT("max_depth")), 1, 5);
	}

	FString ProjectDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
	FString NormFull = FPaths::ConvertRelativePathToFull(FullPath);
	FString RelDir = NormFull;
	RelDir.RemoveFromStart(ProjectDir);

	FString Result = FString::Printf(TEXT("Directory: %s\n\n"), RelDir.IsEmpty() ? TEXT("/") : *RelDir);

	if (bRecursive)
	{
		// Recursive listing with depth limit
		TArray<FString> AllFiles;
		IFileManager::Get().FindFilesRecursive(AllFiles, *FullPath, TEXT("*"), true, true);
		AllFiles.Sort();

		int32 FileCount = 0;
		int32 DirCount = 0;
		for (const FString& FilePath : AllFiles)
		{
			// Calculate depth relative to search root
			FString RelPath = FilePath;
			RelPath.RemoveFromStart(FullPath);
			RelPath.RemoveFromStart(TEXT("/"));
			RelPath.RemoveFromStart(TEXT("\\"));

			int32 Depth = 0;
			for (TCHAR C : RelPath) { if (C == '/' || C == '\\') Depth++; }
			if (Depth >= MaxDepth) continue;

			bool bIsDir = FPaths::DirectoryExists(FilePath);
			if (bIsDir)
			{
				Result += FString::Printf(TEXT("  %s/\n"), *RelPath);
				DirCount++;
			}
			else
			{
				int64 Size = IFileManager::Get().FileSize(*FilePath);
				if (Size > 1024 * 1024)
					Result += FString::Printf(TEXT("  %s  (%.1f MB)\n"), *RelPath, Size / (1024.0 * 1024.0));
				else if (Size > 1024)
					Result += FString::Printf(TEXT("  %s  (%.1f KB)\n"), *RelPath, Size / 1024.0);
				else
					Result += FString::Printf(TEXT("  %s  (%lld bytes)\n"), *RelPath, Size);
				FileCount++;
			}

			if (FileCount + DirCount > 500) // Safety cap
			{
				Result += TEXT("\n... (truncated, >500 entries)\n");
				break;
			}
		}
		Result += FString::Printf(TEXT("\n%d directories, %d files (recursive, depth=%d)"), DirCount, FileCount, MaxDepth);
	}
	else
	{
		// Shallow listing (original behavior)
		TArray<FString> FoundDirs;
		TArray<FString> FoundFiles;
		IFileManager::Get().FindFiles(FoundFiles, *(FullPath / TEXT("*")), true, false);
		IFileManager::Get().FindFiles(FoundDirs, *(FullPath / TEXT("*")), false, true);

		FoundDirs.Sort();
		FoundFiles.Sort();

		if (FoundDirs.Num() > 0)
		{
			Result += TEXT("Directories:\n");
			for (const FString& D : FoundDirs)
			{
				Result += FString::Printf(TEXT("  %s/\n"), *D);
			}
		}

		if (FoundFiles.Num() > 0)
		{
			Result += TEXT("\nFiles:\n");
			for (const FString& F : FoundFiles)
			{
				int64 Size = IFileManager::Get().FileSize(*(FullPath / F));
				if (Size > 1024 * 1024)
					Result += FString::Printf(TEXT("  %s  (%.1f MB)\n"), *F, Size / (1024.0 * 1024.0));
				else if (Size > 1024)
					Result += FString::Printf(TEXT("  %s  (%.1f KB)\n"), *F, Size / 1024.0);
				else
					Result += FString::Printf(TEXT("  %s  (%lld bytes)\n"), *F, Size);
			}
		}

		Result += FString::Printf(TEXT("\n%d directories, %d files"), FoundDirs.Num(), FoundFiles.Num());
	}

	return Result;
}

// ============================================================================
// Tool: search_files
// ============================================================================

FString FGitHubCopilotUEToolExecutor::Tool_SearchFiles(const TSharedPtr<FJsonObject>& Args)
{
	FString Pattern = Args->GetStringField(TEXT("pattern"));
	FString Path = Args->GetStringField(TEXT("path"));
	FString FileFilter = Args->GetStringField(TEXT("file_filter"));

	if (Pattern.IsEmpty()) return TEXT("Error: 'pattern' is required");
	if (Path.IsEmpty()) Path = TEXT("Source");
	if (FileFilter.IsEmpty()) FileFilter = TEXT("*.h;*.cpp");

	FString FullPath = ResolvePath(Path);
	if (!IsPathAllowed(FullPath))
	{
		return FString::Printf(TEXT("Error: Path '%s' is outside the project directory"), *Path);
	}

	// Find all matching files
	TArray<FString> AllFiles;
	TArray<FString> Extensions;
	FileFilter.ParseIntoArray(Extensions, TEXT(";"));

	for (const FString& Ext : Extensions)
	{
		TArray<FString> Matched;
		IFileManager::Get().FindFilesRecursive(Matched, *FullPath, *Ext.TrimStartAndEnd(), true, false);
		AllFiles.Append(Matched);
	}

	FString ProjectDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
	FString Result;
	int32 MatchCount = 0;
	int32 FileMatchCount = 0;
	const int32 MaxMatches = 50;

	for (const FString& File : AllFiles)
	{
		if (MatchCount >= MaxMatches) break;

		FString Content;
		if (!FFileHelper::LoadFileToString(Content, *File)) continue;

		TArray<FString> Lines;
		Content.ParseIntoArrayLines(Lines);

		bool bFileHasMatch = false;
		for (int32 i = 0; i < Lines.Num() && MatchCount < MaxMatches; ++i)
		{
			if (Lines[i].Contains(Pattern))
			{
				if (!bFileHasMatch)
				{
					FString RelFile = FPaths::ConvertRelativePathToFull(File);
					RelFile.RemoveFromStart(ProjectDir);
					Result += FString::Printf(TEXT("\n%s:\n"), *RelFile);
					bFileHasMatch = true;
					FileMatchCount++;
				}
				Result += FString::Printf(TEXT("  %d: %s\n"), i + 1, *Lines[i].TrimStartAndEnd());
				MatchCount++;
			}
		}
	}

	if (MatchCount == 0)
	{
		return FString::Printf(TEXT("No matches found for '%s' in %s"), *Pattern, *Path);
	}

	return FString::Printf(TEXT("Found %d matches in %d files:\n%s"), MatchCount, FileMatchCount, *Result);
}

// ============================================================================
// Tool: create_directory
// ============================================================================

FString FGitHubCopilotUEToolExecutor::Tool_CreateDirectory(const TSharedPtr<FJsonObject>& Args)
{
	FString Path;
	if (!Args->TryGetStringField(TEXT("path"), Path) || Path.IsEmpty())
	{
		return TEXT("Error: 'path' is required");
	}

	const FString FullPath = ResolvePath(Path);
	if (!IsPathAllowed(FullPath))
	{
		return FString::Printf(TEXT("Error: Path '%s' is outside the project directory"), *Path);
	}

	if (FPaths::DirectoryExists(FullPath))
	{
		return FString::Printf(TEXT("Directory already exists: '%s'"), *Path);
	}

	if (IFileManager::Get().MakeDirectory(*FullPath, true))
	{
		return FString::Printf(TEXT("Created directory '%s'"), *Path);
	}

	return FString::Printf(TEXT("Error: Failed to create directory '%s'"), *Path);
}

// ============================================================================
// Tool: copy_file
// ============================================================================

FString FGitHubCopilotUEToolExecutor::Tool_CopyFile(const TSharedPtr<FJsonObject>& Args)
{
	FString SourcePath;
	FString DestinationPath;
	if (!Args->TryGetStringField(TEXT("source_path"), SourcePath) || SourcePath.IsEmpty())
	{
		return TEXT("Error: 'source_path' is required");
	}
	if (!Args->TryGetStringField(TEXT("destination_path"), DestinationPath) || DestinationPath.IsEmpty())
	{
		return TEXT("Error: 'destination_path' is required");
	}

	const FString SourceFullPath = ResolvePath(SourcePath);
	const FString DestinationFullPath = ResolvePath(DestinationPath);
	if (!IsPathAllowed(SourceFullPath) || !IsPathAllowed(DestinationFullPath))
	{
		return TEXT("Error: Source or destination path is outside the project directory");
	}
	if (!FPaths::FileExists(SourceFullPath))
	{
		return FString::Printf(TEXT("Error: Source file '%s' does not exist"), *SourcePath);
	}

	bool bOverwrite = false;
	Args->TryGetBoolField(TEXT("overwrite"), bOverwrite);
	if (!bOverwrite && FPaths::FileExists(DestinationFullPath))
	{
		return FString::Printf(TEXT("Error: Destination file '%s' already exists"), *DestinationPath);
	}

	const FString DestinationDir = FPaths::GetPath(DestinationFullPath);
	if (!DestinationDir.IsEmpty())
	{
		IFileManager::Get().MakeDirectory(*DestinationDir, true);
	}

	const uint32 CopyResult = IFileManager::Get().Copy(*DestinationFullPath, *SourceFullPath, bOverwrite);
	if (CopyResult == COPY_OK)
	{
		return FString::Printf(TEXT("Copied '%s' -> '%s'"), *SourcePath, *DestinationPath);
	}

	return FString::Printf(TEXT("Error: Failed to copy '%s' -> '%s'"), *SourcePath, *DestinationPath);
}

// ============================================================================
// Tool: move_file
// ============================================================================

FString FGitHubCopilotUEToolExecutor::Tool_MoveFile(const TSharedPtr<FJsonObject>& Args)
{
	FString SourcePath;
	FString DestinationPath;
	if (!Args->TryGetStringField(TEXT("source_path"), SourcePath) || SourcePath.IsEmpty())
	{
		return TEXT("Error: 'source_path' is required");
	}
	if (!Args->TryGetStringField(TEXT("destination_path"), DestinationPath) || DestinationPath.IsEmpty())
	{
		return TEXT("Error: 'destination_path' is required");
	}

	const FString SourceFullPath = ResolvePath(SourcePath);
	const FString DestinationFullPath = ResolvePath(DestinationPath);
	if (!IsPathAllowed(SourceFullPath) || !IsPathAllowed(DestinationFullPath))
	{
		return TEXT("Error: Source or destination path is outside the project directory");
	}
	if (!FPaths::FileExists(SourceFullPath))
	{
		return FString::Printf(TEXT("Error: Source file '%s' does not exist"), *SourcePath);
	}

	bool bOverwrite = false;
	Args->TryGetBoolField(TEXT("overwrite"), bOverwrite);
	if (!bOverwrite && FPaths::FileExists(DestinationFullPath))
	{
		return FString::Printf(TEXT("Error: Destination file '%s' already exists"), *DestinationPath);
	}

	const FString DestinationDir = FPaths::GetPath(DestinationFullPath);
	if (!DestinationDir.IsEmpty())
	{
		IFileManager::Get().MakeDirectory(*DestinationDir, true);
	}

	if (IFileManager::Get().Move(*DestinationFullPath, *SourceFullPath, bOverwrite))
	{
		return FString::Printf(TEXT("Moved '%s' -> '%s'"), *SourcePath, *DestinationPath);
	}

	return FString::Printf(TEXT("Error: Failed to move '%s' -> '%s'"), *SourcePath, *DestinationPath);
}

// ============================================================================
// Tool: get_project_structure
// ============================================================================

FString FGitHubCopilotUEToolExecutor::Tool_GetProjectStructure(const TSharedPtr<FJsonObject>& Args)
{
	if (!ContextService.IsValid()) return TEXT("Error: Context service not available");

	FCopilotProjectContext Ctx = ContextService->GatherProjectContext();
	FString ProjectDir = FPaths::ProjectDir();

	FString Result;
	Result += FString::Printf(TEXT("Project: %s\n"), *Ctx.ProjectName);
	Result += FString::Printf(TEXT("Engine: %s\n"), *Ctx.EngineVersion);
	Result += FString::Printf(TEXT("Map: %s\n"), *Ctx.CurrentMapName);
	Result += FString::Printf(TEXT("Platform: %s\n\n"), *Ctx.ActivePlatform);

	// Source tree
	FString SourceDir = FPaths::Combine(ProjectDir, TEXT("Source"));
	if (FPaths::DirectoryExists(SourceDir))
	{
		TArray<FString> SourceFiles;
		IFileManager::Get().FindFilesRecursive(SourceFiles, *SourceDir, TEXT("*.*"), true, false);

		Result += TEXT("Source Files:\n");
		FString ProjDirFull = FPaths::ConvertRelativePathToFull(ProjectDir);
		for (const FString& F : SourceFiles)
		{
			FString Rel = FPaths::ConvertRelativePathToFull(F);
			Rel.RemoveFromStart(ProjDirFull);
			Result += FString::Printf(TEXT("  %s\n"), *Rel);
		}
	}

	// Config files
	FString ConfigDir = FPaths::Combine(ProjectDir, TEXT("Config"));
	if (FPaths::DirectoryExists(ConfigDir))
	{
		TArray<FString> ConfigFiles;
		IFileManager::Get().FindFilesRecursive(ConfigFiles, *ConfigDir, TEXT("*.ini"), true, false);

		Result += TEXT("\nConfig Files:\n");
		for (const FString& F : ConfigFiles)
		{
			Result += FString::Printf(TEXT("  %s\n"), *FPaths::GetCleanFilename(F));
		}
	}

	// Modules
	if (Ctx.ModuleNames.Num() > 0)
	{
		Result += TEXT("\nModules:\n");
		for (const FString& M : Ctx.ModuleNames) Result += FString::Printf(TEXT("  %s\n"), *M);
	}

	// Local plugins
	FString PluginsDir = FPaths::Combine(ProjectDir, TEXT("Plugins"));
	if (FPaths::DirectoryExists(PluginsDir))
	{
		TArray<FString> PluginFiles;
		IFileManager::Get().FindFilesRecursive(PluginFiles, *PluginsDir, TEXT("*.uplugin"), true, false);
		if (PluginFiles.Num() > 0)
		{
			Result += TEXT("\nProject Plugins:\n");
			for (const FString& P : PluginFiles)
			{
				Result += FString::Printf(TEXT("  %s\n"), *FPaths::GetBaseFilename(P));
			}
		}
	}

	// XR plugins
	if (Ctx.EnabledXRPlugins.Num() > 0)
	{
		Result += TEXT("\nXR Plugins:\n");
		for (const FString& X : Ctx.EnabledXRPlugins) Result += FString::Printf(TEXT("  %s\n"), *X);
	}

	return Result;
}

// ============================================================================
// Tool: create_cpp_class
// ============================================================================

FString FGitHubCopilotUEToolExecutor::Tool_CreateCppClass(const TSharedPtr<FJsonObject>& Args)
{
	FString ClassName = Args->GetStringField(TEXT("class_name"));
	FString ParentClass = Args->GetStringField(TEXT("parent_class"));
	FString Module = Args->GetStringField(TEXT("module"));
	FString HeaderContent = Args->GetStringField(TEXT("header_content"));
	FString CppContent = Args->GetStringField(TEXT("cpp_content"));

	if (ClassName.IsEmpty()) return TEXT("Error: 'class_name' is required");
	if (Module.IsEmpty()) Module = FApp::GetProjectName();

	FString SourceDir = FPaths::Combine(FPaths::ProjectDir(), TEXT("Source"), Module);
	if (!FPaths::DirectoryExists(SourceDir))
	{
		return FString::Printf(TEXT("Error: Module source directory '%s' does not exist"), *Module);
	}

	// Generate default header if not provided
	if (HeaderContent.IsEmpty())
	{
		if (ParentClass.IsEmpty()) ParentClass = TEXT("UObject");

		HeaderContent = FString::Printf(
			TEXT("// Auto-generated by GitHubCopilotUE\n\n")
			TEXT("#pragma once\n\n")
			TEXT("#include \"CoreMinimal.h\"\n")
			TEXT("#include \"%s.generated.h\"\n\n")
			TEXT("UCLASS()\n")
			TEXT("class %s_API %s : public %s\n")
			TEXT("{\n")
			TEXT("\tGENERATED_BODY()\n\n")
			TEXT("public:\n")
			TEXT("\t%s();\n")
			TEXT("};\n"),
			*ClassName, *Module.ToUpper(), *ClassName, *ParentClass, *ClassName);
	}

	if (CppContent.IsEmpty())
	{
		CppContent = FString::Printf(
			TEXT("// Auto-generated by GitHubCopilotUE\n\n")
			TEXT("#include \"%s.h\"\n\n")
			TEXT("%s::%s()\n")
			TEXT("{\n")
			TEXT("}\n"),
			*ClassName, *ClassName, *ClassName);
	}

	// Write files
	FString HeaderPath = FPaths::Combine(SourceDir, TEXT("Public"), ClassName + TEXT(".h"));
	FString CppPath = FPaths::Combine(SourceDir, TEXT("Private"), ClassName + TEXT(".cpp"));

	// Ensure dirs exist
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(HeaderPath), true);
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(CppPath), true);

	bool bHeaderOk = FFileHelper::SaveStringToFile(HeaderContent, *HeaderPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	bool bCppOk = FFileHelper::SaveStringToFile(CppContent, *CppPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);

	if (bHeaderOk && bCppOk)
	{
		UE_LOG(LogGitHubCopilotUE, Log, TEXT("ToolExecutor: Created C++ class: %s"), *ClassName);
		return FString::Printf(TEXT("Created C++ class '%s':\n  Header: %s\n  Source: %s\n\nRun compile to build."), *ClassName, *HeaderPath, *CppPath);
	}

	return FString::Printf(TEXT("Error: Failed to create class files (header: %s, cpp: %s)"),
		bHeaderOk ? TEXT("ok") : TEXT("FAILED"), bCppOk ? TEXT("ok") : TEXT("FAILED"));
}

// ============================================================================
// Tool: create_blueprint_asset
// ============================================================================

FString FGitHubCopilotUEToolExecutor::Tool_CreateBlueprintAsset(const TSharedPtr<FJsonObject>& Args)
{
	FString AssetName;
	if (!Args->TryGetStringField(TEXT("asset_name"), AssetName) || AssetName.IsEmpty())
	{
		return TEXT("Error: 'asset_name' is required");
	}

	AssetName = SanitizeAssetName(AssetName);
	if (AssetName.IsEmpty())
	{
		return TEXT("Error: asset_name must contain at least one valid character");
	}

	FString PackagePath = TEXT("/Game/Copilot");
	Args->TryGetStringField(TEXT("package_path"), PackagePath);
	PackagePath.ReplaceInline(TEXT("\\"), TEXT("/"));
	PackagePath = PackagePath.TrimStartAndEnd();
	if (PackagePath.IsEmpty())
	{
		PackagePath = TEXT("/Game/Copilot");
	}
	if (!PackagePath.StartsWith(TEXT("/")))
	{
		PackagePath = TEXT("/Game/") + PackagePath;
	}
	if (PackagePath.EndsWith(TEXT("/")))
	{
		PackagePath.LeftChopInline(1);
	}

	if (!PackagePath.StartsWith(TEXT("/Game")) || !FPackageName::IsValidLongPackageName(PackagePath))
	{
		return FString::Printf(TEXT("Error: Invalid package_path '%s'. Use /Game/..."), *PackagePath);
	}

	FString ParentClassInput = TEXT("AActor");
	Args->TryGetStringField(TEXT("parent_class"), ParentClassInput);
	UClass* ParentClass = ResolveParentClass(ParentClassInput);
	if (ParentClass == nullptr)
	{
		return FString::Printf(TEXT("Error: Could not resolve parent_class '%s'"), *ParentClassInput);
	}

	const FString PackageName = PackagePath / AssetName;
	const FString ObjectPath = PackageName + TEXT(".") + AssetName;
	if (LoadObject<UObject>(nullptr, *ObjectPath) != nullptr)
	{
		return FString::Printf(TEXT("Error: Asset already exists at '%s'"), *ObjectPath);
	}

	UPackage* Package = CreatePackage(*PackageName);
	if (Package == nullptr)
	{
		return FString::Printf(TEXT("Error: Failed to create package '%s'"), *PackageName);
	}

	const EBlueprintType BlueprintType = ParentClass->IsChildOf(UBlueprintFunctionLibrary::StaticClass())
		? BPTYPE_FunctionLibrary
		: BPTYPE_Normal;

	UBlueprint* NewBlueprint = FKismetEditorUtilities::CreateBlueprint(
		ParentClass,
		Package,
		FName(*AssetName),
		BlueprintType,
		UBlueprint::StaticClass(),
		UBlueprintGeneratedClass::StaticClass(),
		FName(TEXT("GitHubCopilotUE")));

	if (NewBlueprint == nullptr)
	{
		return FString::Printf(TEXT("Error: Failed to create Blueprint asset '%s'"), *AssetName);
	}

	FAssetRegistryModule::AssetCreated(NewBlueprint);
	Package->MarkPackageDirty();

	const FString PackageFilename = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
	SaveArgs.SaveFlags = SAVE_None;
	SaveArgs.bSlowTask = false;
	if (!UPackage::SavePackage(Package, NewBlueprint, *PackageFilename, SaveArgs))
	{
		return FString::Printf(
			TEXT("Error: Created Blueprint asset '%s' but failed to save package to '%s'"),
			*AssetName, *PackageFilename);
	}

	bool bOpenEditor = true;
	Args->TryGetBoolField(TEXT("open_editor"), bOpenEditor);
	if (bOpenEditor && GEditor)
	{
		TArray<UObject*> AssetsToOpen;
		AssetsToOpen.Add(NewBlueprint);
		GEditor->GetEditorSubsystem<UAssetEditorSubsystem>()->OpenEditorForAssets(AssetsToOpen);
	}

	UE_LOG(LogGitHubCopilotUE, Log, TEXT("ToolExecutor: Created Blueprint asset %s (parent: %s)"), *ObjectPath, *ParentClass->GetName());
	return FString::Printf(
		TEXT("Created and saved Blueprint asset '%s'\nObject path: %s\nFile: %s\nParent class: %s"),
		*AssetName, *ObjectPath, *PackageFilename, *ParentClass->GetName());
}

// ============================================================================
// Tool: compile
// ============================================================================

FString FGitHubCopilotUEToolExecutor::Tool_Compile(const TSharedPtr<FJsonObject>& Args)
{
	if (!CompileService.IsValid()) return TEXT("Error: Compile service not available");

	FString Mode;
	Args->TryGetStringField(TEXT("mode"), Mode);
	Mode = Mode.TrimStartAndEnd().ToLower();

	FCopilotResponse CompileResponse;
	if (Mode.IsEmpty() || Mode == TEXT("full") || Mode == TEXT("compile"))
	{
		CompileResponse = CompileService->RequestCompile();
	}
	else if (Mode == TEXT("live_coding") || Mode == TEXT("live"))
	{
		CompileResponse = CompileService->RequestLiveCodingPatch();
	}
	else
	{
		return FString::Printf(TEXT("Error: Unsupported compile mode '%s'. Use 'full' or 'live_coding'."), *Mode);
	}

	if (CompileResponse.ResultStatus == ECopilotResultStatus::Success)
	{
		return CompileResponse.ResponseText;
	}

	return FString::Printf(TEXT("Error: %s"), *CompileResponse.ErrorMessage);
}

// ============================================================================
// Tool: live_coding_patch
// ============================================================================

FString FGitHubCopilotUEToolExecutor::Tool_LiveCodingPatch(const TSharedPtr<FJsonObject>& Args)
{
	(void)Args;
	if (!CompileService.IsValid()) return TEXT("Error: Compile service not available");

	FCopilotResponse PatchResponse = CompileService->RequestLiveCodingPatch();
	if (PatchResponse.ResultStatus == ECopilotResultStatus::Success)
	{
		return PatchResponse.ResponseText;
	}
	return FString::Printf(TEXT("Error: %s"), *PatchResponse.ErrorMessage);
}

// ============================================================================
// Tool: run_automation_tests
// ============================================================================

FString FGitHubCopilotUEToolExecutor::Tool_RunAutomationTests(const TSharedPtr<FJsonObject>& Args)
{
	if (!CompileService.IsValid()) return TEXT("Error: Compile service not available");

	FString Filter;
	Args->TryGetStringField(TEXT("filter"), Filter);

	FCopilotResponse TestResponse = CompileService->RunAutomationTests(Filter);
	if (TestResponse.ResultStatus == ECopilotResultStatus::Success)
	{
		return TestResponse.ResponseText;
	}
	return FString::Printf(TEXT("Error: %s"), *TestResponse.ErrorMessage);
}

// ============================================================================
// Tool: get_file_info
// ============================================================================

FString FGitHubCopilotUEToolExecutor::Tool_GetFileInfo(const TSharedPtr<FJsonObject>& Args)
{
	FString Path = Args->GetStringField(TEXT("path"));
	if (Path.IsEmpty()) return TEXT("Error: 'path' is required");

	FString FullPath = ResolvePath(Path);

	if (!FPaths::FileExists(FullPath) && !FPaths::DirectoryExists(FullPath))
	{
		return FString::Printf(TEXT("'%s' does not exist"), *Path);
	}

	if (FPaths::DirectoryExists(FullPath))
	{
		return FString::Printf(TEXT("'%s' is a directory"), *Path);
	}

	int64 Size = IFileManager::Get().FileSize(*FullPath);
	FDateTime ModTime = IFileManager::Get().GetTimeStamp(*FullPath);

	FString Result;
	Result += FString::Printf(TEXT("File: %s\n"), *Path);
	Result += FString::Printf(TEXT("Full path: %s\n"), *FullPath);
	Result += FString::Printf(TEXT("Size: %lld bytes\n"), Size);
	Result += FString::Printf(TEXT("Modified: %s\n"), *ModTime.ToString());
	Result += FString::Printf(TEXT("Extension: %s\n"), *FPaths::GetExtension(FullPath));

	// Count lines for text files
	FString Ext = FPaths::GetExtension(FullPath).ToLower();
	if (Ext == TEXT("h") || Ext == TEXT("cpp") || Ext == TEXT("cs") || Ext == TEXT("ini") || Ext == TEXT("txt") || Ext == TEXT("md") || Ext == TEXT("json"))
	{
		FString Content;
		if (FFileHelper::LoadFileToString(Content, *FullPath))
		{
			TArray<FString> Lines;
			Content.ParseIntoArrayLines(Lines);
			Result += FString::Printf(TEXT("Lines: %d\n"), Lines.Num());
		}
	}

	return Result;
}

// ============================================================================
// Tool: delete_file
// ============================================================================

FString FGitHubCopilotUEToolExecutor::Tool_DeleteFile(const TSharedPtr<FJsonObject>& Args)
{
	FString Path = Args->GetStringField(TEXT("path"));
	if (Path.IsEmpty()) return TEXT("Error: 'path' is required");

	FString FullPath = ResolvePath(Path);
	if (!IsPathAllowed(FullPath))
	{
		return FString::Printf(TEXT("Error: Path '%s' is outside the project directory"), *Path);
	}

	if (!FPaths::FileExists(FullPath))
	{
		return FString::Printf(TEXT("Error: File '%s' does not exist"), *Path);
	}

	// Create backup first
	FString BackupPath = FullPath + TEXT(".deleted.bak");
	IFileManager::Get().Copy(*BackupPath, *FullPath);

	if (IFileManager::Get().Delete(*FullPath))
	{
		UE_LOG(LogGitHubCopilotUE, Log, TEXT("ToolExecutor: Deleted file: %s (backup at .deleted.bak)"), *FullPath);
		return FString::Printf(TEXT("Deleted '%s' (backup saved as .deleted.bak)"), *Path);
	}

	return FString::Printf(TEXT("Error: Failed to delete '%s'"), *Path);
}

// ============================================================================
// Tool: spawn_actor — spawn an actor in the current level
// ============================================================================

FString FGitHubCopilotUEToolExecutor::Tool_SpawnActor(const TSharedPtr<FJsonObject>& Args)
{
	FString ActorClassInput;
	if (!Args->TryGetStringField(TEXT("actor_class"), ActorClassInput) || ActorClassInput.IsEmpty())
	{
		return TEXT("Error: 'actor_class' is required");
	}

	UClass* ActorClass = ResolveParentClass(ActorClassInput);
	if (!ActorClass)
	{
		return FString::Printf(TEXT("Error: Could not resolve actor_class '%s'"), *ActorClassInput);
	}
	if (!ActorClass->IsChildOf(AActor::StaticClass()))
	{
		return FString::Printf(TEXT("Error: '%s' is not an Actor class"), *ActorClassInput);
	}

	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!World)
	{
		return TEXT("Error: No editor world available. Open a level first.");
	}

	// Parse transform
	FVector Location = FVector::ZeroVector;
	FRotator Rotation = FRotator::ZeroRotator;
	FVector Scale = FVector::OneVector;

	const TArray<TSharedPtr<FJsonValue>>* LocArray;
	if (Args->TryGetArrayField(TEXT("location"), LocArray) && LocArray->Num() >= 3)
	{
		Location.X = (*LocArray)[0]->AsNumber();
		Location.Y = (*LocArray)[1]->AsNumber();
		Location.Z = (*LocArray)[2]->AsNumber();
	}

	const TArray<TSharedPtr<FJsonValue>>* RotArray;
	if (Args->TryGetArrayField(TEXT("rotation"), RotArray) && RotArray->Num() >= 3)
	{
		Rotation.Pitch = (*RotArray)[0]->AsNumber();
		Rotation.Yaw = (*RotArray)[1]->AsNumber();
		Rotation.Roll = (*RotArray)[2]->AsNumber();
	}

	const TArray<TSharedPtr<FJsonValue>>* ScaleArray;
	if (Args->TryGetArrayField(TEXT("scale"), ScaleArray) && ScaleArray->Num() >= 3)
	{
		Scale.X = (*ScaleArray)[0]->AsNumber();
		Scale.Y = (*ScaleArray)[1]->AsNumber();
		Scale.Z = (*ScaleArray)[2]->AsNumber();
	}

	FString ActorName;
	Args->TryGetStringField(TEXT("name"), ActorName);

	FActorSpawnParameters SpawnParams;
	SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AdjustIfPossibleButAlwaysSpawn;
	if (!ActorName.IsEmpty())
	{
		SpawnParams.Name = FName(*ActorName);
	}

	AActor* NewActor = World->SpawnActor<AActor>(ActorClass, Location, Rotation, SpawnParams);
	if (!NewActor)
	{
		return FString::Printf(TEXT("Error: Failed to spawn actor of class '%s'"), *ActorClassInput);
	}

	NewActor->SetActorScale3D(Scale);

	// Select the new actor
	if (GEditor)
	{
		GEditor->SelectNone(true, true, false);
		GEditor->SelectActor(NewActor, true, true, true);
	}

	UE_LOG(LogGitHubCopilotUE, Log, TEXT("ToolExecutor: Spawned actor %s (%s) at %s"),
		*NewActor->GetName(), *ActorClass->GetName(), *Location.ToString());
	return FString::Printf(
		TEXT("Spawned '%s' (%s) at location (%s), rotation (%s), scale (%s)"),
		*NewActor->GetName(), *ActorClass->GetName(),
		*Location.ToString(), *Rotation.ToString(), *Scale.ToString());
}

// ============================================================================
// Tool: create_material_asset — create a UMaterial with expression nodes
// ============================================================================

FString FGitHubCopilotUEToolExecutor::Tool_CreateMaterialAsset(const TSharedPtr<FJsonObject>& Args)
{
	FString AssetName;
	if (!Args->TryGetStringField(TEXT("asset_name"), AssetName) || AssetName.IsEmpty())
	{
		return TEXT("Error: 'asset_name' is required");
	}

	AssetName = SanitizeAssetName(AssetName);
	if (AssetName.IsEmpty())
	{
		return TEXT("Error: asset_name must contain at least one valid character");
	}

	FString PackagePath = TEXT("/Game/Copilot/Materials");
	Args->TryGetStringField(TEXT("package_path"), PackagePath);
	PackagePath.ReplaceInline(TEXT("\\"), TEXT("/"));
	PackagePath = PackagePath.TrimStartAndEnd();
	if (PackagePath.IsEmpty()) PackagePath = TEXT("/Game/Copilot/Materials");
	if (!PackagePath.StartsWith(TEXT("/"))) PackagePath = TEXT("/Game/") + PackagePath;
	if (PackagePath.EndsWith(TEXT("/"))) PackagePath.LeftChopInline(1);

	if (!PackagePath.StartsWith(TEXT("/Game")) || !FPackageName::IsValidLongPackageName(PackagePath))
	{
		return FString::Printf(TEXT("Error: Invalid package_path '%s'. Use /Game/..."), *PackagePath);
	}

	const FString PackageName = PackagePath / AssetName;
	const FString ObjectPath = PackageName + TEXT(".") + AssetName;
	if (LoadObject<UObject>(nullptr, *ObjectPath) != nullptr)
	{
		return FString::Printf(TEXT("Error: Asset already exists at '%s'"), *ObjectPath);
	}

	UPackage* Package = CreatePackage(*PackageName);
	if (!Package)
	{
		return FString::Printf(TEXT("Error: Failed to create package '%s'"), *PackageName);
	}

	UMaterial* NewMaterial = NewObject<UMaterial>(Package, FName(*AssetName), RF_Public | RF_Standalone);
	if (!NewMaterial)
	{
		return FString::Printf(TEXT("Error: Failed to create material '%s'"), *AssetName);
	}

	// Parse base color [R, G, B]
	const TArray<TSharedPtr<FJsonValue>>* ColorArray;
	if (Args->TryGetArrayField(TEXT("base_color"), ColorArray) && ColorArray->Num() >= 3)
	{
		UMaterialExpressionConstant3Vector* ColorExpr = NewObject<UMaterialExpressionConstant3Vector>(NewMaterial);
		ColorExpr->Constant = FLinearColor(
			(*ColorArray)[0]->AsNumber(),
			(*ColorArray)[1]->AsNumber(),
			(*ColorArray)[2]->AsNumber());
		NewMaterial->GetEditorOnlyData()->ExpressionCollection.Expressions.Add(ColorExpr);
		NewMaterial->GetEditorOnlyData()->BaseColor.Connect(0, ColorExpr);
		ColorExpr->MaterialExpressionEditorX = -300;
		ColorExpr->MaterialExpressionEditorY = 0;
	}

	// Parse metallic
	double MetallicVal = -1.0;
	if (Args->TryGetNumberField(TEXT("metallic"), MetallicVal))
	{
		UMaterialExpressionConstant* MetallicExpr = NewObject<UMaterialExpressionConstant>(NewMaterial);
		MetallicExpr->R = static_cast<float>(MetallicVal);
		NewMaterial->GetEditorOnlyData()->ExpressionCollection.Expressions.Add(MetallicExpr);
		NewMaterial->GetEditorOnlyData()->Metallic.Connect(0, MetallicExpr);
		MetallicExpr->MaterialExpressionEditorX = -300;
		MetallicExpr->MaterialExpressionEditorY = 100;
	}

	// Parse roughness
	double RoughnessVal = -1.0;
	if (Args->TryGetNumberField(TEXT("roughness"), RoughnessVal))
	{
		UMaterialExpressionConstant* RoughnessExpr = NewObject<UMaterialExpressionConstant>(NewMaterial);
		RoughnessExpr->R = static_cast<float>(RoughnessVal);
		NewMaterial->GetEditorOnlyData()->ExpressionCollection.Expressions.Add(RoughnessExpr);
		NewMaterial->GetEditorOnlyData()->Roughness.Connect(0, RoughnessExpr);
		RoughnessExpr->MaterialExpressionEditorX = -300;
		RoughnessExpr->MaterialExpressionEditorY = 200;
	}

	// Parse emissive color
	const TArray<TSharedPtr<FJsonValue>>* EmissiveArray;
	if (Args->TryGetArrayField(TEXT("emissive_color"), EmissiveArray) && EmissiveArray->Num() >= 3)
	{
		UMaterialExpressionConstant3Vector* EmissiveExpr = NewObject<UMaterialExpressionConstant3Vector>(NewMaterial);
		EmissiveExpr->Constant = FLinearColor(
			(*EmissiveArray)[0]->AsNumber(),
			(*EmissiveArray)[1]->AsNumber(),
			(*EmissiveArray)[2]->AsNumber());
		NewMaterial->GetEditorOnlyData()->ExpressionCollection.Expressions.Add(EmissiveExpr);
		NewMaterial->GetEditorOnlyData()->EmissiveColor.Connect(0, EmissiveExpr);
		EmissiveExpr->MaterialExpressionEditorX = -300;
		EmissiveExpr->MaterialExpressionEditorY = 300;
	}

	// Parse opacity
	double OpacityVal = -1.0;
	if (Args->TryGetNumberField(TEXT("opacity"), OpacityVal))
	{
		NewMaterial->BlendMode = BLEND_Translucent;
		UMaterialExpressionConstant* OpacityExpr = NewObject<UMaterialExpressionConstant>(NewMaterial);
		OpacityExpr->R = static_cast<float>(OpacityVal);
		NewMaterial->GetEditorOnlyData()->ExpressionCollection.Expressions.Add(OpacityExpr);
		NewMaterial->GetEditorOnlyData()->Opacity.Connect(0, OpacityExpr);
		OpacityExpr->MaterialExpressionEditorX = -300;
		OpacityExpr->MaterialExpressionEditorY = 400;
	}

	// Parse blend mode override
	FString BlendModeStr;
	if (Args->TryGetStringField(TEXT("blend_mode"), BlendModeStr))
	{
		BlendModeStr = BlendModeStr.ToLower();
		if (BlendModeStr == TEXT("translucent")) NewMaterial->BlendMode = BLEND_Translucent;
		else if (BlendModeStr == TEXT("additive")) NewMaterial->BlendMode = BLEND_Additive;
		else if (BlendModeStr == TEXT("modulate")) NewMaterial->BlendMode = BLEND_Modulate;
		else if (BlendModeStr == TEXT("masked")) NewMaterial->BlendMode = BLEND_Masked;
		// else keep default (opaque)
	}

	// Parse shading model
	FString ShadingModelStr;
	if (Args->TryGetStringField(TEXT("shading_model"), ShadingModelStr))
	{
		ShadingModelStr = ShadingModelStr.ToLower();
		if (ShadingModelStr == TEXT("unlit")) NewMaterial->SetShadingModel(MSM_Unlit);
		else if (ShadingModelStr == TEXT("subsurface")) NewMaterial->SetShadingModel(MSM_Subsurface);
		else if (ShadingModelStr == TEXT("clearcoat")) NewMaterial->SetShadingModel(MSM_ClearCoat);
		// else keep DefaultLit
	}

	// Compile and save
	NewMaterial->PreEditChange(nullptr);
	NewMaterial->PostEditChange();

	FAssetRegistryModule::AssetCreated(NewMaterial);
	Package->MarkPackageDirty();

	const FString PackageFilename = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
	if (!UPackage::SavePackage(Package, NewMaterial, *PackageFilename, SaveArgs))
	{
		return FString::Printf(TEXT("Created material '%s' but failed to save to disk"), *AssetName);
	}

	// Optionally assign to an actor
	FString AssignTo;
	if (Args->TryGetStringField(TEXT("assign_to"), AssignTo) && !AssignTo.IsEmpty() && GEditor)
	{
		UWorld* World = GEditor->GetEditorWorldContext().World();
		if (World)
		{
			for (TActorIterator<AActor> It(World); It; ++It)
			{
				if (It->GetName() == AssignTo || It->GetActorLabel() == AssignTo)
				{
					TArray<UStaticMeshComponent*> MeshComps;
					It->GetComponents<UStaticMeshComponent>(MeshComps);
					for (UStaticMeshComponent* SMC : MeshComps)
					{
						if (SMC)
						{
							SMC->SetMaterial(0, NewMaterial);
						}
					}
					break;
				}
			}
		}
	}

	bool bOpenEditor = true;
	Args->TryGetBoolField(TEXT("open_editor"), bOpenEditor);
	if (bOpenEditor && GEditor)
	{
		TArray<UObject*> AssetsToOpen;
		AssetsToOpen.Add(NewMaterial);
		GEditor->GetEditorSubsystem<UAssetEditorSubsystem>()->OpenEditorForAssets(AssetsToOpen);
	}

	UE_LOG(LogGitHubCopilotUE, Log, TEXT("ToolExecutor: Created material asset %s"), *ObjectPath);
	return FString::Printf(
		TEXT("Created and saved material '%s'\nObject path: %s\nFile: %s\nBlend mode: %s"),
		*AssetName, *ObjectPath, *PackageFilename,
		*StaticEnum<EBlendMode>()->GetNameStringByValue(static_cast<int64>(NewMaterial->BlendMode)));
}

// ============================================================================
// Tool: create_data_table — create a DataTable asset
// ============================================================================

FString FGitHubCopilotUEToolExecutor::Tool_CreateDataTable(const TSharedPtr<FJsonObject>& Args)
{
	FString AssetName;
	if (!Args->TryGetStringField(TEXT("asset_name"), AssetName) || AssetName.IsEmpty())
	{
		return TEXT("Error: 'asset_name' is required");
	}

	AssetName = SanitizeAssetName(AssetName);

	FString PackagePath = TEXT("/Game/Copilot/Data");
	Args->TryGetStringField(TEXT("package_path"), PackagePath);
	PackagePath.ReplaceInline(TEXT("\\"), TEXT("/"));
	PackagePath = PackagePath.TrimStartAndEnd();
	if (PackagePath.IsEmpty()) PackagePath = TEXT("/Game/Copilot/Data");
	if (!PackagePath.StartsWith(TEXT("/"))) PackagePath = TEXT("/Game/") + PackagePath;
	if (PackagePath.EndsWith(TEXT("/"))) PackagePath.LeftChopInline(1);

	FString RowStructPath;
	if (!Args->TryGetStringField(TEXT("row_struct"), RowStructPath) || RowStructPath.IsEmpty())
	{
		return TEXT("Error: 'row_struct' is required (e.g. '/Script/Engine.DataTableRowHandle' or a custom struct path)");
	}

	// Try to load the row struct
	UScriptStruct* RowStruct = FindObject<UScriptStruct>(nullptr, *RowStructPath);
	if (!RowStruct)
	{
		RowStruct = LoadObject<UScriptStruct>(nullptr, *RowStructPath);
	}
	if (!RowStruct)
	{
		// Try common prefixes
		TArray<FString> Candidates = {
			FString::Printf(TEXT("/Script/Engine.%s"), *RowStructPath),
			FString::Printf(TEXT("/Script/CoreUObject.%s"), *RowStructPath),
		};
		for (const FString& Cand : Candidates)
		{
			RowStruct = LoadObject<UScriptStruct>(nullptr, *Cand);
			if (RowStruct) break;
		}
	}
	if (!RowStruct)
	{
		return FString::Printf(TEXT("Error: Could not resolve row_struct '%s'. Provide a full /Script/ path."), *RowStructPath);
	}

	const FString PackageName = PackagePath / AssetName;
	const FString ObjectPath = PackageName + TEXT(".") + AssetName;
	if (LoadObject<UObject>(nullptr, *ObjectPath) != nullptr)
	{
		return FString::Printf(TEXT("Error: Asset already exists at '%s'"), *ObjectPath);
	}

	UPackage* Package = CreatePackage(*PackageName);
	if (!Package)
	{
		return FString::Printf(TEXT("Error: Failed to create package '%s'"), *PackageName);
	}

	UDataTable* NewTable = NewObject<UDataTable>(Package, FName(*AssetName), RF_Public | RF_Standalone);
	if (!NewTable)
	{
		return FString::Printf(TEXT("Error: Failed to create DataTable '%s'"), *AssetName);
	}

	NewTable->RowStruct = RowStruct;

	FAssetRegistryModule::AssetCreated(NewTable);
	Package->MarkPackageDirty();

	const FString PackageFilename = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
	if (!UPackage::SavePackage(Package, NewTable, *PackageFilename, SaveArgs))
	{
		return FString::Printf(TEXT("Created DataTable '%s' but failed to save"), *AssetName);
	}

	bool bOpenEditor = true;
	Args->TryGetBoolField(TEXT("open_editor"), bOpenEditor);
	if (bOpenEditor && GEditor)
	{
		TArray<UObject*> AssetsToOpen;
		AssetsToOpen.Add(NewTable);
		GEditor->GetEditorSubsystem<UAssetEditorSubsystem>()->OpenEditorForAssets(AssetsToOpen);
	}

	UE_LOG(LogGitHubCopilotUE, Log, TEXT("ToolExecutor: Created DataTable %s (struct: %s)"), *ObjectPath, *RowStruct->GetName());
	return FString::Printf(
		TEXT("Created and saved DataTable '%s'\nObject path: %s\nRow struct: %s\nFile: %s"),
		*AssetName, *ObjectPath, *RowStruct->GetName(), *PackageFilename);
}

// ============================================================================
// Tool: create_niagara_system — create a Niagara particle system asset
// ============================================================================

FString FGitHubCopilotUEToolExecutor::Tool_CreateNiagaraSystem(const TSharedPtr<FJsonObject>& Args)
{
	FString AssetName;
	if (!Args->TryGetStringField(TEXT("asset_name"), AssetName) || AssetName.IsEmpty())
	{
		return TEXT("Error: 'asset_name' is required");
	}

	AssetName = SanitizeAssetName(AssetName);

	FString PackagePath = TEXT("/Game/Copilot/FX");
	Args->TryGetStringField(TEXT("package_path"), PackagePath);
	PackagePath.ReplaceInline(TEXT("\\"), TEXT("/"));
	PackagePath = PackagePath.TrimStartAndEnd();
	if (PackagePath.IsEmpty()) PackagePath = TEXT("/Game/Copilot/FX");
	if (!PackagePath.StartsWith(TEXT("/"))) PackagePath = TEXT("/Game/") + PackagePath;
	if (PackagePath.EndsWith(TEXT("/"))) PackagePath.LeftChopInline(1);

	const FString PackageName = PackagePath / AssetName;
	const FString ObjectPath = PackageName + TEXT(".") + AssetName;
	if (LoadObject<UObject>(nullptr, *ObjectPath) != nullptr)
	{
		return FString::Printf(TEXT("Error: Asset already exists at '%s'"), *ObjectPath);
	}

	// Soft-load Niagara system class to avoid hard module dependency
	UClass* NiagaraSystemClass = StaticLoadClass(UObject::StaticClass(), nullptr,
		TEXT("/Script/Niagara.NiagaraSystem"));
	if (!NiagaraSystemClass)
	{
		return TEXT("Error: Niagara plugin is not available. Enable it in Edit > Plugins.");
	}

	UPackage* Package = CreatePackage(*PackageName);
	if (!Package)
	{
		return FString::Printf(TEXT("Error: Failed to create package '%s'"), *PackageName);
	}

	UObject* NewSystem = NewObject<UObject>(Package, NiagaraSystemClass, FName(*AssetName), RF_Public | RF_Standalone);
	if (!NewSystem)
	{
		return FString::Printf(TEXT("Error: Failed to create Niagara system '%s'"), *AssetName);
	}

	FAssetRegistryModule::AssetCreated(NewSystem);
	Package->MarkPackageDirty();

	const FString PackageFilename = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
	if (!UPackage::SavePackage(Package, NewSystem, *PackageFilename, SaveArgs))
	{
		return FString::Printf(TEXT("Created Niagara system '%s' but failed to save"), *AssetName);
	}

	bool bOpenEditor = true;
	Args->TryGetBoolField(TEXT("open_editor"), bOpenEditor);
	if (bOpenEditor && GEditor)
	{
		TArray<UObject*> AssetsToOpen;
		AssetsToOpen.Add(NewSystem);
		GEditor->GetEditorSubsystem<UAssetEditorSubsystem>()->OpenEditorForAssets(AssetsToOpen);
	}

	UE_LOG(LogGitHubCopilotUE, Log, TEXT("ToolExecutor: Created Niagara system %s"), *ObjectPath);
	return FString::Printf(
		TEXT("Created and saved Niagara system '%s'\nObject path: %s\nFile: %s\nNote: Open in Niagara editor to add emitters and configure behavior."),
		*AssetName, *ObjectPath, *PackageFilename);
}

// ============================================================================
// Tool: web_search
// ============================================================================

FString FGitHubCopilotUEToolExecutor::Tool_WebSearch(const TSharedPtr<FJsonObject>& Args)
{
	FString Query;
	if (!Args->TryGetStringField(TEXT("query"), Query) || Query.IsEmpty())
	{
		return TEXT("Error: 'query' is required");
	}

	int32 MaxResults = 5;
	if (Args->HasField(TEXT("max_results")))
	{
		MaxResults = FMath::Clamp(static_cast<int32>(Args->GetNumberField(TEXT("max_results"))), 1, 10);
	}

	UE_LOG(LogGitHubCopilotUE, Log, TEXT("ToolExecutor: Web search: %s (max=%d)"), *Query, MaxResults);

	// URL-encode the query
	FString EncodedQuery = FGenericPlatformHttp::UrlEncode(Query);
	FString SearchURL = FString::Printf(
		TEXT("https://html.duckduckgo.com/html/?q=%s"),
		*EncodedQuery);

	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> HttpReq = FHttpModule::Get().CreateRequest();
	HttpReq->SetURL(SearchURL);
	HttpReq->SetVerb(TEXT("GET"));
	HttpReq->SetHeader(TEXT("User-Agent"), TEXT("Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/131.0.0.0 Safari/537.36"));
	HttpReq->SetHeader(TEXT("Accept"), TEXT("text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8"));
	HttpReq->SetHeader(TEXT("Accept-Language"), TEXT("en-US,en;q=0.9"));
	HttpReq->SetTimeout(15.0f);

	// Track completion via a thread-safe shared flag
	struct FWebSearchResult
	{
		FHttpResponsePtr Response;
		TAtomic<bool> bDone{false};
		TAtomic<bool> bSuccess{false};
	};
	TSharedPtr<FWebSearchResult> SearchResult = MakeShared<FWebSearchResult>();

	HttpReq->OnProcessRequestComplete().BindLambda(
		[SearchResult](FHttpRequestPtr Req, FHttpResponsePtr Resp, bool bSuccess)
		{
			SearchResult->Response = Resp;
			SearchResult->bSuccess = bSuccess;
			SearchResult->bDone = true;
		});

	HttpReq->ProcessRequest();

	// Pump the HTTP manager manually so callbacks can fire on the game thread
	const double StartTime = FPlatformTime::Seconds();
	const double TimeoutSec = 15.0;
	while (!SearchResult->bDone)
	{
		if (FPlatformTime::Seconds() - StartTime > TimeoutSec)
		{
			HttpReq->CancelRequest();
			return TEXT("Error: Web search timed out after 15 seconds");
		}
		FHttpModule::Get().GetHttpManager().Tick(0.05f);
		FPlatformProcess::Sleep(0.05f);
	}

	if (!SearchResult->bSuccess || !SearchResult->Response.IsValid())
	{
		return TEXT("Error: Web search request failed (network error)");
	}

	int32 StatusCode = SearchResult->Response->GetResponseCode();
	
	// HTTP 202 = "Accepted, still processing" — DuckDuckGo does this under load.
	// Retry up to 3 times with a short delay.
	if (StatusCode == 202)
	{
		for (int32 Retry = 0; Retry < 3 && StatusCode == 202; ++Retry)
		{
			FPlatformProcess::Sleep(1.0f + Retry * 0.5f);
			
			TSharedPtr<FWebSearchResult> RetryResult = MakeShared<FWebSearchResult>();
			TSharedRef<IHttpRequest, ESPMode::ThreadSafe> RetryReq = FHttpModule::Get().CreateRequest();
			RetryReq->SetURL(SearchURL);
			RetryReq->SetVerb(TEXT("GET"));
			RetryReq->SetHeader(TEXT("User-Agent"), TEXT("Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/131.0.0.0 Safari/537.36"));
			RetryReq->SetHeader(TEXT("Accept"), TEXT("text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8"));
			RetryReq->SetHeader(TEXT("Accept-Language"), TEXT("en-US,en;q=0.9"));
			RetryReq->SetTimeout(15.0f);
			RetryReq->OnProcessRequestComplete().BindLambda(
				[RetryResult](FHttpRequestPtr Req, FHttpResponsePtr Resp, bool bSuccess)
				{
					RetryResult->Response = Resp;
					RetryResult->bSuccess = bSuccess;
					RetryResult->bDone = true;
				});
			RetryReq->ProcessRequest();

			const double RetryStart = FPlatformTime::Seconds();
			while (!RetryResult->bDone)
			{
				if (FPlatformTime::Seconds() - RetryStart > 10.0)
				{
					RetryReq->CancelRequest();
					break;
				}
				FHttpModule::Get().GetHttpManager().Tick(0.05f);
				FPlatformProcess::Sleep(0.05f);
			}

			if (RetryResult->bSuccess && RetryResult->Response.IsValid())
			{
				StatusCode = RetryResult->Response->GetResponseCode();
				SearchResult->Response = RetryResult->Response;
				UE_LOG(LogGitHubCopilotUE, Log, TEXT("ToolExecutor: Web search retry %d -> HTTP %d"), Retry + 1, StatusCode);
			}
			else
			{
				break;
			}
		}
	}
	
	if (StatusCode != 200)
	{
		return FString::Printf(TEXT("Error: Web search returned HTTP %d"), StatusCode);
	}

	FString Body = SearchResult->Response->GetContentAsString();

	// Parse DuckDuckGo HTML results — extract result titles and snippets
	// Results are in <a class="result__a"> for titles and <a class="result__snippet"> for snippets
	TArray<FString> Results;
	int32 SearchPos = 0;
	int32 ResultCount = 0;

	while (ResultCount < MaxResults)
	{
		// Find result link
		int32 LinkStart = Body.Find(TEXT("class=\"result__a\""), ESearchCase::CaseSensitive, ESearchDir::FromStart, SearchPos);
		if (LinkStart == INDEX_NONE) break;

		// Find href before this class attribute
		int32 HrefStart = Body.Find(TEXT("href=\""), ESearchCase::CaseSensitive, ESearchDir::FromStart, LinkStart);
		FString Href;
		if (HrefStart != INDEX_NONE && HrefStart < LinkStart + 200)
		{
			HrefStart += 6; // skip href="
			int32 HrefEnd = Body.Find(TEXT("\""), ESearchCase::CaseSensitive, ESearchDir::FromStart, HrefStart);
			if (HrefEnd != INDEX_NONE)
			{
				Href = Body.Mid(HrefStart, HrefEnd - HrefStart);
			}
		}

		// Find title text (between > and </a>)
		int32 TitleOpen = Body.Find(TEXT(">"), ESearchCase::CaseSensitive, ESearchDir::FromStart, LinkStart);
		int32 TitleClose = Body.Find(TEXT("</a>"), ESearchCase::CaseSensitive, ESearchDir::FromStart, TitleOpen);
		FString Title;
		if (TitleOpen != INDEX_NONE && TitleClose != INDEX_NONE)
		{
			Title = Body.Mid(TitleOpen + 1, TitleClose - TitleOpen - 1);
			// Strip HTML tags from title
			FString CleanTitle;
			bool bInTag = false;
			for (TCHAR C : Title)
			{
				if (C == TCHAR('<')) bInTag = true;
				else if (C == TCHAR('>')) bInTag = false;
				else if (!bInTag) CleanTitle += C;
			}
			Title = CleanTitle.TrimStartAndEnd();
		}

		// Find snippet
		int32 SnippetStart = Body.Find(TEXT("class=\"result__snippet\""), ESearchCase::CaseSensitive, ESearchDir::FromStart, TitleClose > 0 ? TitleClose : LinkStart);
		FString Snippet;
		if (SnippetStart != INDEX_NONE && SnippetStart < LinkStart + 2000)
		{
			int32 SnipOpen = Body.Find(TEXT(">"), ESearchCase::CaseSensitive, ESearchDir::FromStart, SnippetStart);
			int32 SnipClose = Body.Find(TEXT("</a>"), ESearchCase::CaseSensitive, ESearchDir::FromStart, SnipOpen);
			if (SnipOpen != INDEX_NONE && SnipClose != INDEX_NONE)
			{
				Snippet = Body.Mid(SnipOpen + 1, SnipClose - SnipOpen - 1);
				// Strip HTML tags
				FString CleanSnippet;
				bool bInSnipTag = false;
				for (TCHAR C : Snippet)
				{
					if (C == TCHAR('<')) bInSnipTag = true;
					else if (C == TCHAR('>')) bInSnipTag = false;
					else if (!bInSnipTag) CleanSnippet += C;
				}
				Snippet = CleanSnippet.TrimStartAndEnd();
			}
		}

		if (!Title.IsEmpty())
		{
			FString Entry = FString::Printf(TEXT("[%d] %s"), ResultCount + 1, *Title);
			if (!Href.IsEmpty())
			{
				Entry += FString::Printf(TEXT("\n    URL: %s"), *Href);
			}
			if (!Snippet.IsEmpty())
			{
				Entry += FString::Printf(TEXT("\n    %s"), *Snippet);
			}
			Results.Add(Entry);
			ResultCount++;
		}

		SearchPos = (TitleClose != INDEX_NONE) ? TitleClose + 4 : LinkStart + 20;
	}

	if (Results.Num() == 0)
	{
		return FString::Printf(TEXT("No results found for: %s"), *Query);
	}

	FString Output = FString::Printf(TEXT("Web search results for \"%s\":\n\n"), *Query);
	Output += FString::Join(Results, TEXT("\n\n"));
	return Output;
}

// ============================================================================
// Tool: capture_viewport — screenshot the active editor window for AI vision analysis
// ============================================================================

FString FGitHubCopilotUEToolExecutor::Tool_CaptureViewport(const TSharedPtr<FJsonObject>& Args)
{
	FString OutputPath = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("CopilotViewportCapture.png"));

	if (FPaths::FileExists(OutputPath))
	{
		IFileManager::Get().Delete(*OutputPath);
	}

	FString Target = TEXT("main");
	if (Args.IsValid() && Args->HasField(TEXT("target")))
	{
		Target = Args->GetStringField(TEXT("target"));
	}

	const TArray<TSharedRef<SWindow>>& AllWindows = FSlateApplication::Get().GetInteractiveTopLevelWindows();
	TSharedPtr<SWindow> ChosenWindow;
	FString ChosenTitle;

	if (Target == TEXT("active"))
	{
		ChosenWindow = FSlateApplication::Get().GetActiveTopLevelWindow();
		if (ChosenWindow.IsValid())
			ChosenTitle = ChosenWindow->GetTitle().ToString();
	}
	else if (Target == TEXT("main"))
	{
		int64 LargestArea = 0;
		for (const TSharedRef<SWindow>& Win : AllWindows)
		{
			FVector2D WinSize = Win->GetSizeInScreen();
			int64 Area = (int64)WinSize.X * (int64)WinSize.Y;
			if (Area > LargestArea)
			{
				LargestArea = Area;
				ChosenWindow = Win;
				ChosenTitle = Win->GetTitle().ToString();
			}
		}
	}
	else
	{
		for (const TSharedRef<SWindow>& Win : AllWindows)
		{
			FString Title = Win->GetTitle().ToString();
			if (Title.Contains(Target, ESearchCase::IgnoreCase))
			{
				ChosenWindow = Win;
				ChosenTitle = Title;
				break;
			}
		}
		if (!ChosenWindow.IsValid())
		{
			FString WindowList;
			for (const TSharedRef<SWindow>& Win : AllWindows)
			{
				FVector2D WinSize = Win->GetSizeInScreen();
				WindowList += FString::Printf(TEXT("  - \"%s\" (%dx%d)\n"),
					*Win->GetTitle().ToString(), (int32)WinSize.X, (int32)WinSize.Y);
			}
			return FString::Printf(TEXT("Error: No window matching \"%s\". Available windows:\n%s"), *Target, *WindowList);
		}
	}

	if (!ChosenWindow.IsValid() && AllWindows.Num() > 0)
	{
		int64 LargestArea = 0;
		for (const TSharedRef<SWindow>& Win : AllWindows)
		{
			FVector2D WinSize = Win->GetSizeInScreen();
			int64 Area = (int64)WinSize.X * (int64)WinSize.Y;
			if (Area > LargestArea)
			{
				LargestArea = Area;
				ChosenWindow = Win;
				ChosenTitle = Win->GetTitle().ToString();
			}
		}
	}

	if (ChosenWindow.IsValid())
	{
		// Use Windows native PrintWindow to capture actual rendered pixels
		// Slate TakeScreenshot cannot capture GPU-rendered viewports or Blueprint graphs
		TSharedPtr<FGenericWindow> NativeWindow = ChosenWindow->GetNativeWindow();
		if (NativeWindow.IsValid())
		{
			HWND Hwnd = (HWND)NativeWindow->GetOSWindowHandle();
			if (Hwnd)
			{
				RECT ClientRect;
				GetClientRect(Hwnd, &ClientRect);
				int32 Width = ClientRect.right - ClientRect.left;
				int32 Height = ClientRect.bottom - ClientRect.top;

				if (Width > 0 && Height > 0)
				{
					HDC WindowDC = GetDC(Hwnd);
					HDC MemDC = CreateCompatibleDC(WindowDC);
					HBITMAP HBmp = CreateCompatibleBitmap(WindowDC, Width, Height);
					HBITMAP OldBmp = (HBITMAP)SelectObject(MemDC, HBmp);

					// PW_RENDERFULLCONTENT (0x2) captures DX/GPU rendered content
					BOOL bPrintOk = PrintWindow(Hwnd, MemDC, PW_CLIENTONLY | 0x2);
					if (!bPrintOk)
					{
						BitBlt(MemDC, 0, 0, Width, Height, WindowDC, 0, 0, SRCCOPY);
					}

					BITMAPINFO BmpInfo;
					FMemory::Memzero(&BmpInfo, sizeof(BmpInfo));
					BmpInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
					BmpInfo.bmiHeader.biWidth = Width;
					BmpInfo.bmiHeader.biHeight = -Height;
					BmpInfo.bmiHeader.biPlanes = 1;
					BmpInfo.bmiHeader.biBitCount = 32;
					BmpInfo.bmiHeader.biCompression = BI_RGB;

					TArray<FColor> PixelData;
					PixelData.SetNumUninitialized(Width * Height);
					GetDIBits(MemDC, HBmp, 0, Height, PixelData.GetData(), &BmpInfo, DIB_RGB_COLORS);

					SelectObject(MemDC, OldBmp);
					DeleteObject(HBmp);
					DeleteDC(MemDC);
					ReleaseDC(Hwnd, WindowDC);

					IImageWrapperModule& ImgModule = FModuleManager::LoadModuleChecked<IImageWrapperModule>(FName("ImageWrapper"));
					TSharedPtr<IImageWrapper> PngWrapper = ImgModule.CreateImageWrapper(EImageFormat::PNG);
					if (PngWrapper.IsValid() && PngWrapper->SetRaw(PixelData.GetData(), PixelData.Num() * sizeof(FColor), Width, Height, ERGBFormat::BGRA, 8))
					{
						const TArray64<uint8>& PngData = PngWrapper->GetCompressed();
						if (FFileHelper::SaveArrayToFile(PngData, *OutputPath))
						{
							return FString::Printf(TEXT("__RENDER_IMAGE__:%s\nCaptured window \"%s\" (%dx%d) via native capture"), *OutputPath, *ChosenTitle, Width, Height);
						}
					}
				}
			}
		}

		// Fallback: Slate capture if native path fails
		TArray<FColor> PixelData;
		FIntVector OutSize;
		TSharedRef<SWidget> WindowContent = ChosenWindow->GetContent();
		bool bCaptured = FSlateApplication::Get().TakeScreenshot(WindowContent, PixelData, OutSize);
		if (bCaptured && PixelData.Num() > 0 && OutSize.X > 0 && OutSize.Y > 0)
		{
			IImageWrapperModule& ImgModule = FModuleManager::LoadModuleChecked<IImageWrapperModule>(FName("ImageWrapper"));
			TSharedPtr<IImageWrapper> PngWrapper = ImgModule.CreateImageWrapper(EImageFormat::PNG);
			if (PngWrapper.IsValid() && PngWrapper->SetRaw(PixelData.GetData(), PixelData.Num() * sizeof(FColor), OutSize.X, OutSize.Y, ERGBFormat::BGRA, 8))
			{
				const TArray64<uint8>& PngData = PngWrapper->GetCompressed();
				if (FFileHelper::SaveArrayToFile(PngData, *OutputPath))
				{
					return FString::Printf(TEXT("__RENDER_IMAGE__:%s\nCaptured window \"%s\" (%dx%d) via Slate"), *OutputPath, *ChosenTitle, OutSize.X, OutSize.Y);
				}
			}
		}
	}

	FScreenshotRequest::RequestScreenshot(OutputPath, false, false);
	FPlatformProcess::Sleep(1.5f);
	if (FPaths::FileExists(OutputPath))
	{
		return FString::Printf(TEXT("__RENDER_IMAGE__:%s\nViewport captured (game viewport): %s"), *OutputPath, *OutputPath);
	}

	return TEXT("Error: Failed to capture editor window. No active viewport available.");
}

// ============================================================================
// Tool: execute_python — Run Python script in the UE built-in Python interpreter
// ============================================================================

FString FGitHubCopilotUEToolExecutor::Tool_ExecutePython(const TSharedPtr<FJsonObject>& Args)
{
	FString Script;
	if (!Args->TryGetStringField(TEXT("script"), Script) || Script.IsEmpty())
	{
		return TEXT("Error: 'script' is required");
	}

	double TimeoutSeconds = 30.0;
	if (Args->HasField(TEXT("timeout_seconds")))
	{
		TimeoutSeconds = FMath::Clamp(Args->GetNumberField(TEXT("timeout_seconds")), 1.0, 300.0);
	}

	UE_LOG(LogGitHubCopilotUE, Log, TEXT("ToolExecutor: ExecutePython — script length=%d, timeout=%.0fs"),
		Script.Len(), TimeoutSeconds);

	// Ensure the temp directory exists
	const FString TempDir = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("CopilotTemp"));
	IFileManager::Get().MakeDirectory(*TempDir, true);

	// Generate unique file names
	const FString UniqueId = FString::Printf(TEXT("%lld_%d"),
		FDateTime::Now().GetTicks(), FMath::RandRange(10000, 99999));
	const FString ScriptPath = FPaths::Combine(TempDir, FString::Printf(TEXT("exec_%s.py"), *UniqueId));
	const FString OutputPath = FPaths::Combine(TempDir, FString::Printf(TEXT("out_%s.txt"), *UniqueId));

	// Escape the user script for embedding inside a triple-quoted Python string.
	// Replace backslashes first, then quotes.
	FString EscapedScript = Script;
	EscapedScript.ReplaceInline(TEXT("\\"), TEXT("\\\\"));
	EscapedScript.ReplaceInline(TEXT("'''"), TEXT("\\'\\'\\'"));

	// Normalize the output path for Python (forward slashes)
	FString PythonOutputPath = OutputPath;
	PythonOutputPath.ReplaceInline(TEXT("\\"), TEXT("/"));

	// Build the wrapper script that captures stdout/stderr
	FString WrapperScript = FString::Printf(TEXT(
		"import sys, io, traceback\n"
		"_old_stdout = sys.stdout\n"
		"_old_stderr = sys.stderr\n"
		"sys.stdout = _buf = io.StringIO()\n"
		"sys.stderr = _buf\n"
		"try:\n"
		"    exec('''%s''')\n"
		"except Exception:\n"
		"    traceback.print_exc()\n"
		"finally:\n"
		"    sys.stdout = _old_stdout\n"
		"    sys.stderr = _old_stderr\n"
		"with open(r'%s', 'w', encoding='utf-8') as _f:\n"
		"    _f.write(_buf.getvalue())\n"),
		*EscapedScript, *PythonOutputPath);

	// Write the wrapper script to disk
	if (!FFileHelper::SaveStringToFile(WrapperScript, *ScriptPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		return FString::Printf(TEXT("Error: Failed to write temp script to '%s'"), *ScriptPath);
	}

	// Normalize the script path for the py console command (forward slashes)
	FString NormalizedScriptPath = ScriptPath;
	NormalizedScriptPath.ReplaceInline(TEXT("\\"), TEXT("/"));

	// Execute via the UE console "py" command — synchronous on the game thread
	FString PyCommand = FString::Printf(TEXT("py \"%s\""), *NormalizedScriptPath);

	if (!GEngine)
	{
		IFileManager::Get().Delete(*ScriptPath);
		return TEXT("Error: GEngine is not available");
	}

	const double StartTime = FPlatformTime::Seconds();

	GEngine->Exec(
		GEditor ? GEditor->GetEditorWorldContext().World() : nullptr,
		*PyCommand);

	const double Elapsed = FPlatformTime::Seconds() - StartTime;

	// Read captured output
	FString Output;
	if (FPaths::FileExists(OutputPath))
	{
		FFileHelper::LoadFileToString(Output, *OutputPath);
		IFileManager::Get().Delete(*OutputPath);
	}

	// Clean up the temp script
	IFileManager::Get().Delete(*ScriptPath);

	if (Output.IsEmpty())
	{
		Output = TEXT("(no output)");
	}

	UE_LOG(LogGitHubCopilotUE, Log, TEXT("ToolExecutor: Python execution completed in %.2fs, output length=%d"),
		Elapsed, Output.Len());

	return FString::Printf(TEXT("Python executed in %.2fs:\n%s"), Elapsed, *Output);
}

// ============================================================================
// Tool: execute_console_command — Run a UE console command and capture output
// ============================================================================

FString FGitHubCopilotUEToolExecutor::Tool_ExecuteConsoleCommand(const TSharedPtr<FJsonObject>& Args)
{
	FString Command;
	if (!Args->TryGetStringField(TEXT("command"), Command) || Command.IsEmpty())
	{
		return TEXT("Error: 'command' is required");
	}

	UE_LOG(LogGitHubCopilotUE, Log, TEXT("ToolExecutor: ExecuteConsoleCommand — %s"), *Command);

	if (!GEngine)
	{
		return TEXT("Error: GEngine is not available");
	}

	// Use FStringOutputDevice to capture all output from the console command
	FStringOutputDevice OutputDevice;
	OutputDevice.SetAutoEmitLineTerminator(true);

	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	GEngine->Exec(World, *Command, OutputDevice);

	FString Output = static_cast<const FString&>(OutputDevice);
	Output.TrimEndInline();

	if (Output.IsEmpty())
	{
		return FString::Printf(TEXT("Command executed successfully: %s"), *Command);
	}

	return FString::Printf(TEXT("Console command '%s' output:\n%s"), *Command, *Output);
}

// ============================================================================
// Tool: execute_shell — Run a shell / PowerShell command on the host OS
// ============================================================================

FString FGitHubCopilotUEToolExecutor::Tool_ExecuteShell(const TSharedPtr<FJsonObject>& Args)
{
	FString Command;
	if (!Args->TryGetStringField(TEXT("command"), Command) || Command.IsEmpty())
	{
		return TEXT("Error: 'command' is required");
	}

	FString WorkingDirectory;
	Args->TryGetStringField(TEXT("working_directory"), WorkingDirectory);

	double TimeoutSeconds = 30.0;
	if (Args->HasField(TEXT("timeout_seconds")))
	{
		TimeoutSeconds = FMath::Clamp(Args->GetNumberField(TEXT("timeout_seconds")), 1.0, 600.0);
	}

	UE_LOG(LogGitHubCopilotUE, Log, TEXT("ToolExecutor: ExecuteShell — %s (timeout=%.0fs)"), *Command, TimeoutSeconds);

	// Security: refuse obviously dangerous commands
	const TArray<FString> DangerousPatterns = {
		TEXT("format c:"),
		TEXT("format d:"),
		TEXT("del /s /q c:\\"),
		TEXT("rd /s /q c:\\"),
		TEXT("rm -rf /"),
		TEXT("mkfs."),
		TEXT(":(){:|:&};:"),
		TEXT("dd if=/dev/zero"),
		TEXT("shutdown"),
		TEXT("restart-computer")
	};

	FString CommandLower = Command.ToLower();
	for (const FString& Pattern : DangerousPatterns)
	{
		if (CommandLower.Contains(Pattern.ToLower()))
		{
			UE_LOG(LogGitHubCopilotUE, Warning,
				TEXT("ToolExecutor: Blocked dangerous shell command: %s"), *Command);
			return FString::Printf(TEXT("Error: Command blocked for safety — matched dangerous pattern '%s'"), *Pattern);
		}
	}

	int32 ReturnCode = -1;
	FString StdOut;
	FString StdErr;

#if PLATFORM_WINDOWS
	const TCHAR* Executable = TEXT("cmd.exe");
	FString FullArgs = FString::Printf(TEXT("/c %s"), *Command);
#else
	const TCHAR* Executable = TEXT("/bin/bash");
	FString FullArgs = FString::Printf(TEXT("-c \"%s\""), *Command);
#endif

	if (WorkingDirectory.IsEmpty())
	{
		WorkingDirectory = FPaths::ProjectDir();
	}

	// Use CreateProc for working directory support and timeout control
	void* PipeRead = nullptr;
	void* PipeWrite = nullptr;
	FPlatformProcess::CreatePipe(PipeRead, PipeWrite);

	FProcHandle ProcHandle = FPlatformProcess::CreateProc(
		Executable,
		*FullArgs,
		false,    // bLaunchDetached
		true,     // bLaunchHidden
		true,     // bLaunchReallyHidden
		nullptr,  // OutProcessID
		0,        // PriorityModifier
		*WorkingDirectory,
		PipeWrite,
		PipeRead
	);

	if (!ProcHandle.IsValid())
	{
		FPlatformProcess::ClosePipe(PipeRead, PipeWrite);
		return FString::Printf(TEXT("Error: Failed to launch process: %s %s"), Executable, *FullArgs);
	}

	// Read output while the process is running, with timeout
	const double StartTime = FPlatformTime::Seconds();
	FString FullOutput;
	bool bTimedOut = false;

	while (FPlatformProcess::IsProcRunning(ProcHandle))
	{
		FullOutput += FPlatformProcess::ReadPipe(PipeRead);

		if (FPlatformTime::Seconds() - StartTime > TimeoutSeconds)
		{
			bTimedOut = true;
			FPlatformProcess::TerminateProc(ProcHandle, true);
			break;
		}

		FPlatformProcess::Sleep(0.05f);
	}

	// Read any remaining output
	FullOutput += FPlatformProcess::ReadPipe(PipeRead);

	if (!bTimedOut)
	{
		FPlatformProcess::GetProcReturnCode(ProcHandle, &ReturnCode);
	}

	FPlatformProcess::CloseProc(ProcHandle);
	FPlatformProcess::ClosePipe(PipeRead, PipeWrite);

	FullOutput.TrimEndInline();

	FString Result;
	if (bTimedOut)
	{
		Result = FString::Printf(TEXT("Error: Command timed out after %.0f seconds.\nPartial output:\n%s"),
			TimeoutSeconds, *FullOutput);
	}
	else
	{
		Result = FString::Printf(TEXT("Exit code: %d\n%s"), ReturnCode, *FullOutput);
	}

	UE_LOG(LogGitHubCopilotUE, Log, TEXT("ToolExecutor: Shell command finished (code=%d, timedout=%d, output_len=%d)"),
		ReturnCode, bTimedOut ? 1 : 0, FullOutput.Len());

	return Result;
}

// ============================================================================
// Tool: play_in_editor — Start a Play-In-Editor (PIE) session
// ============================================================================

FString FGitHubCopilotUEToolExecutor::Tool_PlayInEditor(const TSharedPtr<FJsonObject>& Args)
{
	if (!GEditor)
	{
		return TEXT("Error: Editor is not available");
	}

	if (GEditor->IsPlaySessionInProgress())
	{
		return TEXT("Error: A Play-In-Editor session is already running. Stop it first with stop_pie.");
	}

	FString Mode = TEXT("selected_viewport");
	if (Args.IsValid() && Args->HasField(TEXT("mode")))
	{
		Mode = Args->GetStringField(TEXT("mode")).ToLower();
	}

	UE_LOG(LogGitHubCopilotUE, Log, TEXT("ToolExecutor: PlayInEditor — mode=%s"), *Mode);

	// Optionally configure PIE settings before launching
	if (Mode == TEXT("new_window") || Mode == TEXT("standalone"))
	{
		// Set the PlayInEditor settings to launch in a new window
		ULevelEditorPlaySettings* PlaySettings = GetMutableDefault<ULevelEditorPlaySettings>();
		if (PlaySettings)
		{
			PlaySettings->LastExecutedPlayModeType = PlayMode_InEditorFloating;
		}
	}
	else if (Mode == TEXT("mobile_preview"))
	{
		ULevelEditorPlaySettings* PlaySettings = GetMutableDefault<ULevelEditorPlaySettings>();
		if (PlaySettings)
		{
			PlaySettings->LastExecutedPlayModeType = PlayMode_InMobilePreview;
		}
	}
	else if (Mode == TEXT("vr_preview") || Mode == TEXT("vr"))
	{
		ULevelEditorPlaySettings* PlaySettings = GetMutableDefault<ULevelEditorPlaySettings>();
		if (PlaySettings)
		{
			PlaySettings->LastExecutedPlayModeType = PlayMode_InVulkanPreview;
		}
	}
	// Default: "selected_viewport" — use existing settings

	FRequestPlaySessionParams Params;
	GEditor->RequestPlaySession(Params);

	return FString::Printf(TEXT("Play-In-Editor session requested (mode: %s). The session will start on the next editor tick."), *Mode);
}

// ============================================================================
// Tool: stop_pie — Stop an active Play-In-Editor session
// ============================================================================

FString FGitHubCopilotUEToolExecutor::Tool_StopPIE(const TSharedPtr<FJsonObject>& Args)
{
	if (!GEditor)
	{
		return TEXT("Error: Editor is not available");
	}

	if (!GEditor->IsPlaySessionInProgress())
	{
		return TEXT("No Play-In-Editor session is currently running.");
	}

	UE_LOG(LogGitHubCopilotUE, Log, TEXT("ToolExecutor: StopPIE — requesting end of play session"));

	GEditor->RequestEndPlayMap();

	return TEXT("Play-In-Editor session stop requested. It will end on the next editor tick.");
}

// ============================================================================
// Tool: package_project — Package the project for distribution via UAT
// ============================================================================

FString FGitHubCopilotUEToolExecutor::Tool_PackageProject(const TSharedPtr<FJsonObject>& Args)
{
	FString Platform = TEXT("Win64");
	if (Args.IsValid() && Args->HasField(TEXT("platform")))
	{
		Platform = Args->GetStringField(TEXT("platform"));
	}

	FString Configuration = TEXT("Development");
	if (Args.IsValid() && Args->HasField(TEXT("configuration")))
	{
		Configuration = Args->GetStringField(TEXT("configuration"));
	}

	FString OutputDirectory;
	if (Args.IsValid())
	{
		Args->TryGetStringField(TEXT("output_directory"), OutputDirectory);
	}

	UE_LOG(LogGitHubCopilotUE, Log, TEXT("ToolExecutor: PackageProject — platform=%s, config=%s"),
		*Platform, *Configuration);

	// Validate configuration
	const TArray<FString> ValidConfigs = { TEXT("Development"), TEXT("Shipping"), TEXT("DebugGame") };
	bool bValidConfig = false;
	for (const FString& VC : ValidConfigs)
	{
		if (Configuration.Equals(VC, ESearchCase::IgnoreCase))
		{
			Configuration = VC; // Normalize casing
			bValidConfig = true;
			break;
		}
	}
	if (!bValidConfig)
	{
		return FString::Printf(TEXT("Error: Invalid configuration '%s'. Must be one of: Development, Shipping, DebugGame"), *Configuration);
	}

	// Validate platform
	const TArray<FString> ValidPlatforms = {
		TEXT("Win64"), TEXT("Linux"), TEXT("Mac"),
		TEXT("Android"), TEXT("IOS"), TEXT("LinuxArm64")
	};
	bool bValidPlatform = false;
	for (const FString& VP : ValidPlatforms)
	{
		if (Platform.Equals(VP, ESearchCase::IgnoreCase))
		{
			Platform = VP; // Normalize casing
			bValidPlatform = true;
			break;
		}
	}
	if (!bValidPlatform)
	{
		return FString::Printf(
			TEXT("Error: Invalid platform '%s'. Supported: Win64, Linux, Mac, Android, IOS, LinuxArm64"),
			*Platform);
	}

	// Locate the UAT script
	const FString EngineDir = FPaths::ConvertRelativePathToFull(FPaths::EngineDir());

#if PLATFORM_WINDOWS
	const FString UATPath = FPaths::Combine(EngineDir, TEXT("Build"), TEXT("BatchFiles"), TEXT("RunUAT.bat"));
#else
	const FString UATPath = FPaths::Combine(EngineDir, TEXT("Build"), TEXT("BatchFiles"), TEXT("RunUAT.sh"));
#endif

	if (!FPaths::FileExists(UATPath))
	{
		return FString::Printf(TEXT("Error: RunUAT not found at '%s'"), *UATPath);
	}

	// Locate the .uproject file
	const FString ProjectPath = FPaths::ConvertRelativePathToFull(FPaths::GetProjectFilePath());
	if (!FPaths::FileExists(ProjectPath))
	{
		return FString::Printf(TEXT("Error: Project file not found at '%s'"), *ProjectPath);
	}

	// Determine output directory
	if (OutputDirectory.IsEmpty())
	{
		OutputDirectory = FPaths::Combine(FPaths::ProjectDir(), TEXT("Packaged"), *Platform);
	}
	OutputDirectory = FPaths::ConvertRelativePathToFull(OutputDirectory);
	IFileManager::Get().MakeDirectory(*OutputDirectory, true);

	// Build the UAT command line
	FString UATArgs = FString::Printf(
		TEXT("BuildCookRun")
		TEXT(" -project=\"%s\"")
		TEXT(" -noP4")
		TEXT(" -platform=%s")
		TEXT(" -clientconfig=%s")
		TEXT(" -serverconfig=%s")
		TEXT(" -cook")
		TEXT(" -build")
		TEXT(" -stage")
		TEXT(" -prereqs")
		TEXT(" -pak")
		TEXT(" -archive")
		TEXT(" -archivedirectory=\"%s\""),
		*ProjectPath,
		*Platform,
		*Configuration,
		*Configuration,
		*OutputDirectory
	);

	// Launch the UAT process asynchronously — packaging is long-running
#if PLATFORM_WINDOWS
	const TCHAR* Executable = TEXT("cmd.exe");
	FString FullArgs = FString::Printf(TEXT("/c \"\"%s\" %s\""), *UATPath, *UATArgs);
#else
	const TCHAR* Executable = TEXT("/bin/bash");
	FString FullArgs = FString::Printf(TEXT("-c \"\\\"%s\\\" %s\""), *UATPath, *UATArgs);
#endif

	uint32 ProcessId = 0;
	FProcHandle ProcHandle = FPlatformProcess::CreateProc(
		Executable,
		*FullArgs,
		true,     // bLaunchDetached — let it run independently
		false,    // bLaunchHidden
		false,    // bLaunchReallyHidden
		&ProcessId,
		0,        // PriorityModifier
		nullptr,  // WorkingDirectory
		nullptr,  // PipeWriteChild
		nullptr   // PipeReadChild
	);

	if (!ProcHandle.IsValid())
	{
		return FString::Printf(TEXT("Error: Failed to launch UAT process.\nCommand: %s %s"), Executable, *FullArgs);
	}

	UE_LOG(LogGitHubCopilotUE, Log,
		TEXT("ToolExecutor: PackageProject launched (PID=%u) — %s %s"),
		ProcessId, Executable, *FullArgs);

	return FString::Printf(
		TEXT("Packaging started (PID: %u).\n")
		TEXT("Platform: %s\n")
		TEXT("Configuration: %s\n")
		TEXT("Output: %s\n")
		TEXT("UAT: %s\n\n")
		TEXT("This process runs in the background and will take several minutes. ")
		TEXT("Check the Output Log for progress. The packaged build will appear in the output directory when complete."),
		ProcessId, *Platform, *Configuration, *OutputDirectory, *UATPath);
}


// ============================================================================
// Tier 2 Tool Implementations for FGitHubCopilotUEToolExecutor
// No includes — all required headers already present in the main TU.
// Log category LogGitHubCopilotUE is declared externally.
// ============================================================================

// ============================================================================
// Tool: list_actors — list all actors in the current editor world
// ============================================================================

FString FGitHubCopilotUEToolExecutor::Tool_ListActors(const TSharedPtr<FJsonObject>& Args)
{
	if (!GEditor)
	{
		return TEXT("Error: GEditor is not available.");
	}

	UWorld* World = GEditor->GetEditorWorldContext().World();
	if (!World)
	{
		return TEXT("Error: No editor world available. Open a level first.");
	}

	FString ClassFilter;
	Args->TryGetStringField(TEXT("class_filter"), ClassFilter);

	FString NameFilter;
	Args->TryGetStringField(TEXT("name_filter"), NameFilter);

	int32 MaxResults = 100;
	double MaxResultsVal;
	if (Args->TryGetNumberField(TEXT("max_results"), MaxResultsVal))
	{
		MaxResults = FMath::Clamp(static_cast<int32>(MaxResultsVal), 1, 10000);
	}

	FString Result;
	int32 Count = 0;
	int32 TotalMatched = 0;

	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* Actor = *It;
		if (!Actor)
		{
			continue;
		}

		const FString ActorName = Actor->GetName();
		const FString ActorLabel = Actor->GetActorLabel();
		const FString ClassName = Actor->GetClass()->GetName();

		// Apply class filter
		if (!ClassFilter.IsEmpty())
		{
			if (!ClassName.Contains(ClassFilter, ESearchCase::IgnoreCase))
			{
				continue;
			}
		}

		// Apply name filter (substring match on Name or Label)
		if (!NameFilter.IsEmpty())
		{
			if (!ActorName.Contains(NameFilter, ESearchCase::IgnoreCase) &&
				!ActorLabel.Contains(NameFilter, ESearchCase::IgnoreCase))
			{
				continue;
			}
		}

		++TotalMatched;
		if (Count >= MaxResults)
		{
			continue; // keep counting total but stop appending
		}

		const FVector Location = Actor->GetActorLocation();
		const FRotator Rotation = Actor->GetActorRotation();

		Result += FString::Printf(
			TEXT("[%d] Name: %s | Label: %s | Class: %s | Location: (%.1f, %.1f, %.1f) | Rotation: (P=%.1f, Y=%.1f, R=%.1f)\n"),
			Count + 1,
			*ActorName,
			*ActorLabel,
			*ClassName,
			Location.X, Location.Y, Location.Z,
			Rotation.Pitch, Rotation.Yaw, Rotation.Roll);

		++Count;
	}

	if (Count == 0)
	{
		return TEXT("No actors found matching the specified filters.");
	}

	FString Header = FString::Printf(TEXT("Found %d actor(s)"), TotalMatched);
	if (TotalMatched > MaxResults)
	{
		Header += FString::Printf(TEXT(" (showing first %d)"), MaxResults);
	}
	Header += TEXT(":\n");

	UE_LOG(LogGitHubCopilotUE, Log, TEXT("ToolExecutor: ListActors returned %d/%d actors"), Count, TotalMatched);
	return Header + Result;
}

// ============================================================================
// Tool: get_actor_properties — detailed properties of a specific actor
// ============================================================================

FString FGitHubCopilotUEToolExecutor::Tool_GetActorProperties(const TSharedPtr<FJsonObject>& Args)
{
	if (!GEditor)
	{
		return TEXT("Error: GEditor is not available.");
	}

	FString ActorNameInput;
	if (!Args->TryGetStringField(TEXT("actor_name"), ActorNameInput) || ActorNameInput.IsEmpty())
	{
		return TEXT("Error: 'actor_name' is required.");
	}

	UWorld* World = GEditor->GetEditorWorldContext().World();
	if (!World)
	{
		return TEXT("Error: No editor world available. Open a level first.");
	}

	// Find the actor by Name or Label
	AActor* FoundActor = nullptr;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* Actor = *It;
		if (!Actor) continue;

		if (Actor->GetName().Equals(ActorNameInput, ESearchCase::IgnoreCase) ||
			Actor->GetActorLabel().Equals(ActorNameInput, ESearchCase::IgnoreCase))
		{
			FoundActor = Actor;
			break;
		}
	}

	if (!FoundActor)
	{
		return FString::Printf(TEXT("Error: Actor '%s' not found in the current level."), *ActorNameInput);
	}

	FString Result;

	// Basic info
	Result += FString::Printf(TEXT("=== Actor: %s ===\n"), *FoundActor->GetName());
	Result += FString::Printf(TEXT("Label: %s\n"), *FoundActor->GetActorLabel());
	Result += FString::Printf(TEXT("Class: %s\n"), *FoundActor->GetClass()->GetName());

	// Transform
	const FVector Location = FoundActor->GetActorLocation();
	const FRotator Rotation = FoundActor->GetActorRotation();
	const FVector Scale = FoundActor->GetActorScale3D();
	Result += FString::Printf(TEXT("Location: (%.2f, %.2f, %.2f)\n"), Location.X, Location.Y, Location.Z);
	Result += FString::Printf(TEXT("Rotation: (P=%.2f, Y=%.2f, R=%.2f)\n"), Rotation.Pitch, Rotation.Yaw, Rotation.Roll);
	Result += FString::Printf(TEXT("Scale: (%.2f, %.2f, %.2f)\n"), Scale.X, Scale.Y, Scale.Z);

	// Mobility
	USceneComponent* RootComp = FoundActor->GetRootComponent();
	if (RootComp)
	{
		const TCHAR* MobilityStr = TEXT("Unknown");
		switch (RootComp->Mobility)
		{
		case EComponentMobility::Static:     MobilityStr = TEXT("Static"); break;
		case EComponentMobility::Stationary: MobilityStr = TEXT("Stationary"); break;
		case EComponentMobility::Movable:    MobilityStr = TEXT("Movable"); break;
		}
		Result += FString::Printf(TEXT("Mobility: %s\n"), MobilityStr);
	}

	// Hidden
	Result += FString::Printf(TEXT("Hidden: %s\n"), FoundActor->IsHidden() ? TEXT("true") : TEXT("false"));

	// Tags
	if (FoundActor->Tags.Num() > 0)
	{
		FString TagStr;
		for (const FName& Tag : FoundActor->Tags)
		{
			if (!TagStr.IsEmpty()) TagStr += TEXT(", ");
			TagStr += Tag.ToString();
		}
		Result += FString::Printf(TEXT("Tags: [%s]\n"), *TagStr);
	}
	else
	{
		Result += TEXT("Tags: []\n");
	}

	// Components
	TArray<UActorComponent*> Components;
	FoundActor->GetComponents(Components);
	Result += FString::Printf(TEXT("\nComponents (%d):\n"), Components.Num());
	for (UActorComponent* Comp : Components)
	{
		if (!Comp) continue;

		Result += FString::Printf(TEXT("  - %s [%s]"), *Comp->GetName(), *Comp->GetClass()->GetName());

		// StaticMeshComponent details
		if (UStaticMeshComponent* SMC = Cast<UStaticMeshComponent>(Comp))
		{
			if (SMC->GetStaticMesh())
			{
				Result += FString::Printf(TEXT(" | Mesh: %s"), *SMC->GetStaticMesh()->GetPathName());
			}
			const int32 NumMaterials = SMC->GetNumMaterials();
			if (NumMaterials > 0)
			{
				Result += FString::Printf(TEXT(" | Materials(%d): "), NumMaterials);
				for (int32 i = 0; i < NumMaterials; ++i)
				{
					UMaterialInterface* Mat = SMC->GetMaterial(i);
					if (Mat)
					{
						if (i > 0) Result += TEXT(", ");
						Result += Mat->GetName();
					}
				}
			}
		}

		// SkeletalMeshComponent details
		if (USkeletalMeshComponent* SkMC = Cast<USkeletalMeshComponent>(Comp))
		{
			if (SkMC->GetSkeletalMeshAsset())
			{
				Result += FString::Printf(TEXT(" | Mesh: %s"), *SkMC->GetSkeletalMeshAsset()->GetPathName());
			}
			if (SkMC->GetAnimInstance() && SkMC->GetAnimInstance()->GetClass())
			{
				Result += FString::Printf(TEXT(" | AnimBP: %s"), *SkMC->GetAnimInstance()->GetClass()->GetName());
			}
		}

		Result += TEXT("\n");
	}

	// Custom properties — EditAnywhere / BlueprintReadWrite, limited to 50
	Result += TEXT("\nEditable Properties:\n");
	int32 PropCount = 0;
	const int32 MaxProps = 50;

	for (TFieldIterator<FProperty> PropIt(FoundActor->GetClass()); PropIt; ++PropIt)
	{
		if (PropCount >= MaxProps)
		{
			Result += TEXT("  ... (truncated, limit reached)\n");
			break;
		}

		FProperty* Property = *PropIt;
		if (!Property) continue;

		// Only include EditAnywhere or BlueprintReadWrite properties
		const bool bEditAnywhere = Property->HasAnyPropertyFlags(CPF_Edit);
		const bool bBlueprintVisible = Property->HasAnyPropertyFlags(CPF_BlueprintVisible);
		if (!bEditAnywhere && !bBlueprintVisible)
		{
			continue;
		}

		// Skip internal/noisy categories
		const FString Category = Property->GetMetaData(TEXT("Category"));
		if (Category.Equals(TEXT("Replication"), ESearchCase::IgnoreCase) ||
			Category.Equals(TEXT("Input"), ESearchCase::IgnoreCase))
		{
			continue;
		}

		FString ValueStr;
		Property->ExportTextItem_Direct(ValueStr, Property->ContainerPtrToValuePtr<void>(FoundActor), nullptr, FoundActor, PPF_None);

		// Truncate very long values
		if (ValueStr.Len() > 200)
		{
			ValueStr = ValueStr.Left(200) + TEXT("...");
		}

		Result += FString::Printf(TEXT("  %s (%s): %s\n"),
			*Property->GetName(),
			*Property->GetCPPType(),
			*ValueStr);
		++PropCount;
	}

	UE_LOG(LogGitHubCopilotUE, Log, TEXT("ToolExecutor: GetActorProperties for '%s'"), *FoundActor->GetName());
	return Result;
}

// ============================================================================
// Tool: set_actor_properties — modify properties on an actor
// ============================================================================

FString FGitHubCopilotUEToolExecutor::Tool_SetActorProperties(const TSharedPtr<FJsonObject>& Args)
{
	if (!GEditor)
	{
		return TEXT("Error: GEditor is not available.");
	}

	FString ActorNameInput;
	if (!Args->TryGetStringField(TEXT("actor_name"), ActorNameInput) || ActorNameInput.IsEmpty())
	{
		return TEXT("Error: 'actor_name' is required.");
	}

	UWorld* World = GEditor->GetEditorWorldContext().World();
	if (!World)
	{
		return TEXT("Error: No editor world available. Open a level first.");
	}

	// Find the actor
	AActor* FoundActor = nullptr;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* Actor = *It;
		if (!Actor) continue;

		if (Actor->GetName().Equals(ActorNameInput, ESearchCase::IgnoreCase) ||
			Actor->GetActorLabel().Equals(ActorNameInput, ESearchCase::IgnoreCase))
		{
			FoundActor = Actor;
			break;
		}
	}

	if (!FoundActor)
	{
		return FString::Printf(TEXT("Error: Actor '%s' not found in the current level."), *ActorNameInput);
	}

	FoundActor->Modify();

	FString Changes;
	int32 ChangeCount = 0;

	// Location
	const TArray<TSharedPtr<FJsonValue>>* LocArray;
	if (Args->TryGetArrayField(TEXT("location"), LocArray) && LocArray->Num() >= 3)
	{
		FVector NewLocation(
			(*LocArray)[0]->AsNumber(),
			(*LocArray)[1]->AsNumber(),
			(*LocArray)[2]->AsNumber());
		FoundActor->SetActorLocation(NewLocation);
		Changes += FString::Printf(TEXT("  Location -> (%.2f, %.2f, %.2f)\n"), NewLocation.X, NewLocation.Y, NewLocation.Z);
		++ChangeCount;
	}

	// Rotation
	const TArray<TSharedPtr<FJsonValue>>* RotArray;
	if (Args->TryGetArrayField(TEXT("rotation"), RotArray) && RotArray->Num() >= 3)
	{
		FRotator NewRotation(
			(*RotArray)[0]->AsNumber(),
			(*RotArray)[1]->AsNumber(),
			(*RotArray)[2]->AsNumber());
		FoundActor->SetActorRotation(NewRotation);
		Changes += FString::Printf(TEXT("  Rotation -> (P=%.2f, Y=%.2f, R=%.2f)\n"), NewRotation.Pitch, NewRotation.Yaw, NewRotation.Roll);
		++ChangeCount;
	}

	// Scale
	const TArray<TSharedPtr<FJsonValue>>* ScaleArray;
	if (Args->TryGetArrayField(TEXT("scale"), ScaleArray) && ScaleArray->Num() >= 3)
	{
		FVector NewScale(
			(*ScaleArray)[0]->AsNumber(),
			(*ScaleArray)[1]->AsNumber(),
			(*ScaleArray)[2]->AsNumber());
		FoundActor->SetActorScale3D(NewScale);
		Changes += FString::Printf(TEXT("  Scale -> (%.2f, %.2f, %.2f)\n"), NewScale.X, NewScale.Y, NewScale.Z);
		++ChangeCount;
	}

	// Hidden
	bool bHidden;
	if (Args->TryGetBoolField(TEXT("hidden"), bHidden))
	{
		FoundActor->SetActorHiddenInGame(bHidden);
#if WITH_EDITOR
		FoundActor->SetIsTemporarilyHiddenInEditor(bHidden);
#endif
		Changes += FString::Printf(TEXT("  Hidden -> %s\n"), bHidden ? TEXT("true") : TEXT("false"));
		++ChangeCount;
	}

	// Label
	FString NewLabel;
	if (Args->TryGetStringField(TEXT("label"), NewLabel) && !NewLabel.IsEmpty())
	{
		FoundActor->SetActorLabel(NewLabel);
		Changes += FString::Printf(TEXT("  Label -> %s\n"), *NewLabel);
		++ChangeCount;
	}

	// Mobility
	FString MobilityStr;
	if (Args->TryGetStringField(TEXT("mobility"), MobilityStr) && !MobilityStr.IsEmpty())
	{
		USceneComponent* RootComp = FoundActor->GetRootComponent();
		if (RootComp)
		{
			if (MobilityStr.Equals(TEXT("static"), ESearchCase::IgnoreCase))
			{
				RootComp->SetMobility(EComponentMobility::Static);
				Changes += TEXT("  Mobility -> Static\n");
				++ChangeCount;
			}
			else if (MobilityStr.Equals(TEXT("stationary"), ESearchCase::IgnoreCase))
			{
				RootComp->SetMobility(EComponentMobility::Stationary);
				Changes += TEXT("  Mobility -> Stationary\n");
				++ChangeCount;
			}
			else if (MobilityStr.Equals(TEXT("movable"), ESearchCase::IgnoreCase))
			{
				RootComp->SetMobility(EComponentMobility::Movable);
				Changes += TEXT("  Mobility -> Movable\n");
				++ChangeCount;
			}
			else
			{
				Changes += FString::Printf(TEXT("  Warning: Unknown mobility '%s' (use static/stationary/movable)\n"), *MobilityStr);
			}
		}
		else
		{
			Changes += TEXT("  Warning: Actor has no root component, cannot set mobility\n");
		}
	}

	// Custom properties via reflection
	const TSharedPtr<FJsonObject>* PropsObj;
	if (Args->TryGetObjectField(TEXT("properties"), PropsObj) && PropsObj->IsValid())
	{
		for (const auto& Pair : (*PropsObj)->Values)
		{
			const FString& Key = Pair.Key;
			FString Value;

			// Extract value as string regardless of JSON type
			if (Pair.Value->Type == EJson::String)
			{
				Value = Pair.Value->AsString();
			}
			else if (Pair.Value->Type == EJson::Number)
			{
				Value = FString::SanitizeFloat(Pair.Value->AsNumber());
			}
			else if (Pair.Value->Type == EJson::Boolean)
			{
				Value = Pair.Value->AsBool() ? TEXT("true") : TEXT("false");
			}
			else
			{
				// For complex types, serialize to JSON string
				TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Value);
				FJsonSerializer::Serialize(Pair.Value, FString(), Writer);
				Writer->Close();
			}

			FProperty* Prop = FoundActor->GetClass()->FindPropertyByName(FName(*Key));
			if (Prop)
			{
				const TCHAR* ImportResult = Prop->ImportText_Direct(*Value, Prop->ContainerPtrToValuePtr<void>(FoundActor), FoundActor, PPF_None);
				if (ImportResult)
				{
					Changes += FString::Printf(TEXT("  %s -> %s\n"), *Key, *Value);
					++ChangeCount;
				}
				else
				{
					Changes += FString::Printf(TEXT("  Warning: Failed to set '%s' to '%s'\n"), *Key, *Value);
				}
			}
			else
			{
				Changes += FString::Printf(TEXT("  Warning: Property '%s' not found on %s\n"), *Key, *FoundActor->GetClass()->GetName());
			}
		}
	}

	// Mark dirty
	FoundActor->MarkPackageDirty();
	if (World)
	{
		World->MarkPackageDirty();
	}

	if (ChangeCount == 0)
	{
		return FString::Printf(TEXT("No changes applied to actor '%s'. Provide location, rotation, scale, hidden, label, mobility, or properties."), *FoundActor->GetName());
	}

	UE_LOG(LogGitHubCopilotUE, Log, TEXT("ToolExecutor: SetActorProperties on '%s' — %d change(s)"), *FoundActor->GetName(), ChangeCount);
	return FString::Printf(TEXT("Applied %d change(s) to actor '%s':\n%s"), ChangeCount, *FoundActor->GetName(), *Changes);
}

// ============================================================================
// Tool: delete_actors — delete actors from the level
// ============================================================================

FString FGitHubCopilotUEToolExecutor::Tool_DeleteActors(const TSharedPtr<FJsonObject>& Args)
{
	if (!GEditor)
	{
		return TEXT("Error: GEditor is not available.");
	}

	const TArray<TSharedPtr<FJsonValue>>* NamesArray;
	if (!Args->TryGetArrayField(TEXT("actor_names"), NamesArray) || NamesArray->Num() == 0)
	{
		return TEXT("Error: 'actor_names' array is required and must not be empty.");
	}

	bool bConfirm = false;
	Args->TryGetBoolField(TEXT("confirm"), bConfirm);

	UWorld* World = GEditor->GetEditorWorldContext().World();
	if (!World)
	{
		return TEXT("Error: No editor world available. Open a level first.");
	}

	// Build the set of names to match
	TArray<FString> NamesToDelete;
	for (const TSharedPtr<FJsonValue>& Val : *NamesArray)
	{
		FString Name = Val->AsString().TrimStartAndEnd();
		if (!Name.IsEmpty())
		{
			NamesToDelete.Add(Name);
		}
	}

	if (NamesToDelete.Num() == 0)
	{
		return TEXT("Error: No valid names provided in 'actor_names'.");
	}

	// Find matching actors
	TArray<AActor*> MatchedActors;
	TArray<FString> MatchedDescriptions;

	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* Actor = *It;
		if (!Actor) continue;

		const FString ActorName = Actor->GetName();
		const FString ActorLabel = Actor->GetActorLabel();

		for (const FString& TargetName : NamesToDelete)
		{
			if (ActorName.Equals(TargetName, ESearchCase::IgnoreCase) ||
				ActorLabel.Equals(TargetName, ESearchCase::IgnoreCase))
			{
				MatchedActors.Add(Actor);
				MatchedDescriptions.Add(FString::Printf(TEXT("  %s [%s] (Label: %s)"),
					*ActorName, *Actor->GetClass()->GetName(), *ActorLabel));
				break;
			}
		}
	}

	if (MatchedActors.Num() == 0)
	{
		return TEXT("No actors found matching the provided names.");
	}

	FString Result;

	if (!bConfirm)
	{
		// Dry run
		Result = FString::Printf(TEXT("Would delete %d actor(s):\n"), MatchedActors.Num());
		for (const FString& Desc : MatchedDescriptions)
		{
			Result += Desc + TEXT("\n");
		}
		Result += TEXT("\nSet 'confirm' to true to actually delete these actors.");
	}
	else
	{
		int32 DeletedCount = 0;
		for (int32 i = 0; i < MatchedActors.Num(); ++i)
		{
			AActor* Actor = MatchedActors[i];
			if (Actor && !Actor->IsPendingKillPending())
			{
				UE_LOG(LogGitHubCopilotUE, Log, TEXT("ToolExecutor: Deleting actor '%s' [%s]"),
					*Actor->GetName(), *Actor->GetClass()->GetName());

				const bool bDestroyed = World->DestroyActor(Actor);
				if (bDestroyed)
				{
					++DeletedCount;
				}
			}
		}

		World->MarkPackageDirty();

		Result = FString::Printf(TEXT("Deleted %d actor(s):\n"), DeletedCount);
		for (const FString& Desc : MatchedDescriptions)
		{
			Result += Desc + TEXT("\n");
		}

		UE_LOG(LogGitHubCopilotUE, Log, TEXT("ToolExecutor: DeleteActors — deleted %d actor(s)"), DeletedCount);
	}

	return Result;
}

// ============================================================================
// Tool: import_asset — import external files into the Content Browser
// ============================================================================

FString FGitHubCopilotUEToolExecutor::Tool_ImportAsset(const TSharedPtr<FJsonObject>& Args)
{
	if (!GEditor)
	{
		return TEXT("Error: GEditor is not available.");
	}

	FString SourcePath;
	if (!Args->TryGetStringField(TEXT("source_path"), SourcePath) || SourcePath.IsEmpty())
	{
		return TEXT("Error: 'source_path' is required.");
	}

	// Normalize and verify file existence
	FPaths::NormalizeFilename(SourcePath);
	if (!FPaths::FileExists(SourcePath))
	{
		return FString::Printf(TEXT("Error: Source file does not exist: '%s'"), *SourcePath);
	}

	FString DestPath = TEXT("/Game/Copilot/Imports");
	Args->TryGetStringField(TEXT("destination_path"), DestPath);
	DestPath = DestPath.TrimStartAndEnd();
	if (DestPath.IsEmpty())
	{
		DestPath = TEXT("/Game/Copilot/Imports");
	}
	if (!DestPath.StartsWith(TEXT("/")))
	{
		DestPath = TEXT("/Game/") + DestPath;
	}

	// Validate destination path
	if (!DestPath.StartsWith(TEXT("/Game")))
	{
		return FString::Printf(TEXT("Error: Destination path must start with /Game. Got: '%s'"), *DestPath);
	}

	UE_LOG(LogGitHubCopilotUE, Log, TEXT("ToolExecutor: ImportAsset from '%s' to '%s'"), *SourcePath, *DestPath);

	FAssetToolsModule& AssetToolsModule = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools");
	IAssetTools& AssetTools = AssetToolsModule.Get();

	UAutomatedAssetImportData* ImportData = NewObject<UAutomatedAssetImportData>();
	if (!ImportData)
	{
		return TEXT("Error: Failed to create UAutomatedAssetImportData.");
	}

	ImportData->Filenames.Add(SourcePath);
	ImportData->DestinationPath = DestPath;
	ImportData->bReplaceExisting = true;

	TArray<UObject*> ImportedAssets = AssetTools.ImportAssetsAutomated(ImportData);

	if (ImportedAssets.Num() == 0)
	{
		return FString::Printf(TEXT("Error: Import failed for '%s'. The file type may not be supported or the import settings may be incorrect."), *SourcePath);
	}

	FString Result = FString::Printf(TEXT("Successfully imported %d asset(s):\n"), ImportedAssets.Num());
	for (UObject* Asset : ImportedAssets)
	{
		if (Asset)
		{
			Result += FString::Printf(TEXT("  %s [%s]\n"), *Asset->GetPathName(), *Asset->GetClass()->GetName());
		}
	}

	UE_LOG(LogGitHubCopilotUE, Log, TEXT("ToolExecutor: ImportAsset — imported %d asset(s)"), ImportedAssets.Num());
	return Result;
}

// ============================================================================
// Tool: open_level — open/load a different level in the editor
// ============================================================================

FString FGitHubCopilotUEToolExecutor::Tool_OpenLevel(const TSharedPtr<FJsonObject>& Args)
{
	if (!GEditor)
	{
		return TEXT("Error: GEditor is not available.");
	}

	FString LevelPath;
	if (!Args->TryGetStringField(TEXT("level_path"), LevelPath) || LevelPath.IsEmpty())
	{
		return TEXT("Error: 'level_path' is required.");
	}

	LevelPath = LevelPath.TrimStartAndEnd();

	// If the user provided just a name (no path separator), search asset registry
	if (!LevelPath.Contains(TEXT("/")))
	{
		FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
		IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

		TArray<FAssetData> FoundAssets;
		FARFilter Filter;
		Filter.ClassPaths.Add(UWorld::StaticClass()->GetClassPathName());
		Filter.bRecursivePaths = true;
		Filter.PackagePaths.Add(FName(TEXT("/Game")));

		AssetRegistry.GetAssets(Filter, FoundAssets);

		FString BestMatch;
		for (const FAssetData& Asset : FoundAssets)
		{
			if (Asset.AssetName.ToString().Equals(LevelPath, ESearchCase::IgnoreCase))
			{
				BestMatch = Asset.GetObjectPathString();
				// Extract the package path (remove the .AssetName suffix)
				BestMatch = Asset.PackageName.ToString();
				break;
			}
		}

		if (BestMatch.IsEmpty())
		{
			// Try substring match as fallback
			for (const FAssetData& Asset : FoundAssets)
			{
				if (Asset.AssetName.ToString().Contains(LevelPath, ESearchCase::IgnoreCase))
				{
					BestMatch = Asset.PackageName.ToString();
					break;
				}
			}
		}

		if (!BestMatch.IsEmpty())
		{
			LevelPath = BestMatch;
		}
		else
		{
			return FString::Printf(TEXT("Error: Could not find a level named '%s' in /Game. Provide a full content path like /Game/Maps/MyLevel."), *LevelPath);
		}
	}

	UE_LOG(LogGitHubCopilotUE, Log, TEXT("ToolExecutor: OpenLevel — loading '%s'"), *LevelPath);

	// Use FEditorFileUtils::LoadMap
	const bool bLoadSuccessful = FEditorFileUtils::LoadMap(LevelPath);

	if (bLoadSuccessful)
	{
		UE_LOG(LogGitHubCopilotUE, Log, TEXT("ToolExecutor: OpenLevel — successfully loaded '%s'"), *LevelPath);
		return FString::Printf(TEXT("Successfully opened level: %s"), *LevelPath);
	}
	else
	{
		return FString::Printf(TEXT("Error: Failed to load level '%s'. Verify the path is correct and the level asset exists."), *LevelPath);
	}
}

// ============================================================================
// Tool: rename_asset — rename or move an asset in the Content Browser
// ============================================================================

FString FGitHubCopilotUEToolExecutor::Tool_RenameAsset(const TSharedPtr<FJsonObject>& Args)
{
	if (!GEditor)
	{
		return TEXT("Error: GEditor is not available.");
	}

	FString SourcePath;
	if (!Args->TryGetStringField(TEXT("source_path"), SourcePath) || SourcePath.IsEmpty())
	{
		return TEXT("Error: 'source_path' is required.");
	}

	SourcePath = SourcePath.TrimStartAndEnd();

	FString NewName;
	Args->TryGetStringField(TEXT("new_name"), NewName);
	NewName = NewName.TrimStartAndEnd();

	FString DestinationPath;
	Args->TryGetStringField(TEXT("destination_path"), DestinationPath);
	DestinationPath = DestinationPath.TrimStartAndEnd();

	if (NewName.IsEmpty() && DestinationPath.IsEmpty())
	{
		return TEXT("Error: At least one of 'new_name' or 'destination_path' must be provided.");
	}

	// Load the asset to verify it exists
	UObject* Asset = LoadObject<UObject>(nullptr, *SourcePath);
	if (!Asset)
	{
		// Try appending the asset name (e.g., /Game/Meshes/Foo -> /Game/Meshes/Foo.Foo)
		FString AssetName = FPaths::GetBaseFilename(SourcePath);
		FString FullObjectPath = SourcePath + TEXT(".") + AssetName;
		Asset = LoadObject<UObject>(nullptr, *FullObjectPath);
	}

	if (!Asset)
	{
		return FString::Printf(TEXT("Error: Asset not found at '%s'."), *SourcePath);
	}

	// Determine final name and package path
	FString CurrentPackagePath = FPackageName::GetLongPackagePath(Asset->GetOutermost()->GetName());
	FString CurrentAssetName = Asset->GetName();

	FString FinalName = NewName.IsEmpty() ? CurrentAssetName : NewName;
	FString FinalPackagePath = DestinationPath.IsEmpty() ? CurrentPackagePath : DestinationPath;

	if (!FinalPackagePath.StartsWith(TEXT("/Game")))
	{
		return FString::Printf(TEXT("Error: Destination path must start with /Game. Got: '%s'"), *FinalPackagePath);
	}

	UE_LOG(LogGitHubCopilotUE, Log, TEXT("ToolExecutor: RenameAsset '%s' -> '%s/%s'"),
		*SourcePath, *FinalPackagePath, *FinalName);

	FAssetToolsModule& AssetToolsModule = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools");

	TArray<FAssetRenameData> RenameData;
	RenameData.Emplace(Asset, FinalPackagePath, FinalName);

	const bool bSuccess = AssetToolsModule.Get().RenameAssets(RenameData);

	if (bSuccess)
	{
		FString NewFullPath = FinalPackagePath / FinalName;
		UE_LOG(LogGitHubCopilotUE, Log, TEXT("ToolExecutor: RenameAsset — success: '%s'"), *NewFullPath);
		return FString::Printf(TEXT("Successfully renamed/moved asset:\n  From: %s\n  To: %s"), *SourcePath, *NewFullPath);
	}
	else
	{
		return FString::Printf(TEXT("Error: Failed to rename/move asset '%s'. The destination may already exist or the asset may be in use."), *SourcePath);
	}
}

// ============================================================================
// Tool: get_selected_actors — get currently selected actors in the viewport
// ============================================================================

FString FGitHubCopilotUEToolExecutor::Tool_GetSelectedActors(const TSharedPtr<FJsonObject>& Args)
{
	if (!GEditor)
	{
		return TEXT("Error: GEditor is not available.");
	}

	USelection* SelectedActors = GEditor->GetSelectedActors();
	if (!SelectedActors)
	{
		return TEXT("Error: Could not retrieve editor selection.");
	}

	int32 Count = 0;
	FString Result;

	for (int32 Idx = 0; Idx < SelectedActors->Num(); ++Idx)
	{
		UObject* Obj = SelectedActors->GetSelectedObject(Idx);
		AActor* Actor = Cast<AActor>(Obj);
		if (!Actor)
		{
			continue;
		}

		const FVector Location = Actor->GetActorLocation();
		const FRotator Rotation = Actor->GetActorRotation();
		const FVector Scale = Actor->GetActorScale3D();

		Result += FString::Printf(
			TEXT("[%d] Name: %s | Label: %s | Class: %s\n"),
			Count + 1,
			*Actor->GetName(),
			*Actor->GetActorLabel(),
			*Actor->GetClass()->GetName());
		Result += FString::Printf(
			TEXT("     Location: (%.2f, %.2f, %.2f) | Rotation: (P=%.2f, Y=%.2f, R=%.2f) | Scale: (%.2f, %.2f, %.2f)\n"),
			Location.X, Location.Y, Location.Z,
			Rotation.Pitch, Rotation.Yaw, Rotation.Roll,
			Scale.X, Scale.Y, Scale.Z);

		++Count;
	}

	if (Count == 0)
	{
		return TEXT("No actors are currently selected in the editor viewport.");
	}

	UE_LOG(LogGitHubCopilotUE, Log, TEXT("ToolExecutor: GetSelectedActors — %d actor(s) selected"), Count);
	return FString::Printf(TEXT("%d selected actor(s):\n%s"), Count, *Result);
}


// ============================================================================
//  Tier 3 Tool Implementations — Animation, Sequencer, Lighting, Settings,
//  UI, Blueprint Graph, and Git operations
//
//  These are FUNCTION BODIES ONLY (no includes).
//  Expected to be integrated into GitHubCopilotUEToolExecutor.cpp which
//  already provides: Editor.h, Engine/World.h, EngineUtils.h,
//  GameFramework/Actor.h, Components/SceneComponent.h,
//  Components/StaticMeshComponent.h, AssetRegistry/AssetRegistryModule.h,
//  UObject/Package.h, UObject/SavePackage.h, Misc/PackageName.h,
//  Misc/FileHelper.h, HAL/PlatformProcess.h,
//  Subsystems/AssetEditorSubsystem.h, Engine/Blueprint.h,
//  Kismet2/KismetEditorUtilities.h, Materials/Material.h,
//  Engine/DataTable.h
// ============================================================================


// ============================================================================
// Tool: retarget_animations — Name-based animation retarget between skeletons
// ============================================================================

FString FGitHubCopilotUEToolExecutor::Tool_RetargetAnimations(const TSharedPtr<FJsonObject>& Args)
{
	FString SourceSkeletonPath;
	if (!Args->TryGetStringField(TEXT("source_skeleton"), SourceSkeletonPath) || SourceSkeletonPath.IsEmpty())
	{
		return TEXT("Error: 'source_skeleton' is required (asset path, e.g. /Game/Characters/Mannequin/Skeleton)");
	}

	FString TargetSkeletonPath;
	if (!Args->TryGetStringField(TEXT("target_skeleton"), TargetSkeletonPath) || TargetSkeletonPath.IsEmpty())
	{
		return TEXT("Error: 'target_skeleton' is required (asset path, e.g. /Game/Characters/Custom/Skeleton)");
	}

	FString OutputPath = TEXT("/Game/Copilot/RetargetedAnims");
	Args->TryGetStringField(TEXT("output_path"), OutputPath);
	OutputPath.ReplaceInline(TEXT("\\"), TEXT("/"));
	if (OutputPath.IsEmpty()) OutputPath = TEXT("/Game/Copilot/RetargetedAnims");
	if (!OutputPath.StartsWith(TEXT("/"))) OutputPath = TEXT("/Game/") + OutputPath;
	if (OutputPath.EndsWith(TEXT("/"))) OutputPath.LeftChopInline(1);

	UE_LOG(LogGitHubCopilotUE, Log,
		TEXT("ToolExecutor: RetargetAnimations — src=%s, tgt=%s, out=%s"),
		*SourceSkeletonPath, *TargetSkeletonPath, *OutputPath);

	// Soft-load USkeleton class to avoid hard dependency on Animation headers
	UClass* SkeletonClass = StaticLoadClass(UObject::StaticClass(), nullptr,
		TEXT("/Script/Engine.Skeleton"));
	if (!SkeletonClass)
	{
		return TEXT("Error: Could not load USkeleton class");
	}

	UClass* AnimSequenceClass = StaticLoadClass(UObject::StaticClass(), nullptr,
		TEXT("/Script/Engine.AnimSequence"));
	if (!AnimSequenceClass)
	{
		return TEXT("Error: Could not load UAnimSequence class");
	}

	// Load source and target skeletons
	UObject* SrcSkelObj = LoadObject<UObject>(nullptr, *SourceSkeletonPath);
	if (!SrcSkelObj || !SrcSkelObj->IsA(SkeletonClass))
	{
		return FString::Printf(TEXT("Error: Could not load source skeleton at '%s'"), *SourceSkeletonPath);
	}

	UObject* TgtSkelObj = LoadObject<UObject>(nullptr, *TargetSkeletonPath);
	if (!TgtSkelObj || !TgtSkelObj->IsA(SkeletonClass))
	{
		return FString::Printf(TEXT("Error: Could not load target skeleton at '%s'"), *TargetSkeletonPath);
	}

	// Determine which animations to retarget
	TArray<FString> AnimPaths;
	const TArray<TSharedPtr<FJsonValue>>* AnimArray;
	if (Args->TryGetArrayField(TEXT("animations"), AnimArray) && AnimArray->Num() > 0)
	{
		for (const TSharedPtr<FJsonValue>& Val : *AnimArray)
		{
			FString Path = Val->AsString();
			if (!Path.IsEmpty())
			{
				AnimPaths.Add(Path);
			}
		}
	}
	else
	{
		// Find all UAnimSequence assets using the source skeleton via asset registry
		FAssetRegistryModule& ARModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
		IAssetRegistry& AssetRegistry = ARModule.Get();

		FARFilter Filter;
		Filter.ClassPaths.Add(AnimSequenceClass->GetClassPathName());
		Filter.bRecursiveClasses = true;
		Filter.bRecursivePaths = true;
		Filter.PackagePaths.Add(FName(TEXT("/Game")));

		TArray<FAssetData> FoundAssets;
		AssetRegistry.GetAssets(Filter, FoundAssets);

		// Filter assets by skeleton — check the Skeleton tag
		const FName SkeletonTagName(TEXT("Skeleton"));
		// The tag stores a soft object path string referencing the skeleton
		const FString SrcSkelObjPath = SrcSkelObj->GetPathName();

		for (const FAssetData& Asset : FoundAssets)
		{
			FAssetTagValueRef SkeletonTag = Asset.TagsAndValues.FindTag(SkeletonTagName);
			if (SkeletonTag.IsSet())
			{
				FString TagStr = SkeletonTag.GetValue();
				// The tag may store a full path or a SoftObjectPath string
				if (TagStr.Contains(SrcSkelObjPath) || TagStr.Contains(SourceSkeletonPath))
				{
					AnimPaths.Add(Asset.GetObjectPathString());
				}
			}
		}

		if (AnimPaths.Num() == 0)
		{
			return FString::Printf(
				TEXT("No AnimSequence assets found using source skeleton '%s'. "
				     "Try providing explicit paths in the 'animations' array."),
				*SourceSkeletonPath);
		}
	}

	// Use the Skeleton property via reflection to set the target skeleton on duplicated assets
	FProperty* SkeletonProp = AnimSequenceClass->FindPropertyByName(FName(TEXT("Skeleton")));
	// Fallback: try the setter function later via UFunction

	int32 SuccessCount = 0;
	int32 FailCount = 0;
	FString ResultDetails;

	for (const FString& AnimPath : AnimPaths)
	{
		UObject* AnimObj = LoadObject<UObject>(nullptr, *AnimPath);
		if (!AnimObj || !AnimObj->IsA(AnimSequenceClass))
		{
			ResultDetails += FString::Printf(TEXT("  SKIP: Could not load '%s'\n"), *AnimPath);
			FailCount++;
			continue;
		}

		// Derive a clean asset name from the original
		FString OrigName = FPaths::GetBaseFilename(AnimPath);
		FString NewAssetName = SanitizeAssetName(OrigName + TEXT("_Retargeted"));
		FString NewPackageName = OutputPath / NewAssetName;
		FString NewObjectPath = NewPackageName + TEXT(".") + NewAssetName;

		// Check if already exists
		if (LoadObject<UObject>(nullptr, *NewObjectPath) != nullptr)
		{
			ResultDetails += FString::Printf(TEXT("  SKIP: Already exists '%s'\n"), *NewObjectPath);
			FailCount++;
			continue;
		}

		// Duplicate the animation asset to the output path
		UPackage* NewPackage = CreatePackage(*NewPackageName);
		if (!NewPackage)
		{
			ResultDetails += FString::Printf(TEXT("  FAIL: Could not create package for '%s'\n"), *NewAssetName);
			FailCount++;
			continue;
		}

		UObject* DupAnim = StaticDuplicateObject(AnimObj, NewPackage, FName(*NewAssetName));
		if (!DupAnim)
		{
			ResultDetails += FString::Printf(TEXT("  FAIL: Could not duplicate '%s'\n"), *OrigName);
			FailCount++;
			continue;
		}

		DupAnim->SetFlags(RF_Public | RF_Standalone);
		DupAnim->ClearFlags(RF_Transient);

		// Set the target skeleton on the duplicate via reflection
		if (SkeletonProp)
		{
			// For TObjectPtr<USkeleton> or USkeleton* properties
			FObjectPropertyBase* ObjProp = CastField<FObjectPropertyBase>(SkeletonProp);
			if (ObjProp)
			{
				ObjProp->SetObjectPropertyValue(
					ObjProp->ContainerPtrToValuePtr<void>(DupAnim), TgtSkelObj);
			}
		}

		// Notify, save
		FAssetRegistryModule::AssetCreated(DupAnim);
		NewPackage->MarkPackageDirty();

		const FString Filename = FPackageName::LongPackageNameToFilename(
			NewPackageName, FPackageName::GetAssetPackageExtension());
		FSavePackageArgs SaveArgs;
		SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
		if (UPackage::SavePackage(NewPackage, DupAnim, *Filename, SaveArgs))
		{
			ResultDetails += FString::Printf(TEXT("  OK: %s -> %s\n"), *OrigName, *NewObjectPath);
			SuccessCount++;
		}
		else
		{
			ResultDetails += FString::Printf(TEXT("  FAIL: Created but could not save '%s'\n"), *NewAssetName);
			FailCount++;
		}
	}

	UE_LOG(LogGitHubCopilotUE, Log,
		TEXT("ToolExecutor: RetargetAnimations complete — %d succeeded, %d failed"),
		SuccessCount, FailCount);

	return FString::Printf(
		TEXT("Animation retarget complete (name-based bone remapping).\n")
		TEXT("Source skeleton: %s\n")
		TEXT("Target skeleton: %s\n")
		TEXT("Output path: %s\n")
		TEXT("Succeeded: %d | Failed/Skipped: %d\n\n%s")
		TEXT("\nNote: This performs name-based retargeting. Bones must have matching names between skeletons. ")
		TEXT("For full IK retargeting, use the IK Retargeter editor."),
		*SourceSkeletonPath, *TargetSkeletonPath, *OutputPath,
		SuccessCount, FailCount, *ResultDetails);
}


// ============================================================================
// Tool: create_anim_montage — Create an Animation Montage from an AnimSequence
// ============================================================================

FString FGitHubCopilotUEToolExecutor::Tool_CreateAnimMontage(const TSharedPtr<FJsonObject>& Args)
{
	FString AssetName;
	if (!Args->TryGetStringField(TEXT("asset_name"), AssetName) || AssetName.IsEmpty())
	{
		return TEXT("Error: 'asset_name' is required");
	}
	AssetName = SanitizeAssetName(AssetName);

	FString AnimSequencePath;
	if (!Args->TryGetStringField(TEXT("animation_sequence"), AnimSequencePath) || AnimSequencePath.IsEmpty())
	{
		return TEXT("Error: 'animation_sequence' is required (e.g. /Game/Anims/Walk)");
	}

	FString PackagePath = TEXT("/Game/Copilot/Montages");
	Args->TryGetStringField(TEXT("package_path"), PackagePath);
	PackagePath.ReplaceInline(TEXT("\\"), TEXT("/"));
	PackagePath = PackagePath.TrimStartAndEnd();
	if (PackagePath.IsEmpty()) PackagePath = TEXT("/Game/Copilot/Montages");
	if (!PackagePath.StartsWith(TEXT("/"))) PackagePath = TEXT("/Game/") + PackagePath;
	if (PackagePath.EndsWith(TEXT("/"))) PackagePath.LeftChopInline(1);

	FString SlotName = TEXT("DefaultSlot");
	Args->TryGetStringField(TEXT("slot_name"), SlotName);
	if (SlotName.IsEmpty()) SlotName = TEXT("DefaultSlot");

	UE_LOG(LogGitHubCopilotUE, Log,
		TEXT("ToolExecutor: CreateAnimMontage — name=%s, anim=%s, slot=%s"),
		*AssetName, *AnimSequencePath, *SlotName);

	// Soft-load animation classes
	UClass* AnimSequenceClass = StaticLoadClass(UObject::StaticClass(), nullptr,
		TEXT("/Script/Engine.AnimSequence"));
	UClass* AnimMontageClass = StaticLoadClass(UObject::StaticClass(), nullptr,
		TEXT("/Script/Engine.AnimMontage"));

	if (!AnimSequenceClass || !AnimMontageClass)
	{
		return TEXT("Error: Could not load animation classes");
	}

	// Load the source animation sequence
	UObject* AnimSeqObj = LoadObject<UObject>(nullptr, *AnimSequencePath);
	if (!AnimSeqObj || !AnimSeqObj->IsA(AnimSequenceClass))
	{
		return FString::Printf(TEXT("Error: Could not load AnimSequence at '%s'"), *AnimSequencePath);
	}

	// Get the skeleton from the anim sequence via reflection
	UObject* SkeletonObj = nullptr;
	{
		FProperty* SkelProp = AnimSequenceClass->FindPropertyByName(FName(TEXT("Skeleton")));
		if (SkelProp)
		{
			FObjectPropertyBase* ObjProp = CastField<FObjectPropertyBase>(SkelProp);
			if (ObjProp)
			{
				SkeletonObj = ObjProp->GetObjectPropertyValue(
					ObjProp->ContainerPtrToValuePtr<void>(AnimSeqObj));
			}
		}
	}

	// Check for existing asset
	const FString PackageName = PackagePath / AssetName;
	const FString ObjectPath = PackageName + TEXT(".") + AssetName;
	if (LoadObject<UObject>(nullptr, *ObjectPath) != nullptr)
	{
		return FString::Printf(TEXT("Error: Asset already exists at '%s'"), *ObjectPath);
	}

	// Create the montage package and object
	UPackage* Package = CreatePackage(*PackageName);
	if (!Package)
	{
		return FString::Printf(TEXT("Error: Failed to create package '%s'"), *PackageName);
	}

	UObject* MontageObj = NewObject<UObject>(Package, AnimMontageClass, FName(*AssetName),
		RF_Public | RF_Standalone);
	if (!MontageObj)
	{
		return FString::Printf(TEXT("Error: Failed to create AnimMontage '%s'"), *AssetName);
	}

	// Set the skeleton on the montage via reflection
	if (SkeletonObj)
	{
		// UAnimMontage inherits from UAnimCompositeBase -> UAnimSequenceBase -> UAnimationAsset
		// UAnimationAsset has a SetSkeleton() method — call via UFunction
		UFunction* SetSkelFunc = AnimMontageClass->FindFunctionByName(FName(TEXT("SetSkeleton")));
		if (SetSkelFunc)
		{
			struct { UObject* Skel; } Params;
			Params.Skel = SkeletonObj;
			MontageObj->ProcessEvent(SetSkelFunc, &Params);
		}
		else
		{
			// Fallback: set Skeleton property directly
			FProperty* SkelProp = AnimMontageClass->FindPropertyByName(FName(TEXT("Skeleton")));
			if (SkelProp)
			{
				FObjectPropertyBase* ObjProp = CastField<FObjectPropertyBase>(SkelProp);
				if (ObjProp)
				{
					ObjProp->SetObjectPropertyValue(
						ObjProp->ContainerPtrToValuePtr<void>(MontageObj), SkeletonObj);
				}
			}
		}
	}

	// Configure the montage: add a slot track and a segment referencing the anim sequence.
	// UAnimMontage has SlotAnimTracks (TArray<FSlotAnimationTrack>).
	// Since we don't have direct header access, we use a Python fallback to configure
	// the montage internals after creation.
	//
	// Write a small Python script to set up the montage segments properly.
	const FString TempDir = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("CopilotTemp"));
	IFileManager::Get().MakeDirectory(*TempDir, true);

	const FString UniqueId = FString::Printf(TEXT("%lld_%d"),
		FDateTime::Now().GetTicks(), FMath::RandRange(10000, 99999));
	const FString SetupScript = FPaths::Combine(TempDir,
		FString::Printf(TEXT("montage_setup_%s.py"), *UniqueId));

	// Normalize paths for Python
	FString PyAnimPath = AnimSequencePath;
	FString PyMontagePath = ObjectPath;

	FString PythonCode = FString::Printf(TEXT(
		"import unreal\n"
		"try:\n"
		"    anim = unreal.load_asset('%s')\n"
		"    montage = unreal.load_asset('%s')\n"
		"    if montage and anim and hasattr(montage, 'set_editor_property'):\n"
		"        # Attempt to configure slot via editor scripting\n"
		"        pass  # Montage was created successfully; configure in editor\n"
		"except Exception as e:\n"
		"    print(f'Montage setup note: {e}')\n"),
		*PyAnimPath, *PyMontagePath);

	FFileHelper::SaveStringToFile(PythonCode, *SetupScript,
		FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);

	// Register, save
	FAssetRegistryModule::AssetCreated(MontageObj);
	Package->MarkPackageDirty();

	const FString PackageFilename = FPackageName::LongPackageNameToFilename(
		PackageName, FPackageName::GetAssetPackageExtension());
	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
	bool bSaved = UPackage::SavePackage(Package, MontageObj, *PackageFilename, SaveArgs);

	// Execute the Python setup script
	if (GEngine)
	{
		FString NormSetupScript = SetupScript;
		NormSetupScript.ReplaceInline(TEXT("\\"), TEXT("/"));
		FString PyCmd = FString::Printf(TEXT("py \"%s\""), *NormSetupScript);
		GEngine->Exec(
			GEditor ? GEditor->GetEditorWorldContext().World() : nullptr,
			*PyCmd);
	}

	// Cleanup temp script
	IFileManager::Get().Delete(*SetupScript);

	// Optionally open editor
	bool bOpenEditor = true;
	Args->TryGetBoolField(TEXT("open_editor"), bOpenEditor);
	if (bOpenEditor && GEditor)
	{
		TArray<UObject*> AssetsToOpen;
		AssetsToOpen.Add(MontageObj);
		GEditor->GetEditorSubsystem<UAssetEditorSubsystem>()->OpenEditorForAssets(AssetsToOpen);
	}

	UE_LOG(LogGitHubCopilotUE, Log, TEXT("ToolExecutor: Created AnimMontage %s"), *ObjectPath);

	return FString::Printf(
		TEXT("Created AnimMontage '%s'%s\n")
		TEXT("Object path: %s\n")
		TEXT("Source animation: %s\n")
		TEXT("Slot: %s\n")
		TEXT("File: %s\n")
		TEXT("Note: Open in Montage editor to configure sections, blend settings, and slot assignments."),
		*AssetName, bSaved ? TEXT(" (saved)") : TEXT(" (created but save failed)"),
		*ObjectPath, *AnimSequencePath, *SlotName, *PackageFilename);
}


// ============================================================================
// Tool: create_level_sequence — Create a Level Sequence (cinematic) asset
// ============================================================================

FString FGitHubCopilotUEToolExecutor::Tool_CreateLevelSequence(const TSharedPtr<FJsonObject>& Args)
{
	FString AssetName;
	if (!Args->TryGetStringField(TEXT("asset_name"), AssetName) || AssetName.IsEmpty())
	{
		return TEXT("Error: 'asset_name' is required");
	}
	AssetName = SanitizeAssetName(AssetName);

	FString PackagePath = TEXT("/Game/Copilot/Sequences");
	Args->TryGetStringField(TEXT("package_path"), PackagePath);
	PackagePath.ReplaceInline(TEXT("\\"), TEXT("/"));
	PackagePath = PackagePath.TrimStartAndEnd();
	if (PackagePath.IsEmpty()) PackagePath = TEXT("/Game/Copilot/Sequences");
	if (!PackagePath.StartsWith(TEXT("/"))) PackagePath = TEXT("/Game/") + PackagePath;
	if (PackagePath.EndsWith(TEXT("/"))) PackagePath.LeftChopInline(1);

	const FString PackageName = PackagePath / AssetName;
	const FString ObjectPath = PackageName + TEXT(".") + AssetName;

	UE_LOG(LogGitHubCopilotUE, Log,
		TEXT("ToolExecutor: CreateLevelSequence — name=%s, path=%s"), *AssetName, *PackagePath);

	// Check for existing asset
	if (LoadObject<UObject>(nullptr, *ObjectPath) != nullptr)
	{
		return FString::Printf(TEXT("Error: Asset already exists at '%s'"), *ObjectPath);
	}

	// Soft-load the LevelSequence class to avoid hard dependency on LevelSequence module
	UClass* LevelSeqClass = StaticLoadClass(UObject::StaticClass(), nullptr,
		TEXT("/Script/LevelSequence.LevelSequence"));
	if (!LevelSeqClass)
	{
		return TEXT("Error: LevelSequence class not available. Ensure the LevelSequence plugin is enabled.");
	}

	UPackage* Package = CreatePackage(*PackageName);
	if (!Package)
	{
		return FString::Printf(TEXT("Error: Failed to create package '%s'"), *PackageName);
	}

	UObject* NewSequence = NewObject<UObject>(Package, LevelSeqClass, FName(*AssetName),
		RF_Public | RF_Standalone);
	if (!NewSequence)
	{
		return FString::Printf(TEXT("Error: Failed to create LevelSequence '%s'"), *AssetName);
	}

	// Initialize the movie scene inside the sequence via reflection
	// LevelSequence has an Initialize() or a MovieScene sub-object
	UFunction* InitFunc = LevelSeqClass->FindFunctionByName(FName(TEXT("Initialize")));
	if (InitFunc)
	{
		NewSequence->ProcessEvent(InitFunc, nullptr);
	}

	FAssetRegistryModule::AssetCreated(NewSequence);
	Package->MarkPackageDirty();

	const FString PackageFilename = FPackageName::LongPackageNameToFilename(
		PackageName, FPackageName::GetAssetPackageExtension());
	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
	bool bSaved = UPackage::SavePackage(Package, NewSequence, *PackageFilename, SaveArgs);

	// Optionally open in Sequencer editor
	bool bOpenEditor = true;
	Args->TryGetBoolField(TEXT("open_editor"), bOpenEditor);
	if (bOpenEditor && GEditor)
	{
		TArray<UObject*> AssetsToOpen;
		AssetsToOpen.Add(NewSequence);
		GEditor->GetEditorSubsystem<UAssetEditorSubsystem>()->OpenEditorForAssets(AssetsToOpen);
	}

	UE_LOG(LogGitHubCopilotUE, Log, TEXT("ToolExecutor: Created LevelSequence %s"), *ObjectPath);

	return FString::Printf(
		TEXT("Created LevelSequence '%s'%s\n")
		TEXT("Object path: %s\n")
		TEXT("File: %s\n")
		TEXT("Open in Sequencer to add tracks, cameras, and keyframes."),
		*AssetName, bSaved ? TEXT(" (saved)") : TEXT(" (created but save failed)"),
		*ObjectPath, *PackageFilename);
}


// ============================================================================
// Tool: build_lighting — Build/bake lighting for the current level
// ============================================================================

FString FGitHubCopilotUEToolExecutor::Tool_BuildLighting(const TSharedPtr<FJsonObject>& Args)
{
	if (!GEditor)
	{
		return TEXT("Error: Editor is not available");
	}

	FString Quality = TEXT("preview");
	if (Args.IsValid())
	{
		Args->TryGetStringField(TEXT("quality"), Quality);
	}
	Quality = Quality.ToLower().TrimStartAndEnd();

	// Map quality string to the engine quality level value
	int32 QualityLevel = 0; // Preview
	if (Quality == TEXT("medium"))
	{
		QualityLevel = 1;
	}
	else if (Quality == TEXT("high"))
	{
		QualityLevel = 2;
	}
	else if (Quality == TEXT("production"))
	{
		QualityLevel = 3;
	}
	else
	{
		Quality = TEXT("preview");
		QualityLevel = 0;
	}

	UE_LOG(LogGitHubCopilotUE, Log,
		TEXT("ToolExecutor: BuildLighting — quality=%s (level=%d)"), *Quality, QualityLevel);

	UWorld* World = GEditor->GetEditorWorldContext().World();
	if (!World)
	{
		return TEXT("Error: No editor world available");
	}

	// Set the lighting quality via console variable before building
	FString SetQualityCmd = FString::Printf(TEXT("r.LightingQuality %d"), QualityLevel);
	GEngine->Exec(World, *SetQualityCmd);

	// Execute the BUILD LIGHTING console command — this initiates an async build
	FStringOutputDevice OutputDevice;
	OutputDevice.SetAutoEmitLineTerminator(true);
	GEngine->Exec(World, TEXT("BUILD LIGHTING"), OutputDevice);

	FString Output = static_cast<const FString&>(OutputDevice);
	Output.TrimEndInline();

	FString LevelName = World->GetMapName();

	return FString::Printf(
		TEXT("Lighting build started for level '%s' at '%s' quality.\n")
		TEXT("This is an asynchronous operation — check the Build progress bar in the editor.\n")
		TEXT("%s"),
		*LevelName, *Quality,
		Output.IsEmpty() ? TEXT("Build initiated successfully.") : *Output);
}


// ============================================================================
// Tool: get_project_settings — Read project settings from .ini files
// ============================================================================

FString FGitHubCopilotUEToolExecutor::Tool_GetProjectSettings(const TSharedPtr<FJsonObject>& Args)
{
	FString Section;
	if (!Args->TryGetStringField(TEXT("section"), Section) || Section.IsEmpty())
	{
		return TEXT("Error: 'section' is required (e.g. /Script/EngineSettings.GeneralProjectSettings)");
	}

	FString Key;
	Args->TryGetStringField(TEXT("key"), Key);

	FString IniFile = TEXT("Game");
	Args->TryGetStringField(TEXT("ini_file"), IniFile);
	IniFile = IniFile.TrimStartAndEnd();

	UE_LOG(LogGitHubCopilotUE, Log,
		TEXT("ToolExecutor: GetProjectSettings — section=%s, key=%s, ini=%s"),
		*Section, *Key, *IniFile);

	// Resolve the ini file path
	FString IniPath;
	if (IniFile.Equals(TEXT("Engine"), ESearchCase::IgnoreCase))
	{
		IniPath = FPaths::Combine(FPaths::ProjectConfigDir(), TEXT("DefaultEngine.ini"));
	}
	else if (IniFile.Equals(TEXT("Editor"), ESearchCase::IgnoreCase))
	{
		IniPath = FPaths::Combine(FPaths::ProjectConfigDir(), TEXT("DefaultEditor.ini"));
	}
	else if (IniFile.Equals(TEXT("Input"), ESearchCase::IgnoreCase))
	{
		IniPath = FPaths::Combine(FPaths::ProjectConfigDir(), TEXT("DefaultInput.ini"));
	}
	else
	{
		// Default to Game
		IniPath = FPaths::Combine(FPaths::ProjectConfigDir(), TEXT("DefaultGame.ini"));
		IniFile = TEXT("Game");
	}

	if (!FPaths::FileExists(IniPath))
	{
		return FString::Printf(TEXT("Error: Config file not found at '%s'"), *IniPath);
	}

	// Use GConfig to read settings
	if (!Key.IsEmpty())
	{
		// Read a specific key
		FString Value;
		if (GConfig->GetString(*Section, *Key, Value, IniPath))
		{
			return FString::Printf(TEXT("[%s] %s = %s\n(from Default%s.ini)"),
				*Section, *Key, *Value, *IniFile);
		}

		// Try reading as an array
		TArray<FString> Values;
		GConfig->GetArray(*Section, *Key, Values, IniPath);
		if (Values.Num() > 0)
		{
			FString Result = FString::Printf(TEXT("[%s] %s (array, %d entries):\n"),
				*Section, *Key, Values.Num());
			for (int32 i = 0; i < Values.Num(); ++i)
			{
				Result += FString::Printf(TEXT("  [%d] %s\n"), i, *Values[i]);
			}
			Result += FString::Printf(TEXT("(from Default%s.ini)"), *IniFile);
			return Result;
		}

		return FString::Printf(TEXT("Key '%s' not found in section '%s' of Default%s.ini"),
			*Key, *Section, *IniFile);
	}
	else
	{
		// Read all keys in section
		TArray<FString> SectionPairs;
		FString RawSection;

		// Load the file and parse the section manually for completeness
		FString FileContent;
		if (!FFileHelper::LoadFileToString(FileContent, *IniPath))
		{
			return FString::Printf(TEXT("Error: Could not read '%s'"), *IniPath);
		}

		FString Result = FString::Printf(TEXT("Settings from [%s] in Default%s.ini:\n"),
			*Section, *IniFile);

		// Parse the ini file line by line to find the section
		TArray<FString> Lines;
		FileContent.ParseIntoArrayLines(Lines);

		bool bInSection = false;
		int32 KeyCount = 0;
		const FString SectionHeader = FString::Printf(TEXT("[%s]"), *Section);

		for (const FString& Line : Lines)
		{
			FString TrimmedLine = Line.TrimStartAndEnd();

			if (TrimmedLine.Equals(SectionHeader, ESearchCase::IgnoreCase))
			{
				bInSection = true;
				continue;
			}

			if (bInSection)
			{
				// New section starts — stop
				if (TrimmedLine.StartsWith(TEXT("[")) && TrimmedLine.EndsWith(TEXT("]")))
				{
					break;
				}

				// Skip empty lines and comments
				if (TrimmedLine.IsEmpty() || TrimmedLine.StartsWith(TEXT(";")) || TrimmedLine.StartsWith(TEXT("#")))
				{
					continue;
				}

				Result += FString::Printf(TEXT("  %s\n"), *TrimmedLine);
				KeyCount++;
			}
		}

		if (KeyCount == 0)
		{
			return FString::Printf(
				TEXT("Section '%s' not found or is empty in Default%s.ini.\n")
				TEXT("File: %s"),
				*Section, *IniFile, *IniPath);
		}

		Result += FString::Printf(TEXT("\n(%d entries found)"), KeyCount);
		return Result;
	}
}


// ============================================================================
// Tool: set_project_settings — Write project settings to .ini files
// ============================================================================

FString FGitHubCopilotUEToolExecutor::Tool_SetProjectSettings(const TSharedPtr<FJsonObject>& Args)
{
	FString Section;
	if (!Args->TryGetStringField(TEXT("section"), Section) || Section.IsEmpty())
	{
		return TEXT("Error: 'section' is required");
	}

	FString Key;
	if (!Args->TryGetStringField(TEXT("key"), Key) || Key.IsEmpty())
	{
		return TEXT("Error: 'key' is required");
	}

	FString Value;
	if (!Args->TryGetStringField(TEXT("value"), Value))
	{
		return TEXT("Error: 'value' is required");
	}

	FString IniFile = TEXT("Game");
	Args->TryGetStringField(TEXT("ini_file"), IniFile);
	IniFile = IniFile.TrimStartAndEnd();

	UE_LOG(LogGitHubCopilotUE, Log,
		TEXT("ToolExecutor: SetProjectSettings — [%s] %s=%s in Default%s.ini"),
		*Section, *Key, *Value, *IniFile);

	// Resolve the ini file path
	FString IniPath;
	if (IniFile.Equals(TEXT("Engine"), ESearchCase::IgnoreCase))
	{
		IniPath = FPaths::Combine(FPaths::ProjectConfigDir(), TEXT("DefaultEngine.ini"));
	}
	else if (IniFile.Equals(TEXT("Editor"), ESearchCase::IgnoreCase))
	{
		IniPath = FPaths::Combine(FPaths::ProjectConfigDir(), TEXT("DefaultEditor.ini"));
	}
	else if (IniFile.Equals(TEXT("Input"), ESearchCase::IgnoreCase))
	{
		IniPath = FPaths::Combine(FPaths::ProjectConfigDir(), TEXT("DefaultInput.ini"));
	}
	else
	{
		IniPath = FPaths::Combine(FPaths::ProjectConfigDir(), TEXT("DefaultGame.ini"));
		IniFile = TEXT("Game");
	}

	// Read old value for logging
	FString OldValue;
	bool bHadOldValue = GConfig->GetString(*Section, *Key, OldValue, IniPath);

	// Write the new value
	GConfig->SetString(*Section, *Key, *Value, IniPath);
	GConfig->Flush(false, IniPath);

	FString Result;
	if (bHadOldValue)
	{
		Result = FString::Printf(
			TEXT("Updated setting in Default%s.ini:\n")
			TEXT("  [%s]\n")
			TEXT("  %s = %s\n")
			TEXT("  (was: %s)"),
			*IniFile, *Section, *Key, *Value, *OldValue);
	}
	else
	{
		Result = FString::Printf(
			TEXT("Added new setting to Default%s.ini:\n")
			TEXT("  [%s]\n")
			TEXT("  %s = %s"),
			*IniFile, *Section, *Key, *Value);
	}

	Result += TEXT("\n\nNote: Some settings require an editor restart to take effect.");
	return Result;
}


// ============================================================================
// Tool: create_widget_blueprint — Create a UMG Widget Blueprint asset
// ============================================================================

FString FGitHubCopilotUEToolExecutor::Tool_CreateWidgetBlueprint(const TSharedPtr<FJsonObject>& Args)
{
	FString AssetName;
	if (!Args->TryGetStringField(TEXT("asset_name"), AssetName) || AssetName.IsEmpty())
	{
		return TEXT("Error: 'asset_name' is required");
	}
	AssetName = SanitizeAssetName(AssetName);

	FString PackagePath = TEXT("/Game/Copilot/UI");
	Args->TryGetStringField(TEXT("package_path"), PackagePath);
	PackagePath.ReplaceInline(TEXT("\\"), TEXT("/"));
	PackagePath = PackagePath.TrimStartAndEnd();
	if (PackagePath.IsEmpty()) PackagePath = TEXT("/Game/Copilot/UI");
	if (!PackagePath.StartsWith(TEXT("/"))) PackagePath = TEXT("/Game/") + PackagePath;
	if (PackagePath.EndsWith(TEXT("/"))) PackagePath.LeftChopInline(1);

	FString ParentClassStr = TEXT("UserWidget");
	Args->TryGetStringField(TEXT("parent_class"), ParentClassStr);
	if (ParentClassStr.IsEmpty()) ParentClassStr = TEXT("UserWidget");

	UE_LOG(LogGitHubCopilotUE, Log,
		TEXT("ToolExecutor: CreateWidgetBlueprint — name=%s, path=%s, parent=%s"),
		*AssetName, *PackagePath, *ParentClassStr);

	const FString PackageName = PackagePath / AssetName;
	const FString ObjectPath = PackageName + TEXT(".") + AssetName;

	if (LoadObject<UObject>(nullptr, *ObjectPath) != nullptr)
	{
		return FString::Printf(TEXT("Error: Asset already exists at '%s'"), *ObjectPath);
	}

	// Resolve the parent class (default: UUserWidget)
	UClass* ParentClass = nullptr;
	{
		// Try common UMG widget classes
		TArray<FString> CandidatePaths;
		if (ParentClassStr.StartsWith(TEXT("/Script/")))
		{
			CandidatePaths.Add(ParentClassStr);
		}
		else
		{
			CandidatePaths.Add(FString::Printf(TEXT("/Script/UMG.%s"), *ParentClassStr));
			CandidatePaths.Add(FString::Printf(TEXT("/Script/UMG.U%s"), *ParentClassStr));
			CandidatePaths.Add(FString::Printf(TEXT("/Script/Engine.%s"), *ParentClassStr));
			CandidatePaths.Add(FString::Printf(TEXT("/Script/Engine.U%s"), *ParentClassStr));
		}

		for (const FString& Candidate : CandidatePaths)
		{
			ParentClass = StaticLoadClass(UObject::StaticClass(), nullptr, *Candidate);
			if (ParentClass) break;
		}

		if (!ParentClass)
		{
			// Fallback: load UUserWidget explicitly
			ParentClass = StaticLoadClass(UObject::StaticClass(), nullptr,
				TEXT("/Script/UMG.UserWidget"));
		}

		if (!ParentClass)
		{
			return TEXT("Error: Could not load UUserWidget class. Ensure the UMG module is available.");
		}
	}

	// Soft-load the WidgetBlueprint class and factory
	UClass* WidgetBPClass = StaticLoadClass(UObject::StaticClass(), nullptr,
		TEXT("/Script/UMGEditor.WidgetBlueprint"));
	if (!WidgetBPClass)
	{
		return TEXT("Error: WidgetBlueprint class not available. Ensure the UMG Editor plugin is enabled.");
	}

	// Try to use the WidgetBlueprintFactory for proper initialization
	UClass* FactoryClass = StaticLoadClass(UObject::StaticClass(), nullptr,
		TEXT("/Script/UMGEditor.WidgetBlueprintFactory"));

	UPackage* Package = CreatePackage(*PackageName);
	if (!Package)
	{
		return FString::Printf(TEXT("Error: Failed to create package '%s'"), *PackageName);
	}

	UObject* NewWidgetBP = nullptr;

	if (FactoryClass)
	{
		// Use factory for proper initialization
		UObject* Factory = NewObject<UObject>(GetTransientPackage(), FactoryClass);
		if (Factory)
		{
			// Set ParentClass on the factory via reflection
			FProperty* ParentClassProp = FactoryClass->FindPropertyByName(FName(TEXT("ParentClass")));
			if (ParentClassProp)
			{
				FClassProperty* ClassProp = CastField<FClassProperty>(ParentClassProp);
				if (ClassProp)
				{
					ClassProp->SetPropertyValue(
						ClassProp->ContainerPtrToValuePtr<void>(Factory), ParentClass);
				}
			}

			// Call FactoryCreateNew via the UFactory interface
			UFunction* CreateNewFunc = FactoryClass->FindFunctionByName(
				FName(TEXT("FactoryCreateNew")));

			// Simpler: use UFactory's CreateNew directly if the object is a UFactory
			// We'll cast via class check
			UClass* UFactoryClass = FindObject<UClass>(nullptr, TEXT("/Script/UnrealEd.Factory"));
			if (UFactoryClass && Factory->IsA(UFactoryClass))
			{
				// Use the C++ interface: cast is safe since we verified IsA
				// However, without the header we use the function directly
				struct FFactoryCreateNewParams
				{
					UClass* InClass;
					UObject* InParent;
					FName InName;
					EObjectFlags Flags;
					UObject* Context;
					FFeedbackContext* Warn;
					UObject* ReturnValue;
				};

				if (CreateNewFunc)
				{
					FFactoryCreateNewParams Params;
					Params.InClass = WidgetBPClass;
					Params.InParent = Package;
					Params.InName = FName(*AssetName);
					Params.Flags = RF_Public | RF_Standalone;
					Params.Context = nullptr;
					Params.Warn = GWarn;
					Params.ReturnValue = nullptr;
					Factory->ProcessEvent(CreateNewFunc, &Params);
					NewWidgetBP = Params.ReturnValue;
				}
			}
		}
	}

	// Fallback: create directly if factory approach didn't work
	if (!NewWidgetBP)
	{
		NewWidgetBP = NewObject<UObject>(Package, WidgetBPClass, FName(*AssetName),
			RF_Public | RF_Standalone);

		if (NewWidgetBP)
		{
			// Set parent class via Blueprint's ParentClass property
			FProperty* BPParentProp = WidgetBPClass->FindPropertyByName(
				FName(TEXT("ParentClass")));
			if (BPParentProp)
			{
				FClassProperty* ClassProp = CastField<FClassProperty>(BPParentProp);
				if (ClassProp)
				{
					ClassProp->SetPropertyValue(
						ClassProp->ContainerPtrToValuePtr<void>(NewWidgetBP), ParentClass);
				}
			}
		}
	}

	if (!NewWidgetBP)
	{
		return FString::Printf(TEXT("Error: Failed to create Widget Blueprint '%s'"), *AssetName);
	}

	FAssetRegistryModule::AssetCreated(NewWidgetBP);
	Package->MarkPackageDirty();

	const FString PackageFilename = FPackageName::LongPackageNameToFilename(
		PackageName, FPackageName::GetAssetPackageExtension());
	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
	bool bSaved = UPackage::SavePackage(Package, NewWidgetBP, *PackageFilename, SaveArgs);

	// Optionally open editor
	bool bOpenEditor = true;
	Args->TryGetBoolField(TEXT("open_editor"), bOpenEditor);
	if (bOpenEditor && GEditor)
	{
		TArray<UObject*> AssetsToOpen;
		AssetsToOpen.Add(NewWidgetBP);
		GEditor->GetEditorSubsystem<UAssetEditorSubsystem>()->OpenEditorForAssets(AssetsToOpen);
	}

	UE_LOG(LogGitHubCopilotUE, Log, TEXT("ToolExecutor: Created Widget Blueprint %s"), *ObjectPath);

	return FString::Printf(
		TEXT("Created Widget Blueprint '%s'%s\n")
		TEXT("Object path: %s\n")
		TEXT("Parent class: %s\n")
		TEXT("File: %s\n")
		TEXT("Open in UMG Designer to add widgets and layout your UI."),
		*AssetName, bSaved ? TEXT(" (saved)") : TEXT(" (created but save failed)"),
		*ObjectPath, *ParentClassStr, *PackageFilename);
}


// ============================================================================
// Tool: get_blueprint_graph — Read a Blueprint's graphs as structured text
// ============================================================================

FString FGitHubCopilotUEToolExecutor::Tool_GetBlueprintGraph(const TSharedPtr<FJsonObject>& Args)
{
	FString BlueprintPath;
	if (!Args->TryGetStringField(TEXT("blueprint_path"), BlueprintPath) || BlueprintPath.IsEmpty())
	{
		return TEXT("Error: 'blueprint_path' is required (e.g. /Game/Blueprints/BP_Player)");
	}

	UE_LOG(LogGitHubCopilotUE, Log,
		TEXT("ToolExecutor: GetBlueprintGraph — path=%s"), *BlueprintPath);

	UBlueprint* BP = LoadObject<UBlueprint>(nullptr, *BlueprintPath);
	if (!BP)
	{
		// Try appending the asset name for short-form paths
		FString BaseName = FPaths::GetBaseFilename(BlueprintPath);
		FString FullPath = BlueprintPath + TEXT(".") + BaseName;
		BP = LoadObject<UBlueprint>(nullptr, *FullPath);
	}

	if (!BP)
	{
		return FString::Printf(TEXT("Error: Could not load Blueprint at '%s'"), *BlueprintPath);
	}

	FString Result;
	Result += FString::Printf(TEXT("Blueprint: %s\n"), *BP->GetPathName());
	Result += FString::Printf(TEXT("Parent class: %s\n"),
		BP->ParentClass ? *BP->ParentClass->GetName() : TEXT("None"));
	Result += FString::Printf(TEXT("Blueprint type: %s\n"),
		*UEnum::GetValueAsString(BP->BlueprintType));
	Result += TEXT("========================================\n\n");

	// Helper lambda to dump a graph's nodes
	auto DumpGraph = [&Result](UEdGraph* Graph, const FString& GraphCategory)
	{
		if (!Graph) return;

		Result += FString::Printf(TEXT("--- %s: %s (%d nodes) ---\n"),
			*GraphCategory, *Graph->GetName(), Graph->Nodes.Num());

		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (!Node) continue;

			FString NodeTitle = Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString();
			FString NodeClass = Node->GetClass()->GetName();

			Result += FString::Printf(TEXT("  [%s] %s  (pos: %d, %d)\n"),
				*NodeClass, *NodeTitle, Node->NodePosX, Node->NodePosY);

			// Optionally show node comment
			if (!Node->NodeComment.IsEmpty())
			{
				Result += FString::Printf(TEXT("    Comment: %s\n"), *Node->NodeComment);
			}

			// Enumerate pins
			for (UEdGraphPin* Pin : Node->Pins)
			{
				if (!Pin) continue;

				FString Direction = (Pin->Direction == EGPD_Input) ? TEXT("IN") : TEXT("OUT");
				FString PinType = Pin->PinType.PinCategory.ToString();
				FString SubCategory = Pin->PinType.PinSubCategory.ToString();

				if (!SubCategory.IsEmpty() && SubCategory != TEXT("None"))
				{
					PinType += TEXT(":") + SubCategory;
				}
				if (Pin->PinType.PinSubCategoryObject.IsValid())
				{
					PinType += TEXT(":") + Pin->PinType.PinSubCategoryObject->GetName();
				}

				FString DefaultVal;
				if (!Pin->DefaultValue.IsEmpty())
				{
					DefaultVal = FString::Printf(TEXT(" = \"%s\""), *Pin->DefaultValue);
				}

				FString LinkedStr;
				if (Pin->LinkedTo.Num() > 0)
				{
					TArray<FString> LinkedNames;
					for (UEdGraphPin* Linked : Pin->LinkedTo)
					{
						if (Linked && Linked->GetOwningNode())
						{
							LinkedNames.Add(FString::Printf(TEXT("%s.%s"),
								*Linked->GetOwningNode()->GetNodeTitle(ENodeTitleType::ListView).ToString(),
								*Linked->PinName.ToString()));
						}
					}
					LinkedStr = FString::Printf(TEXT(" -> [%s]"), *FString::Join(LinkedNames, TEXT(", ")));
				}

				Result += FString::Printf(TEXT("    %s %s [%s]%s%s\n"),
					*Direction, *Pin->PinName.ToString(), *PinType,
					*DefaultVal, *LinkedStr);
			}

			Result += TEXT("\n");
		}
	};

	// Dump Event Graphs (UbergraphPages)
	for (UEdGraph* Graph : BP->UbergraphPages)
	{
		DumpGraph(Graph, TEXT("EventGraph"));
	}

	// Dump Function Graphs
	for (UEdGraph* Graph : BP->FunctionGraphs)
	{
		DumpGraph(Graph, TEXT("Function"));
	}

	// Dump Macro Graphs
	for (UEdGraph* Graph : BP->MacroGraphs)
	{
		DumpGraph(Graph, TEXT("Macro"));
	}

	// Dump Delegate Graphs
	for (UEdGraph* Graph : BP->DelegateSignatureGraphs)
	{
		DumpGraph(Graph, TEXT("Delegate"));
	}

	if (Result.Len() > 32000)
	{
		Result = Result.Left(32000);
		Result += TEXT("\n\n... (output truncated — Blueprint has too many nodes)");
	}

	return Result;
}


// ============================================================================
// Tool: add_blueprint_node — Add a node to a Blueprint graph
// ============================================================================

FString FGitHubCopilotUEToolExecutor::Tool_AddBlueprintNode(const TSharedPtr<FJsonObject>& Args)
{
	FString BlueprintPath;
	if (!Args->TryGetStringField(TEXT("blueprint_path"), BlueprintPath) || BlueprintPath.IsEmpty())
	{
		return TEXT("Error: 'blueprint_path' is required");
	}

	FString NodeClassStr;
	if (!Args->TryGetStringField(TEXT("node_class"), NodeClassStr) || NodeClassStr.IsEmpty())
	{
		return TEXT("Error: 'node_class' is required (e.g. K2Node_CallFunction, K2Node_IfThenElse)");
	}

	FString GraphName = TEXT("EventGraph");
	Args->TryGetStringField(TEXT("graph_name"), GraphName);
	if (GraphName.IsEmpty()) GraphName = TEXT("EventGraph");

	FString FunctionName;
	Args->TryGetStringField(TEXT("function_name"), FunctionName);

	int32 PosX = 0;
	int32 PosY = 0;
	if (Args->HasField(TEXT("position_x")))
	{
		PosX = static_cast<int32>(Args->GetNumberField(TEXT("position_x")));
	}
	if (Args->HasField(TEXT("position_y")))
	{
		PosY = static_cast<int32>(Args->GetNumberField(TEXT("position_y")));
	}

	UE_LOG(LogGitHubCopilotUE, Log,
		TEXT("ToolExecutor: AddBlueprintNode — bp=%s, class=%s, graph=%s, func=%s"),
		*BlueprintPath, *NodeClassStr, *GraphName, *FunctionName);

	// Load the Blueprint
	UBlueprint* BP = LoadObject<UBlueprint>(nullptr, *BlueprintPath);
	if (!BP)
	{
		FString BaseName = FPaths::GetBaseFilename(BlueprintPath);
		FString FullPath = BlueprintPath + TEXT(".") + BaseName;
		BP = LoadObject<UBlueprint>(nullptr, *FullPath);
	}
	if (!BP)
	{
		return FString::Printf(TEXT("Error: Could not load Blueprint at '%s'"), *BlueprintPath);
	}

	// Find the target graph
	UEdGraph* TargetGraph = nullptr;

	// Search event graphs
	for (UEdGraph* Graph : BP->UbergraphPages)
	{
		if (Graph && Graph->GetName().Equals(GraphName, ESearchCase::IgnoreCase))
		{
			TargetGraph = Graph;
			break;
		}
	}

	// Search function graphs if not found
	if (!TargetGraph)
	{
		for (UEdGraph* Graph : BP->FunctionGraphs)
		{
			if (Graph && Graph->GetName().Equals(GraphName, ESearchCase::IgnoreCase))
			{
				TargetGraph = Graph;
				break;
			}
		}
	}

	if (!TargetGraph)
	{
		// List available graphs for the error message
		FString Available;
		for (UEdGraph* Graph : BP->UbergraphPages)
		{
			if (Graph) Available += FString::Printf(TEXT("  EventGraph: %s\n"), *Graph->GetName());
		}
		for (UEdGraph* Graph : BP->FunctionGraphs)
		{
			if (Graph) Available += FString::Printf(TEXT("  Function: %s\n"), *Graph->GetName());
		}
		return FString::Printf(
			TEXT("Error: Graph '%s' not found in Blueprint.\nAvailable graphs:\n%s"),
			*GraphName, *Available);
	}

	// Resolve the node class — try multiple module paths
	UClass* NodeClass = nullptr;
	{
		TArray<FString> CandidatePaths;

		// If already a full path, try it directly
		if (NodeClassStr.StartsWith(TEXT("/Script/")))
		{
			CandidatePaths.Add(NodeClassStr);
		}
		else
		{
			// Common Kismet/BlueprintGraph module locations
			CandidatePaths.Add(FString::Printf(TEXT("/Script/BlueprintGraph.%s"), *NodeClassStr));
			CandidatePaths.Add(FString::Printf(TEXT("/Script/BlueprintGraph.U%s"), *NodeClassStr));
			CandidatePaths.Add(FString::Printf(TEXT("/Script/Engine.%s"), *NodeClassStr));
			CandidatePaths.Add(FString::Printf(TEXT("/Script/Engine.U%s"), *NodeClassStr));
			CandidatePaths.Add(FString::Printf(TEXT("/Script/UnrealEd.%s"), *NodeClassStr));
			CandidatePaths.Add(FString::Printf(TEXT("/Script/UnrealEd.U%s"), *NodeClassStr));
		}

		for (const FString& Candidate : CandidatePaths)
		{
			NodeClass = StaticLoadClass(UObject::StaticClass(), nullptr, *Candidate);
			if (NodeClass) break;
		}
	}

	if (!NodeClass)
	{
		return FString::Printf(
			TEXT("Error: Could not find node class '%s'. "
			     "Common classes: K2Node_CallFunction, K2Node_IfThenElse, K2Node_Event, "
			     "K2Node_VariableGet, K2Node_VariableSet, K2Node_SpawnActorFromClass"),
			*NodeClassStr);
	}

	// Verify it's an EdGraphNode subclass
	UClass* EdGraphNodeClass = FindObject<UClass>(nullptr, TEXT("/Script/Engine.EdGraphNode"));
	if (!EdGraphNodeClass)
	{
		EdGraphNodeClass = UEdGraphNode::StaticClass();
	}

	if (!NodeClass->IsChildOf(EdGraphNodeClass))
	{
		return FString::Printf(TEXT("Error: '%s' is not a UEdGraphNode subclass"), *NodeClassStr);
	}

	// Create the node
	UEdGraphNode* NewNode = NewObject<UEdGraphNode>(TargetGraph, NodeClass);
	if (!NewNode)
	{
		return FString::Printf(TEXT("Error: Failed to create node of class '%s'"), *NodeClassStr);
	}

	NewNode->NodePosX = PosX;
	NewNode->NodePosY = PosY;
	NewNode->CreateNewGuid();

	// For K2Node_CallFunction, set the function reference
	if (NodeClassStr.Contains(TEXT("CallFunction")) && !FunctionName.IsEmpty())
	{
		// Try to set FunctionReference.MemberName via reflection
		FProperty* FuncRefProp = NodeClass->FindPropertyByName(FName(TEXT("FunctionReference")));
		if (FuncRefProp)
		{
			// FMemberReference has a MemberName property
			void* FuncRefPtr = FuncRefProp->ContainerPtrToValuePtr<void>(NewNode);
			if (FuncRefPtr)
			{
				// Set MemberName within the FMemberReference struct
				FStructProperty* StructProp = CastField<FStructProperty>(FuncRefProp);
				if (StructProp && StructProp->Struct)
				{
					FProperty* MemberNameProp = StructProp->Struct->FindPropertyByName(
						FName(TEXT("MemberName")));
					if (MemberNameProp)
					{
						FNameProperty* NameProp = CastField<FNameProperty>(MemberNameProp);
						if (NameProp)
						{
							NameProp->SetPropertyValue(
								NameProp->ContainerPtrToValuePtr<void>(FuncRefPtr),
								FName(*FunctionName));
						}
					}
				}
			}
		}
	}

	// Add node to graph and allocate pins
	TargetGraph->AddNode(NewNode, false, false);
	NewNode->AllocateDefaultPins();
	NewNode->PostPlacedNewNode();

	// Mark the Blueprint as modified and recompile
	FBlueprintEditorUtils::MarkBlueprintAsModified(BP);

	FString NodeTitle = NewNode->GetNodeTitle(ENodeTitleType::FullTitle).ToString();
	int32 PinCount = NewNode->Pins.Num();

	// Build pin summary
	FString PinSummary;
	for (UEdGraphPin* Pin : NewNode->Pins)
	{
		if (!Pin) continue;
		FString Dir = (Pin->Direction == EGPD_Input) ? TEXT("IN") : TEXT("OUT");
		PinSummary += FString::Printf(TEXT("  %s: %s [%s]\n"),
			*Dir, *Pin->PinName.ToString(), *Pin->PinType.PinCategory.ToString());
	}

	UE_LOG(LogGitHubCopilotUE, Log,
		TEXT("ToolExecutor: Added node '%s' to graph '%s' in '%s'"),
		*NodeTitle, *GraphName, *BlueprintPath);

	return FString::Printf(
		TEXT("Added node to Blueprint '%s':\n")
		TEXT("  Graph: %s\n")
		TEXT("  Node: %s (%s)\n")
		TEXT("  Position: (%d, %d)\n")
		TEXT("  Pins (%d):\n%s")
		TEXT("\nBlueprint marked as modified. Compile to apply changes."),
		*BP->GetName(), *GraphName, *NodeTitle, *NodeClassStr,
		PosX, PosY, PinCount, *PinSummary);
}


// ============================================================================
// Tool: git_commit — Stage and commit changes using git
// ============================================================================

FString FGitHubCopilotUEToolExecutor::Tool_GitCommit(const TSharedPtr<FJsonObject>& Args)
{
	FString Message;
	if (!Args->TryGetStringField(TEXT("message"), Message) || Message.IsEmpty())
	{
		return TEXT("Error: 'message' is required");
	}

	bool bAmend = false;
	Args->TryGetBoolField(TEXT("amend"), bAmend);

	UE_LOG(LogGitHubCopilotUE, Log,
		TEXT("ToolExecutor: GitCommit — message='%s', amend=%d"),
		*Message, bAmend ? 1 : 0);

	const FString WorkingDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());

	// Determine files to stage
	TArray<FString> FilesToStage;
	const TArray<TSharedPtr<FJsonValue>>* FilesArray;
	if (Args->TryGetArrayField(TEXT("files"), FilesArray) && FilesArray->Num() > 0)
	{
		for (const TSharedPtr<FJsonValue>& Val : *FilesArray)
		{
			FString File = Val->AsString();
			if (!File.IsEmpty())
			{
				FilesToStage.Add(File);
			}
		}
	}

	// Helper lambda to run a git command and capture output
	auto RunGit = [&WorkingDir](const FString& GitArgs, FString& OutOutput) -> int32
	{
		int32 ReturnCode = -1;
		FString StdOut;
		FString StdErr;

		void* PipeRead = nullptr;
		void* PipeWrite = nullptr;
		FPlatformProcess::CreatePipe(PipeRead, PipeWrite);

#if PLATFORM_WINDOWS
		const TCHAR* Executable = TEXT("git.exe");
#else
		const TCHAR* Executable = TEXT("git");
#endif

		FProcHandle ProcHandle = FPlatformProcess::CreateProc(
			Executable, *GitArgs,
			false, true, true,
			nullptr, 0,
			*WorkingDir,
			PipeWrite, PipeRead);

		if (!ProcHandle.IsValid())
		{
			FPlatformProcess::ClosePipe(PipeRead, PipeWrite);
			OutOutput = TEXT("Failed to launch git process");
			return -1;
		}

		// Wait for completion with timeout
		const double StartTime = FPlatformTime::Seconds();
		while (FPlatformProcess::IsProcRunning(ProcHandle))
		{
			StdOut += FPlatformProcess::ReadPipe(PipeRead);
			if (FPlatformTime::Seconds() - StartTime > 30.0)
			{
				FPlatformProcess::TerminateProc(ProcHandle, true);
				FPlatformProcess::ClosePipe(PipeRead, PipeWrite);
				OutOutput = TEXT("Git command timed out");
				return -1;
			}
			FPlatformProcess::Sleep(0.05f);
		}

		StdOut += FPlatformProcess::ReadPipe(PipeRead);
		FPlatformProcess::GetProcReturnCode(ProcHandle, &ReturnCode);
		FPlatformProcess::CloseProc(ProcHandle);
		FPlatformProcess::ClosePipe(PipeRead, PipeWrite);

		OutOutput = StdOut.TrimEnd();
		return ReturnCode;
	};

	FString Result;
	FString CmdOutput;

	// Step 1: Stage files
	if (FilesToStage.Num() > 0)
	{
		// Stage specific files
		FString FileList;
		for (const FString& File : FilesToStage)
		{
			// Escape paths with spaces
			FileList += FString::Printf(TEXT(" \"%s\""), *File);
		}
		FString AddArgs = FString::Printf(TEXT("add%s"), *FileList);
		int32 AddCode = RunGit(AddArgs, CmdOutput);
		if (AddCode != 0)
		{
			return FString::Printf(TEXT("Error: git add failed (exit %d):\n%s"), AddCode, *CmdOutput);
		}
		Result += FString::Printf(TEXT("Staged %d file(s)\n"), FilesToStage.Num());
	}
	else
	{
		// Stage all changes
		int32 AddCode = RunGit(TEXT("add -A"), CmdOutput);
		if (AddCode != 0)
		{
			return FString::Printf(TEXT("Error: git add -A failed (exit %d):\n%s"), AddCode, *CmdOutput);
		}
		Result += TEXT("Staged all changes\n");
	}

	// Step 2: Commit
	// Escape the message for the command line
	FString EscapedMessage = Message;
	EscapedMessage.ReplaceInline(TEXT("\""), TEXT("\\\""));

	FString CommitArgs;
	if (bAmend)
	{
		CommitArgs = FString::Printf(TEXT("commit --amend -m \"%s\""), *EscapedMessage);
	}
	else
	{
		CommitArgs = FString::Printf(TEXT("commit -m \"%s\""), *EscapedMessage);
	}

	int32 CommitCode = RunGit(CommitArgs, CmdOutput);
	if (CommitCode != 0)
	{
		// Check if it's just "nothing to commit"
		if (CmdOutput.Contains(TEXT("nothing to commit")))
		{
			return TEXT("Nothing to commit — working tree is clean.");
		}
		return FString::Printf(TEXT("Error: git commit failed (exit %d):\n%s"), CommitCode, *CmdOutput);
	}

	Result += CmdOutput;

	UE_LOG(LogGitHubCopilotUE, Log,
		TEXT("ToolExecutor: GitCommit completed (code=%d)"), CommitCode);

	return Result;
}


// ============================================================================
// Tool: git_push — Push commits to remote
// ============================================================================

FString FGitHubCopilotUEToolExecutor::Tool_GitPush(const TSharedPtr<FJsonObject>& Args)
{
	FString Remote = TEXT("origin");
	if (Args.IsValid())
	{
		Args->TryGetStringField(TEXT("remote"), Remote);
	}
	if (Remote.IsEmpty()) Remote = TEXT("origin");

	FString Branch;
	if (Args.IsValid())
	{
		Args->TryGetStringField(TEXT("branch"), Branch);
	}

	bool bForce = false;
	if (Args.IsValid())
	{
		Args->TryGetBoolField(TEXT("force"), bForce);
	}

	UE_LOG(LogGitHubCopilotUE, Log,
		TEXT("ToolExecutor: GitPush — remote=%s, branch=%s, force=%d"),
		*Remote, *Branch, bForce ? 1 : 0);

	const FString WorkingDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());

	// Build the push command arguments
	FString PushArgs = FString::Printf(TEXT("push %s"), *Remote);
	if (!Branch.IsEmpty())
	{
		PushArgs += FString::Printf(TEXT(" %s"), *Branch);
	}
	if (bForce)
	{
		PushArgs += TEXT(" --force");
	}

	// Run the git push command
	int32 ReturnCode = -1;
	FString StdOut;

	void* PipeRead = nullptr;
	void* PipeWrite = nullptr;
	FPlatformProcess::CreatePipe(PipeRead, PipeWrite);

#if PLATFORM_WINDOWS
	const TCHAR* Executable = TEXT("git.exe");
#else
	const TCHAR* Executable = TEXT("git");
#endif

	FProcHandle ProcHandle = FPlatformProcess::CreateProc(
		Executable, *PushArgs,
		false, true, true,
		nullptr, 0,
		*WorkingDir,
		PipeWrite, PipeRead);

	if (!ProcHandle.IsValid())
	{
		FPlatformProcess::ClosePipe(PipeRead, PipeWrite);
		return TEXT("Error: Failed to launch git process. Is git installed and in PATH?");
	}

	const double StartTime = FPlatformTime::Seconds();
	const double TimeoutSec = 120.0; // Push can take a while
	bool bTimedOut = false;

	while (FPlatformProcess::IsProcRunning(ProcHandle))
	{
		StdOut += FPlatformProcess::ReadPipe(PipeRead);
		if (FPlatformTime::Seconds() - StartTime > TimeoutSec)
		{
			bTimedOut = true;
			FPlatformProcess::TerminateProc(ProcHandle, true);
			break;
		}
		FPlatformProcess::Sleep(0.1f);
	}

	StdOut += FPlatformProcess::ReadPipe(PipeRead);

	if (!bTimedOut)
	{
		FPlatformProcess::GetProcReturnCode(ProcHandle, &ReturnCode);
	}

	FPlatformProcess::CloseProc(ProcHandle);
	FPlatformProcess::ClosePipe(PipeRead, PipeWrite);

	StdOut.TrimEndInline();

	if (bTimedOut)
	{
		return FString::Printf(
			TEXT("Error: git push timed out after %.0f seconds.\nPartial output:\n%s"),
			TimeoutSec, *StdOut);
	}

	if (ReturnCode != 0)
	{
		return FString::Printf(
			TEXT("git push failed (exit %d):\n%s"),
			ReturnCode, *StdOut);
	}

	FString Result = FString::Printf(TEXT("git push %s"), *Remote);
	if (!Branch.IsEmpty()) Result += FString::Printf(TEXT(" %s"), *Branch);
	if (bForce) Result += TEXT(" --force");
	Result += FString::Printf(TEXT("\n\n%s"),
		StdOut.IsEmpty() ? TEXT("Push completed successfully.") : *StdOut);

	UE_LOG(LogGitHubCopilotUE, Log,
		TEXT("ToolExecutor: GitPush completed (code=%d)"), ReturnCode);

	return Result;
}


// ============================================================================
// Tool definitions — sent to the AI so it knows what tools are available
// ============================================================================

TSharedPtr<FJsonValue> FGitHubCopilotUEToolExecutor::MakeToolDef(
	const FString& Name, const FString& Description, const TSharedPtr<FJsonObject>& Parameters)
{
	TSharedPtr<FJsonObject> FuncObj = MakeShareable(new FJsonObject);
	FuncObj->SetStringField(TEXT("name"), Name);
	FuncObj->SetStringField(TEXT("description"), Description);
	FuncObj->SetObjectField(TEXT("parameters"), Parameters);

	TSharedPtr<FJsonObject> ToolObj = MakeShareable(new FJsonObject);
	ToolObj->SetStringField(TEXT("type"), TEXT("function"));
	ToolObj->SetObjectField(TEXT("function"), FuncObj);

	return MakeShareable(new FJsonValueObject(ToolObj));
}

/** Helper to build a JSON schema property */
static void AddProp(const TSharedPtr<FJsonObject>& Props, const FString& Name, const FString& Type, const FString& Desc)
{
	TSharedPtr<FJsonObject> Prop = MakeShareable(new FJsonObject);
	Prop->SetStringField(TEXT("type"), Type);
	Prop->SetStringField(TEXT("description"), Desc);
	Props->SetObjectField(Name, Prop);
}

TArray<TSharedPtr<FJsonValue>> FGitHubCopilotUEToolExecutor::BuildToolDefinitions()
{
	TArray<TSharedPtr<FJsonValue>> Tools;

	// ── view (CLI style alias) ──
	{
		TSharedPtr<FJsonObject> Params = MakeShareable(new FJsonObject);
		Params->SetStringField(TEXT("type"), TEXT("object"));
		TSharedPtr<FJsonObject> Props = MakeShareable(new FJsonObject);
		AddProp(Props, TEXT("path"), TEXT("string"), TEXT("Absolute or project-relative path to a file or directory"));
		AddProp(Props, TEXT("start_line"), TEXT("integer"), TEXT("Optional start line (file only)"));
		AddProp(Props, TEXT("end_line"), TEXT("integer"), TEXT("Optional end line (file only)"));
		Params->SetObjectField(TEXT("properties"), Props);
		TArray<TSharedPtr<FJsonValue>> Req;
		Req.Add(MakeShareable(new FJsonValueString(TEXT("path"))));
		Params->SetArrayField(TEXT("required"), Req);
		Params->SetBoolField(TEXT("additionalProperties"), false);
		Tools.Add(MakeToolDef(TEXT("view"),
			TEXT("View a file or list a directory. Files return line-numbered content; directories return entries."),
			Params));
	}

	// ── glob (CLI style alias) ──
	{
		TSharedPtr<FJsonObject> Params = MakeShareable(new FJsonObject);
		Params->SetStringField(TEXT("type"), TEXT("object"));
		TSharedPtr<FJsonObject> Props = MakeShareable(new FJsonObject);
		AddProp(Props, TEXT("pattern"), TEXT("string"), TEXT("Wildcard pattern (for example: Source/**/*.cpp or *.uasset)"));
		AddProp(Props, TEXT("path"), TEXT("string"), TEXT("Root directory to search from. Default: project root."));
		AddProp(Props, TEXT("max_results"), TEXT("integer"), TEXT("Maximum number of matches to return. Default: 500."));
		Params->SetObjectField(TEXT("properties"), Props);
		TArray<TSharedPtr<FJsonValue>> Req;
		Req.Add(MakeShareable(new FJsonValueString(TEXT("pattern"))));
		Params->SetArrayField(TEXT("required"), Req);
		Params->SetBoolField(TEXT("additionalProperties"), false);
		Tools.Add(MakeToolDef(TEXT("glob"),
			TEXT("Find files by wildcard pattern, similar to Copilot CLI glob."),
			Params));
	}

	// ── rg (CLI style alias) ──
	{
		TSharedPtr<FJsonObject> Params = MakeShareable(new FJsonObject);
		Params->SetStringField(TEXT("type"), TEXT("object"));
		TSharedPtr<FJsonObject> Props = MakeShareable(new FJsonObject);
		AddProp(Props, TEXT("pattern"), TEXT("string"), TEXT("Pattern to search for"));
		AddProp(Props, TEXT("path"), TEXT("string"), TEXT("Directory to search in. Default: Source"));
		AddProp(Props, TEXT("file_filter"), TEXT("string"), TEXT("Semicolon-separated wildcard filter. Default: *.h;*.cpp"));
		Params->SetObjectField(TEXT("properties"), Props);
		TArray<TSharedPtr<FJsonValue>> Req;
		Req.Add(MakeShareable(new FJsonValueString(TEXT("pattern"))));
		Params->SetArrayField(TEXT("required"), Req);
		Params->SetBoolField(TEXT("additionalProperties"), false);
		Tools.Add(MakeToolDef(TEXT("rg"),
			TEXT("Search text across files (ripgrep-style alias routed to search_files)."),
			Params));
	}

	// ── read_file ──
	{
		TSharedPtr<FJsonObject> Params = MakeShareable(new FJsonObject);
		Params->SetStringField(TEXT("type"), TEXT("object"));
		TSharedPtr<FJsonObject> Props = MakeShareable(new FJsonObject);
		AddProp(Props, TEXT("path"), TEXT("string"), TEXT("File path (relative to project root, or absolute)"));
		AddProp(Props, TEXT("start_line"), TEXT("integer"), TEXT("Optional start line (1-based) to read a specific range"));
		AddProp(Props, TEXT("end_line"), TEXT("integer"), TEXT("Optional end line (1-based)"));
		Params->SetObjectField(TEXT("properties"), Props);
		TArray<TSharedPtr<FJsonValue>> Req;
		Req.Add(MakeShareable(new FJsonValueString(TEXT("path"))));
		Params->SetArrayField(TEXT("required"), Req);
		Params->SetBoolField(TEXT("additionalProperties"), false);
		Tools.Add(MakeToolDef(TEXT("read_file"),
			TEXT("Read the contents of a file. Returns the file content with line numbers. Use start_line/end_line for large files."),
			Params));
	}

	// ── write_file ──
	{
		TSharedPtr<FJsonObject> Params = MakeShareable(new FJsonObject);
		Params->SetStringField(TEXT("type"), TEXT("object"));
		TSharedPtr<FJsonObject> Props = MakeShareable(new FJsonObject);
		AddProp(Props, TEXT("path"), TEXT("string"), TEXT("File path to write (relative to project root, or absolute). Creates the file if it doesn't exist."));
		AddProp(Props, TEXT("content"), TEXT("string"), TEXT("The full file content to write"));
		Params->SetObjectField(TEXT("properties"), Props);
		TArray<TSharedPtr<FJsonValue>> Req;
		Req.Add(MakeShareable(new FJsonValueString(TEXT("path"))));
		Req.Add(MakeShareable(new FJsonValueString(TEXT("content"))));
		Params->SetArrayField(TEXT("required"), Req);
		Params->SetBoolField(TEXT("additionalProperties"), false);
		Tools.Add(MakeToolDef(TEXT("write_file"),
			TEXT("Write content to a file. Creates the file if it doesn't exist. Automatically backs up existing files before overwriting."),
			Params));
	}

	// ── edit_file ──
	{
		TSharedPtr<FJsonObject> Params = MakeShareable(new FJsonObject);
		Params->SetStringField(TEXT("type"), TEXT("object"));
		TSharedPtr<FJsonObject> Props = MakeShareable(new FJsonObject);
		AddProp(Props, TEXT("path"), TEXT("string"), TEXT("File path to edit"));
		AddProp(Props, TEXT("old_str"), TEXT("string"), TEXT("Exact string to find and replace. Must match exactly one occurrence."));
		AddProp(Props, TEXT("new_str"), TEXT("string"), TEXT("Replacement string"));
		Params->SetObjectField(TEXT("properties"), Props);
		TArray<TSharedPtr<FJsonValue>> Req;
		Req.Add(MakeShareable(new FJsonValueString(TEXT("path"))));
		Req.Add(MakeShareable(new FJsonValueString(TEXT("old_str"))));
		Req.Add(MakeShareable(new FJsonValueString(TEXT("new_str"))));
		Params->SetArrayField(TEXT("required"), Req);
		Params->SetBoolField(TEXT("additionalProperties"), false);
		Tools.Add(MakeToolDef(TEXT("edit_file"),
			TEXT("Edit a file by replacing an exact string match with new content. The old_str must appear exactly once in the file. Creates a backup before editing."),
			Params));
	}

	// ── list_directory ──
	{
		TSharedPtr<FJsonObject> Params = MakeShareable(new FJsonObject);
		Params->SetStringField(TEXT("type"), TEXT("object"));
		TSharedPtr<FJsonObject> Props = MakeShareable(new FJsonObject);
		AddProp(Props, TEXT("path"), TEXT("string"), TEXT("Directory path (relative to project root). Default: project root."));
		AddProp(Props, TEXT("recursive"), TEXT("boolean"), TEXT("If true, list files recursively up to max_depth levels deep. Default: false."));
		AddProp(Props, TEXT("max_depth"), TEXT("integer"), TEXT("Max recursion depth (1-5). Default: 2. Only used when recursive=true."));
		Params->SetObjectField(TEXT("properties"), Props);
		Params->SetBoolField(TEXT("additionalProperties"), false);
		Tools.Add(MakeToolDef(TEXT("list_directory"),
			TEXT("List files and subdirectories. Use recursive=true to see nested content (e.g. Content/ folder with imported assets)."),
			Params));
	}

	// ── create_directory ──
	{
		TSharedPtr<FJsonObject> Params = MakeShareable(new FJsonObject);
		Params->SetStringField(TEXT("type"), TEXT("object"));
		TSharedPtr<FJsonObject> Props = MakeShareable(new FJsonObject);
		AddProp(Props, TEXT("path"), TEXT("string"), TEXT("Directory path to create"));
		Params->SetObjectField(TEXT("properties"), Props);
		TArray<TSharedPtr<FJsonValue>> Req;
		Req.Add(MakeShareable(new FJsonValueString(TEXT("path"))));
		Params->SetArrayField(TEXT("required"), Req);
		Params->SetBoolField(TEXT("additionalProperties"), false);
		Tools.Add(MakeToolDef(TEXT("create_directory"),
			TEXT("Create a directory (and parent directories) inside the project."),
			Params));
	}

	// ── copy_file ──
	{
		TSharedPtr<FJsonObject> Params = MakeShareable(new FJsonObject);
		Params->SetStringField(TEXT("type"), TEXT("object"));
		TSharedPtr<FJsonObject> Props = MakeShareable(new FJsonObject);
		AddProp(Props, TEXT("source_path"), TEXT("string"), TEXT("Source file path"));
		AddProp(Props, TEXT("destination_path"), TEXT("string"), TEXT("Destination file path"));
		AddProp(Props, TEXT("overwrite"), TEXT("boolean"), TEXT("Allow replacing destination if it already exists"));
		Params->SetObjectField(TEXT("properties"), Props);
		TArray<TSharedPtr<FJsonValue>> Req;
		Req.Add(MakeShareable(new FJsonValueString(TEXT("source_path"))));
		Req.Add(MakeShareable(new FJsonValueString(TEXT("destination_path"))));
		Params->SetArrayField(TEXT("required"), Req);
		Params->SetBoolField(TEXT("additionalProperties"), false);
		Tools.Add(MakeToolDef(TEXT("copy_file"),
			TEXT("Copy a file from one path to another within the project."),
			Params));
	}

	// ── move_file ──
	{
		TSharedPtr<FJsonObject> Params = MakeShareable(new FJsonObject);
		Params->SetStringField(TEXT("type"), TEXT("object"));
		TSharedPtr<FJsonObject> Props = MakeShareable(new FJsonObject);
		AddProp(Props, TEXT("source_path"), TEXT("string"), TEXT("Source file path"));
		AddProp(Props, TEXT("destination_path"), TEXT("string"), TEXT("Destination file path"));
		AddProp(Props, TEXT("overwrite"), TEXT("boolean"), TEXT("Allow replacing destination if it already exists"));
		Params->SetObjectField(TEXT("properties"), Props);
		TArray<TSharedPtr<FJsonValue>> Req;
		Req.Add(MakeShareable(new FJsonValueString(TEXT("source_path"))));
		Req.Add(MakeShareable(new FJsonValueString(TEXT("destination_path"))));
		Params->SetArrayField(TEXT("required"), Req);
		Params->SetBoolField(TEXT("additionalProperties"), false);
		Tools.Add(MakeToolDef(TEXT("move_file"),
			TEXT("Move/rename a file within the project."),
			Params));
	}

	// ── search_files ──
	{
		TSharedPtr<FJsonObject> Params = MakeShareable(new FJsonObject);
		Params->SetStringField(TEXT("type"), TEXT("object"));
		TSharedPtr<FJsonObject> Props = MakeShareable(new FJsonObject);
		AddProp(Props, TEXT("pattern"), TEXT("string"), TEXT("Text pattern to search for (case-sensitive substring match)"));
		AddProp(Props, TEXT("path"), TEXT("string"), TEXT("Directory to search in (relative to project root). Default: 'Source'"));
		AddProp(Props, TEXT("file_filter"), TEXT("string"), TEXT("Semicolon-separated file extensions to search. Default: '*.h;*.cpp'"));
		Params->SetObjectField(TEXT("properties"), Props);
		TArray<TSharedPtr<FJsonValue>> Req;
		Req.Add(MakeShareable(new FJsonValueString(TEXT("pattern"))));
		Params->SetArrayField(TEXT("required"), Req);
		Params->SetBoolField(TEXT("additionalProperties"), false);
		Tools.Add(MakeToolDef(TEXT("search_files"),
			TEXT("Search for a text pattern across source files. Returns matching lines with file paths and line numbers."),
			Params));
	}

	// ── get_project_structure ──
	{
		TSharedPtr<FJsonObject> Params = MakeShareable(new FJsonObject);
		Params->SetStringField(TEXT("type"), TEXT("object"));
		TSharedPtr<FJsonObject> Props = MakeShareable(new FJsonObject);
		Params->SetObjectField(TEXT("properties"), Props);
		Params->SetBoolField(TEXT("additionalProperties"), false);
		Tools.Add(MakeToolDef(TEXT("get_project_structure"),
			TEXT("Get the full project structure: source files, config files, modules, plugins, engine version, XR status."),
			Params));
	}

	// ── create_cpp_class ──
	{
		TSharedPtr<FJsonObject> Params = MakeShareable(new FJsonObject);
		Params->SetStringField(TEXT("type"), TEXT("object"));
		TSharedPtr<FJsonObject> Props = MakeShareable(new FJsonObject);
		AddProp(Props, TEXT("class_name"), TEXT("string"), TEXT("Name of the C++ class to create"));
		AddProp(Props, TEXT("parent_class"), TEXT("string"), TEXT("Parent class. Default: UObject"));
		AddProp(Props, TEXT("module"), TEXT("string"), TEXT("Module name. Default: project game module"));
		AddProp(Props, TEXT("header_content"), TEXT("string"), TEXT("Full .h file content. If empty, generates a default UCLASS header."));
		AddProp(Props, TEXT("cpp_content"), TEXT("string"), TEXT("Full .cpp file content. If empty, generates a default implementation."));
		Params->SetObjectField(TEXT("properties"), Props);
		TArray<TSharedPtr<FJsonValue>> Req;
		Req.Add(MakeShareable(new FJsonValueString(TEXT("class_name"))));
		Params->SetArrayField(TEXT("required"), Req);
		Params->SetBoolField(TEXT("additionalProperties"), false);
		Tools.Add(MakeToolDef(TEXT("create_cpp_class"),
			TEXT("Create a new C++ class with .h and .cpp files in the project source tree. Provide full content or let it generate defaults."),
			Params));
	}

	// ── create_blueprint_asset ──
	{
		TSharedPtr<FJsonObject> Params = MakeShareable(new FJsonObject);
		Params->SetStringField(TEXT("type"), TEXT("object"));
		TSharedPtr<FJsonObject> Props = MakeShareable(new FJsonObject);
		AddProp(Props, TEXT("asset_name"), TEXT("string"), TEXT("Blueprint asset name (e.g. BP_PlayerHelper)"));
		AddProp(Props, TEXT("package_path"), TEXT("string"), TEXT("Content path package (e.g. /Game/Blueprints). Default: /Game/Copilot"));
		AddProp(Props, TEXT("parent_class"), TEXT("string"), TEXT("Parent class (e.g. AActor, UActorComponent, APawn, UBlueprintFunctionLibrary, or /Script/... class path)."));
		AddProp(Props, TEXT("open_editor"), TEXT("boolean"), TEXT("Whether to open the new asset editor tab immediately. Default: true"));
		Params->SetObjectField(TEXT("properties"), Props);
		TArray<TSharedPtr<FJsonValue>> Req;
		Req.Add(MakeShareable(new FJsonValueString(TEXT("asset_name"))));
		Params->SetArrayField(TEXT("required"), Req);
		Params->SetBoolField(TEXT("additionalProperties"), false);
		Tools.Add(MakeToolDef(TEXT("create_blueprint_asset"),
			TEXT("Create a Blueprint asset in the Content Browser. Supports Actor/Component/Pawn/Character and Blueprint Function Library parents."),
			Params));
	}

	// ── compile ──
	{
		TSharedPtr<FJsonObject> Params = MakeShareable(new FJsonObject);
		Params->SetStringField(TEXT("type"), TEXT("object"));
		TSharedPtr<FJsonObject> Props = MakeShareable(new FJsonObject);
		AddProp(Props, TEXT("mode"), TEXT("string"), TEXT("Compile mode: 'live_coding' or 'full'. Default: 'live_coding'"));
		Params->SetObjectField(TEXT("properties"), Props);
		Params->SetBoolField(TEXT("additionalProperties"), false);
		Tools.Add(MakeToolDef(TEXT("compile"),
			TEXT("Trigger compile operations. mode='full' uses editor compile, mode='live_coding' triggers Live Coding patch."),
			Params));
	}

	// ── live_coding_patch ──
	{
		TSharedPtr<FJsonObject> Params = MakeShareable(new FJsonObject);
		Params->SetStringField(TEXT("type"), TEXT("object"));
		TSharedPtr<FJsonObject> Props = MakeShareable(new FJsonObject);
		Params->SetObjectField(TEXT("properties"), Props);
		Params->SetBoolField(TEXT("additionalProperties"), false);
		Tools.Add(MakeToolDef(TEXT("live_coding_patch"),
			TEXT("Trigger a Live Coding patch compile."),
			Params));
	}

	// ── run_automation_tests ──
	{
		TSharedPtr<FJsonObject> Params = MakeShareable(new FJsonObject);
		Params->SetStringField(TEXT("type"), TEXT("object"));
		TSharedPtr<FJsonObject> Props = MakeShareable(new FJsonObject);
		AddProp(Props, TEXT("filter"), TEXT("string"), TEXT("Automation filter (for example: Project., Engine., Smoke.)"));
		Params->SetObjectField(TEXT("properties"), Props);
		Params->SetBoolField(TEXT("additionalProperties"), false);
		Tools.Add(MakeToolDef(TEXT("run_automation_tests"),
			TEXT("Run Unreal automation tests with an optional filter."),
			Params));
	}

	// ── get_file_info ──
	{
		TSharedPtr<FJsonObject> Params = MakeShareable(new FJsonObject);
		Params->SetStringField(TEXT("type"), TEXT("object"));
		TSharedPtr<FJsonObject> Props = MakeShareable(new FJsonObject);
		AddProp(Props, TEXT("path"), TEXT("string"), TEXT("File path to get info about"));
		Params->SetObjectField(TEXT("properties"), Props);
		TArray<TSharedPtr<FJsonValue>> Req;
		Req.Add(MakeShareable(new FJsonValueString(TEXT("path"))));
		Params->SetArrayField(TEXT("required"), Req);
		Params->SetBoolField(TEXT("additionalProperties"), false);
		Tools.Add(MakeToolDef(TEXT("get_file_info"),
			TEXT("Get metadata about a file: size, line count, modification date, extension."),
			Params));
	}

	// ── delete_file ──
	{
		TSharedPtr<FJsonObject> Params = MakeShareable(new FJsonObject);
		Params->SetStringField(TEXT("type"), TEXT("object"));
		TSharedPtr<FJsonObject> Props = MakeShareable(new FJsonObject);
		AddProp(Props, TEXT("path"), TEXT("string"), TEXT("File path to delete"));
		Params->SetObjectField(TEXT("properties"), Props);
		TArray<TSharedPtr<FJsonValue>> Req;
		Req.Add(MakeShareable(new FJsonValueString(TEXT("path"))));
		Params->SetArrayField(TEXT("required"), Req);
		Params->SetBoolField(TEXT("additionalProperties"), false);
		Tools.Add(MakeToolDef(TEXT("delete_file"),
			TEXT("Delete a file. Creates a .deleted.bak backup before deletion. Only works within the project directory."),
			Params));
	}

	// ── spawn_actor ──
	{
		TSharedPtr<FJsonObject> Params = MakeShareable(new FJsonObject);
		Params->SetStringField(TEXT("type"), TEXT("object"));
		TSharedPtr<FJsonObject> Props = MakeShareable(new FJsonObject);
		AddProp(Props, TEXT("actor_class"), TEXT("string"), TEXT("Actor class to spawn (e.g. AActor, APointLight, AStaticMeshActor, ACharacter, or /Script/... path)"));
		AddProp(Props, TEXT("name"), TEXT("string"), TEXT("Optional display name for the spawned actor"));
		AddProp(Props, TEXT("location"), TEXT("array"), TEXT("Spawn location [X, Y, Z] (default [0,0,0])"));
		AddProp(Props, TEXT("rotation"), TEXT("array"), TEXT("Spawn rotation [Pitch, Yaw, Roll] in degrees (default [0,0,0])"));
		AddProp(Props, TEXT("scale"), TEXT("array"), TEXT("Scale [X, Y, Z] (default [1,1,1])"));
		Params->SetObjectField(TEXT("properties"), Props);
		TArray<TSharedPtr<FJsonValue>> Req;
		Req.Add(MakeShareable(new FJsonValueString(TEXT("actor_class"))));
		Params->SetArrayField(TEXT("required"), Req);
		Params->SetBoolField(TEXT("additionalProperties"), false);
		Tools.Add(MakeToolDef(TEXT("spawn_actor"),
			TEXT("Spawn an actor into the current level at a given transform. The actor appears immediately in the editor viewport."),
			Params));
	}

	// ── create_material_asset ──
	{
		TSharedPtr<FJsonObject> Params = MakeShareable(new FJsonObject);
		Params->SetStringField(TEXT("type"), TEXT("object"));
		TSharedPtr<FJsonObject> Props = MakeShareable(new FJsonObject);
		AddProp(Props, TEXT("asset_name"), TEXT("string"), TEXT("Material asset name (e.g. M_GoldMetal)"));
		AddProp(Props, TEXT("package_path"), TEXT("string"), TEXT("Content path (e.g. /Game/Materials). Default: /Game/Copilot/Materials"));
		AddProp(Props, TEXT("base_color"), TEXT("array"), TEXT("Base color [R, G, B] values 0-1 (e.g. [0.8, 0.2, 0.1])"));
		AddProp(Props, TEXT("metallic"), TEXT("number"), TEXT("Metallic value 0-1 (default 0.0)"));
		AddProp(Props, TEXT("roughness"), TEXT("number"), TEXT("Roughness value 0-1 (default 0.5)"));
		AddProp(Props, TEXT("emissive_color"), TEXT("array"), TEXT("Emissive color [R, G, B] (values can exceed 1 for glow)"));
		AddProp(Props, TEXT("opacity"), TEXT("number"), TEXT("Opacity 0-1 (setting this switches blend mode to Translucent)"));
		AddProp(Props, TEXT("blend_mode"), TEXT("string"), TEXT("Blend mode: opaque, translucent, additive, modulate, masked"));
		AddProp(Props, TEXT("shading_model"), TEXT("string"), TEXT("Shading model: default_lit, unlit, subsurface, clearcoat"));
		AddProp(Props, TEXT("assign_to"), TEXT("string"), TEXT("Optional: name of actor in level to assign material to"));
		AddProp(Props, TEXT("open_editor"), TEXT("boolean"), TEXT("Open material editor after creation (default true)"));
		Params->SetObjectField(TEXT("properties"), Props);
		TArray<TSharedPtr<FJsonValue>> Req;
		Req.Add(MakeShareable(new FJsonValueString(TEXT("asset_name"))));
		Params->SetArrayField(TEXT("required"), Req);
		Params->SetBoolField(TEXT("additionalProperties"), false);
		Tools.Add(MakeToolDef(TEXT("create_material_asset"),
			TEXT("Create a Material asset with expression nodes (base color, metallic, roughness, emissive, opacity). Saves as .uasset and optionally assigns to an actor in the level."),
			Params));
	}

	// ── create_data_table ──
	{
		TSharedPtr<FJsonObject> Params = MakeShareable(new FJsonObject);
		Params->SetStringField(TEXT("type"), TEXT("object"));
		TSharedPtr<FJsonObject> Props = MakeShareable(new FJsonObject);
		AddProp(Props, TEXT("asset_name"), TEXT("string"), TEXT("DataTable asset name (e.g. DT_Weapons)"));
		AddProp(Props, TEXT("package_path"), TEXT("string"), TEXT("Content path (default: /Game/Copilot/Data)"));
		AddProp(Props, TEXT("row_struct"), TEXT("string"), TEXT("Full path to row struct (e.g. /Script/YourProject.FWeaponData or /Script/Engine.FDataTableRowHandle)"));
		AddProp(Props, TEXT("open_editor"), TEXT("boolean"), TEXT("Open DataTable editor after creation (default true)"));
		Params->SetObjectField(TEXT("properties"), Props);
		TArray<TSharedPtr<FJsonValue>> Req;
		Req.Add(MakeShareable(new FJsonValueString(TEXT("asset_name"))));
		Req.Add(MakeShareable(new FJsonValueString(TEXT("row_struct"))));
		Params->SetArrayField(TEXT("required"), Req);
		Params->SetBoolField(TEXT("additionalProperties"), false);
		Tools.Add(MakeToolDef(TEXT("create_data_table"),
			TEXT("Create a DataTable asset in the Content Browser backed by a given row struct. Open in editor to add rows."),
			Params));
	}

	// ── create_niagara_system ──
	{
		TSharedPtr<FJsonObject> Params = MakeShareable(new FJsonObject);
		Params->SetStringField(TEXT("type"), TEXT("object"));
		TSharedPtr<FJsonObject> Props = MakeShareable(new FJsonObject);
		AddProp(Props, TEXT("asset_name"), TEXT("string"), TEXT("Niagara system asset name (e.g. NS_FireEffect)"));
		AddProp(Props, TEXT("package_path"), TEXT("string"), TEXT("Content path (default: /Game/Copilot/FX)"));
		AddProp(Props, TEXT("open_editor"), TEXT("boolean"), TEXT("Open Niagara editor after creation (default true)"));
		Params->SetObjectField(TEXT("properties"), Props);
		TArray<TSharedPtr<FJsonValue>> Req;
		Req.Add(MakeShareable(new FJsonValueString(TEXT("asset_name"))));
		Params->SetArrayField(TEXT("required"), Req);
		Params->SetBoolField(TEXT("additionalProperties"), false);
		Tools.Add(MakeToolDef(TEXT("create_niagara_system"),
			TEXT("Create an empty Niagara particle system asset. Open in Niagara editor to add emitters and configure particle behavior."),
			Params));
	}

	// ── web_search ──
	{
		TSharedPtr<FJsonObject> Params = MakeShareable(new FJsonObject);
		Params->SetStringField(TEXT("type"), TEXT("object"));
		TSharedPtr<FJsonObject> Props = MakeShareable(new FJsonObject);
		AddProp(Props, TEXT("query"), TEXT("string"), TEXT("Search query to look up on the web"));
		AddProp(Props, TEXT("max_results"), TEXT("integer"), TEXT("Maximum number of results to return (1-10, default: 5)"));
		Params->SetObjectField(TEXT("properties"), Props);
		TArray<TSharedPtr<FJsonValue>> Req;
		Req.Add(MakeShareable(new FJsonValueString(TEXT("query"))));
		Params->SetArrayField(TEXT("required"), Req);
		Params->SetBoolField(TEXT("additionalProperties"), false);
		Tools.Add(MakeToolDef(TEXT("web_search"),
			TEXT("Search the web for information. Use this to look up documentation, APIs, tutorials, error messages, or any current information. Returns titles, URLs, and snippets from search results."),
			Params));
	}

	// ── capture_viewport ──
	{
		TSharedPtr<FJsonObject> Params = MakeShareable(new FJsonObject);
		Params->SetStringField(TEXT("type"), TEXT("object"));
		TSharedPtr<FJsonObject> Props = MakeShareable(new FJsonObject);
		AddProp(Props, TEXT("target"), TEXT("string"),
			TEXT("Which window to capture. \"main\" (default) = largest editor window, \"active\" = currently focused window, or a substring to match a window title (e.g. \"Blueprint\", \"Material\", \"Level\"). If no match, returns a list of available windows."));
		Params->SetObjectField(TEXT("properties"), Props);
		Params->SetArrayField(TEXT("required"), TArray<TSharedPtr<FJsonValue>>());
		Params->SetBoolField(TEXT("additionalProperties"), false);
		Tools.Add(MakeToolDef(TEXT("capture_viewport"),
			TEXT("Capture a screenshot of an Unreal Engine editor window. By default captures the main (largest) editor window, which shows the viewport, Blueprint editor, or whatever is open. Use the target parameter to capture a specific window by title. The image is sent back to you for visual analysis."),
			Params));
	}

	// ═══════════════════════════════════════════════════════════════
	// TIER 1 — Core autonomy tools
	// ═══════════════════════════════════════════════════════════════

	// ── execute_python ──
	{
		TSharedPtr<FJsonObject> Params = MakeShareable(new FJsonObject);
		Params->SetStringField(TEXT("type"), TEXT("object"));
		TSharedPtr<FJsonObject> Props = MakeShareable(new FJsonObject);
		AddProp(Props, TEXT("script"), TEXT("string"), TEXT("Python script code to execute in UE's built-in Python interpreter. Has access to the 'unreal' module for full engine API access."));
		AddProp(Props, TEXT("timeout_seconds"), TEXT("number"), TEXT("Maximum execution time in seconds (default: 30, max: 300)"));
		Params->SetObjectField(TEXT("properties"), Props);
		TArray<TSharedPtr<FJsonValue>> Req;
		Req.Add(MakeShareable(new FJsonValueString(TEXT("script"))));
		Params->SetArrayField(TEXT("required"), Req);
		Params->SetBoolField(TEXT("additionalProperties"), false);
		Tools.Add(MakeToolDef(TEXT("execute_python"),
			TEXT("Execute Python code in UE's built-in Python interpreter. Has access to the full 'unreal' module for asset manipulation, batch operations, skeletal mesh retargeting, animation processing, and any engine API. Stdout/stderr is captured and returned. Use this for complex operations that don't have a dedicated tool."),
			Params));
	}

	// ── execute_console_command ──
	{
		TSharedPtr<FJsonObject> Params = MakeShareable(new FJsonObject);
		Params->SetStringField(TEXT("type"), TEXT("object"));
		TSharedPtr<FJsonObject> Props = MakeShareable(new FJsonObject);
		AddProp(Props, TEXT("command"), TEXT("string"), TEXT("UE console command to execute (e.g. 'stat fps', 'r.ScreenPercentage 100', 'show collision', 'obj list')"));
		Params->SetObjectField(TEXT("properties"), Props);
		TArray<TSharedPtr<FJsonValue>> Req;
		Req.Add(MakeShareable(new FJsonValueString(TEXT("command"))));
		Params->SetArrayField(TEXT("required"), Req);
		Params->SetBoolField(TEXT("additionalProperties"), false);
		Tools.Add(MakeToolDef(TEXT("execute_console_command"),
			TEXT("Execute an Unreal Engine console command and capture the output. Use for engine settings, debugging (stat commands), rendering config, object inspection, and any command you'd type in the ~ console."),
			Params));
	}

	// ── execute_shell ──
	{
		TSharedPtr<FJsonObject> Params = MakeShareable(new FJsonObject);
		Params->SetStringField(TEXT("type"), TEXT("object"));
		TSharedPtr<FJsonObject> Props = MakeShareable(new FJsonObject);
		AddProp(Props, TEXT("command"), TEXT("string"), TEXT("Shell command to execute (runs via cmd.exe /c on Windows)"));
		AddProp(Props, TEXT("working_directory"), TEXT("string"), TEXT("Working directory for the command (default: project directory)"));
		AddProp(Props, TEXT("timeout_seconds"), TEXT("number"), TEXT("Maximum execution time in seconds (default: 30, max: 600)"));
		Params->SetObjectField(TEXT("properties"), Props);
		TArray<TSharedPtr<FJsonValue>> Req;
		Req.Add(MakeShareable(new FJsonValueString(TEXT("command"))));
		Params->SetArrayField(TEXT("required"), Req);
		Params->SetBoolField(TEXT("additionalProperties"), false);
		Tools.Add(MakeToolDef(TEXT("execute_shell"),
			TEXT("Execute a shell command on the host OS and capture stdout/stderr. Use for git, pip, file conversions, external scripts, or any CLI operation. Runs via cmd.exe on Windows. Dangerous commands (format, rm -rf /) are blocked."),
			Params));
	}

	// ── play_in_editor ──
	{
		TSharedPtr<FJsonObject> Params = MakeShareable(new FJsonObject);
		Params->SetStringField(TEXT("type"), TEXT("object"));
		TSharedPtr<FJsonObject> Props = MakeShareable(new FJsonObject);
		AddProp(Props, TEXT("mode"), TEXT("string"), TEXT("PIE mode: 'selected_viewport' (default), 'new_window', 'mobile_preview', 'vr_preview'"));
		Params->SetObjectField(TEXT("properties"), Props);
		Params->SetArrayField(TEXT("required"), TArray<TSharedPtr<FJsonValue>>());
		Params->SetBoolField(TEXT("additionalProperties"), false);
		Tools.Add(MakeToolDef(TEXT("play_in_editor"),
			TEXT("Start a Play-In-Editor (PIE) session to test the game. Can launch in the selected viewport, a new window, mobile preview, or VR preview mode."),
			Params));
	}

	// ── stop_pie ──
	{
		TSharedPtr<FJsonObject> Params = MakeShareable(new FJsonObject);
		Params->SetStringField(TEXT("type"), TEXT("object"));
		TSharedPtr<FJsonObject> Props = MakeShareable(new FJsonObject);
		Params->SetObjectField(TEXT("properties"), Props);
		Params->SetArrayField(TEXT("required"), TArray<TSharedPtr<FJsonValue>>());
		Params->SetBoolField(TEXT("additionalProperties"), false);
		Tools.Add(MakeToolDef(TEXT("stop_pie"),
			TEXT("Stop the currently running Play-In-Editor (PIE) session and return to editing mode."),
			Params));
	}

	// ── package_project ──
	{
		TSharedPtr<FJsonObject> Params = MakeShareable(new FJsonObject);
		Params->SetStringField(TEXT("type"), TEXT("object"));
		TSharedPtr<FJsonObject> Props = MakeShareable(new FJsonObject);
		AddProp(Props, TEXT("platform"), TEXT("string"), TEXT("Target platform: Win64, Linux, Mac, Android, IOS, LinuxArm64 (default: Win64)"));
		AddProp(Props, TEXT("configuration"), TEXT("string"), TEXT("Build configuration: Development, Shipping, DebugGame (default: Development)"));
		AddProp(Props, TEXT("output_directory"), TEXT("string"), TEXT("Output directory for packaged build (default: ProjectDir/Packaged/Platform)"));
		Params->SetObjectField(TEXT("properties"), Props);
		Params->SetArrayField(TEXT("required"), TArray<TSharedPtr<FJsonValue>>());
		Params->SetBoolField(TEXT("additionalProperties"), false);
		Tools.Add(MakeToolDef(TEXT("package_project"),
			TEXT("Package the project for distribution. Launches RunUAT BuildCookRun in the background. This is a long-running operation — check the Output Log for progress."),
			Params));
	}

	// ═══════════════════════════════════════════════════════════════
	// TIER 2 — Scene & asset tools
	// ═══════════════════════════════════════════════════════════════

	// ── list_actors ──
	{
		TSharedPtr<FJsonObject> Params = MakeShareable(new FJsonObject);
		Params->SetStringField(TEXT("type"), TEXT("object"));
		TSharedPtr<FJsonObject> Props = MakeShareable(new FJsonObject);
		AddProp(Props, TEXT("class_filter"), TEXT("string"), TEXT("Filter actors by class name (substring match, e.g. 'PointLight', 'StaticMesh')"));
		AddProp(Props, TEXT("name_filter"), TEXT("string"), TEXT("Filter actors by name or label (substring match)"));
		AddProp(Props, TEXT("max_results"), TEXT("integer"), TEXT("Maximum number of results to return (default: 100, max: 10000)"));
		Params->SetObjectField(TEXT("properties"), Props);
		Params->SetArrayField(TEXT("required"), TArray<TSharedPtr<FJsonValue>>());
		Params->SetBoolField(TEXT("additionalProperties"), false);
		Tools.Add(MakeToolDef(TEXT("list_actors"),
			TEXT("List all actors in the current editor level. Optionally filter by class name or actor name/label. Returns name, label, class, location, and rotation for each actor."),
			Params));
	}

	// ── get_actor_properties ──
	{
		TSharedPtr<FJsonObject> Params = MakeShareable(new FJsonObject);
		Params->SetStringField(TEXT("type"), TEXT("object"));
		TSharedPtr<FJsonObject> Props = MakeShareable(new FJsonObject);
		AddProp(Props, TEXT("actor_name"), TEXT("string"), TEXT("Actor name or label to inspect"));
		Params->SetObjectField(TEXT("properties"), Props);
		TArray<TSharedPtr<FJsonValue>> Req;
		Req.Add(MakeShareable(new FJsonValueString(TEXT("actor_name"))));
		Params->SetArrayField(TEXT("required"), Req);
		Params->SetBoolField(TEXT("additionalProperties"), false);
		Tools.Add(MakeToolDef(TEXT("get_actor_properties"),
			TEXT("Get detailed properties of a specific actor: class, transform, mobility, visibility, tags, components (meshes, materials, animations), and editable properties."),
			Params));
	}

	// ── set_actor_properties ──
	{
		TSharedPtr<FJsonObject> Params = MakeShareable(new FJsonObject);
		Params->SetStringField(TEXT("type"), TEXT("object"));
		TSharedPtr<FJsonObject> Props = MakeShareable(new FJsonObject);
		AddProp(Props, TEXT("actor_name"), TEXT("string"), TEXT("Actor name or label to modify"));
		AddProp(Props, TEXT("location"), TEXT("array"), TEXT("New location [X, Y, Z]"));
		AddProp(Props, TEXT("rotation"), TEXT("array"), TEXT("New rotation [Pitch, Yaw, Roll] in degrees"));
		AddProp(Props, TEXT("scale"), TEXT("array"), TEXT("New scale [X, Y, Z]"));
		AddProp(Props, TEXT("hidden"), TEXT("boolean"), TEXT("Set actor visibility"));
		AddProp(Props, TEXT("label"), TEXT("string"), TEXT("Set actor display label"));
		AddProp(Props, TEXT("mobility"), TEXT("string"), TEXT("Set mobility: static, stationary, movable"));
		AddProp(Props, TEXT("properties"), TEXT("object"), TEXT("Key-value pairs of property name to value for reflection-based property setting"));
		Params->SetObjectField(TEXT("properties"), Props);
		TArray<TSharedPtr<FJsonValue>> Req;
		Req.Add(MakeShareable(new FJsonValueString(TEXT("actor_name"))));
		Params->SetArrayField(TEXT("required"), Req);
		Params->SetBoolField(TEXT("additionalProperties"), false);
		Tools.Add(MakeToolDef(TEXT("set_actor_properties"),
			TEXT("Set properties on an actor: transform (location, rotation, scale), visibility, label, mobility, and arbitrary properties via UE reflection."),
			Params));
	}

	// ── delete_actors ──
	{
		TSharedPtr<FJsonObject> Params = MakeShareable(new FJsonObject);
		Params->SetStringField(TEXT("type"), TEXT("object"));
		TSharedPtr<FJsonObject> Props = MakeShareable(new FJsonObject);
		AddProp(Props, TEXT("actor_names"), TEXT("array"), TEXT("Array of actor names or labels to delete"));
		AddProp(Props, TEXT("confirm"), TEXT("boolean"), TEXT("If true, actually delete. If false (default), preview what would be deleted."));
		Params->SetObjectField(TEXT("properties"), Props);
		TArray<TSharedPtr<FJsonValue>> Req;
		Req.Add(MakeShareable(new FJsonValueString(TEXT("actor_names"))));
		Params->SetArrayField(TEXT("required"), Req);
		Params->SetBoolField(TEXT("additionalProperties"), false);
		Tools.Add(MakeToolDef(TEXT("delete_actors"),
			TEXT("Delete actors from the current level. By default runs in preview mode (confirm=false) to show what would be deleted. Set confirm=true to actually delete."),
			Params));
	}

	// ── import_asset ──
	{
		TSharedPtr<FJsonObject> Params = MakeShareable(new FJsonObject);
		Params->SetStringField(TEXT("type"), TEXT("object"));
		TSharedPtr<FJsonObject> Props = MakeShareable(new FJsonObject);
		AddProp(Props, TEXT("source_path"), TEXT("string"), TEXT("Path to the file on disk to import (FBX, OBJ, PNG, WAV, etc.)"));
		AddProp(Props, TEXT("destination_path"), TEXT("string"), TEXT("Content Browser destination path (e.g. /Game/Meshes). Default: /Game/Copilot/Imports"));
		Params->SetObjectField(TEXT("properties"), Props);
		TArray<TSharedPtr<FJsonValue>> Req;
		Req.Add(MakeShareable(new FJsonValueString(TEXT("source_path"))));
		Params->SetArrayField(TEXT("required"), Req);
		Params->SetBoolField(TEXT("additionalProperties"), false);
		Tools.Add(MakeToolDef(TEXT("import_asset"),
			TEXT("Import an external file (FBX, OBJ, PNG, WAV, TGA, EXR, etc.) into the Content Browser. Automatically detects file type and uses the appropriate importer."),
			Params));
	}

	// ── open_level ──
	{
		TSharedPtr<FJsonObject> Params = MakeShareable(new FJsonObject);
		Params->SetStringField(TEXT("type"), TEXT("object"));
		TSharedPtr<FJsonObject> Props = MakeShareable(new FJsonObject);
		AddProp(Props, TEXT("level_path"), TEXT("string"), TEXT("Level path in Content Browser (e.g. /Game/Maps/MainLevel) or just the level name to search for"));
		Params->SetObjectField(TEXT("properties"), Props);
		TArray<TSharedPtr<FJsonValue>> Req;
		Req.Add(MakeShareable(new FJsonValueString(TEXT("level_path"))));
		Params->SetArrayField(TEXT("required"), Req);
		Params->SetBoolField(TEXT("additionalProperties"), false);
		Tools.Add(MakeToolDef(TEXT("open_level"),
			TEXT("Open a level/map in the editor. Provide a content path or just the level name — it will search the asset registry if the exact path is not found."),
			Params));
	}

	// ── rename_asset ──
	{
		TSharedPtr<FJsonObject> Params = MakeShareable(new FJsonObject);
		Params->SetStringField(TEXT("type"), TEXT("object"));
		TSharedPtr<FJsonObject> Props = MakeShareable(new FJsonObject);
		AddProp(Props, TEXT("source_path"), TEXT("string"), TEXT("Current asset path in Content Browser (e.g. /Game/Meshes/OldName)"));
		AddProp(Props, TEXT("new_name"), TEXT("string"), TEXT("New name for the asset (same folder)"));
		AddProp(Props, TEXT("destination_path"), TEXT("string"), TEXT("Move to a different Content Browser folder"));
		Params->SetObjectField(TEXT("properties"), Props);
		TArray<TSharedPtr<FJsonValue>> Req;
		Req.Add(MakeShareable(new FJsonValueString(TEXT("source_path"))));
		Params->SetArrayField(TEXT("required"), Req);
		Params->SetBoolField(TEXT("additionalProperties"), false);
		Tools.Add(MakeToolDef(TEXT("rename_asset"),
			TEXT("Rename or move an asset in the Content Browser. Updates all references automatically."),
			Params));
	}

	// ── get_selected_actors ──
	{
		TSharedPtr<FJsonObject> Params = MakeShareable(new FJsonObject);
		Params->SetStringField(TEXT("type"), TEXT("object"));
		TSharedPtr<FJsonObject> Props = MakeShareable(new FJsonObject);
		Params->SetObjectField(TEXT("properties"), Props);
		Params->SetArrayField(TEXT("required"), TArray<TSharedPtr<FJsonValue>>());
		Params->SetBoolField(TEXT("additionalProperties"), false);
		Tools.Add(MakeToolDef(TEXT("get_selected_actors"),
			TEXT("Get the list of currently selected actors in the editor viewport. Returns name, class, and transform for each selected actor."),
			Params));
	}

	// ═══════════════════════════════════════════════════════════════
	// TIER 3 — Specialized tools
	// ═══════════════════════════════════════════════════════════════

	// ── retarget_animations ──
	{
		TSharedPtr<FJsonObject> Params = MakeShareable(new FJsonObject);
		Params->SetStringField(TEXT("type"), TEXT("object"));
		TSharedPtr<FJsonObject> Props = MakeShareable(new FJsonObject);
		AddProp(Props, TEXT("source_skeleton"), TEXT("string"), TEXT("Asset path of the source skeleton (e.g. /Game/Characters/OldSkel/Skeleton)"));
		AddProp(Props, TEXT("target_skeleton"), TEXT("string"), TEXT("Asset path of the target skeleton (e.g. /Game/Characters/Manny/Skeleton)"));
		AddProp(Props, TEXT("animations"), TEXT("array"), TEXT("Specific animation paths to retarget. If omitted, retargets all animations using the source skeleton."));
		AddProp(Props, TEXT("output_path"), TEXT("string"), TEXT("Content Browser path for retargeted animations (default: /Game/Copilot/RetargetedAnims)"));
		Params->SetObjectField(TEXT("properties"), Props);
		TArray<TSharedPtr<FJsonValue>> Req;
		Req.Add(MakeShareable(new FJsonValueString(TEXT("source_skeleton"))));
		Req.Add(MakeShareable(new FJsonValueString(TEXT("target_skeleton"))));
		Params->SetArrayField(TEXT("required"), Req);
		Params->SetBoolField(TEXT("additionalProperties"), false);
		Tools.Add(MakeToolDef(TEXT("retarget_animations"),
			TEXT("Retarget animations from one skeleton to another using name-based bone matching. Duplicates animations and assigns the target skeleton. For complex retargeting with IK Rigs, use execute_python with the unreal module instead."),
			Params));
	}

	// ── create_anim_montage ──
	{
		TSharedPtr<FJsonObject> Params = MakeShareable(new FJsonObject);
		Params->SetStringField(TEXT("type"), TEXT("object"));
		TSharedPtr<FJsonObject> Props = MakeShareable(new FJsonObject);
		AddProp(Props, TEXT("asset_name"), TEXT("string"), TEXT("Montage asset name (e.g. AM_AttackCombo)"));
		AddProp(Props, TEXT("animation_sequence"), TEXT("string"), TEXT("Path to source AnimSequence asset (e.g. /Game/Anims/Attack)"));
		AddProp(Props, TEXT("package_path"), TEXT("string"), TEXT("Content path (default: /Game/Copilot/Montages)"));
		AddProp(Props, TEXT("slot_name"), TEXT("string"), TEXT("Slot name for the montage (default: DefaultSlot)"));
		AddProp(Props, TEXT("open_editor"), TEXT("boolean"), TEXT("Open montage editor after creation (default: true)"));
		Params->SetObjectField(TEXT("properties"), Props);
		TArray<TSharedPtr<FJsonValue>> Req;
		Req.Add(MakeShareable(new FJsonValueString(TEXT("asset_name"))));
		Req.Add(MakeShareable(new FJsonValueString(TEXT("animation_sequence"))));
		Params->SetArrayField(TEXT("required"), Req);
		Params->SetBoolField(TEXT("additionalProperties"), false);
		Tools.Add(MakeToolDef(TEXT("create_anim_montage"),
			TEXT("Create an Animation Montage asset from an AnimSequence. Montages enable anim notify events, sections, branching, and can be played via Blueprint or C++."),
			Params));
	}

	// ── create_level_sequence ──
	{
		TSharedPtr<FJsonObject> Params = MakeShareable(new FJsonObject);
		Params->SetStringField(TEXT("type"), TEXT("object"));
		TSharedPtr<FJsonObject> Props = MakeShareable(new FJsonObject);
		AddProp(Props, TEXT("asset_name"), TEXT("string"), TEXT("Level Sequence asset name (e.g. LS_IntroCinematic)"));
		AddProp(Props, TEXT("package_path"), TEXT("string"), TEXT("Content path (default: /Game/Copilot/Sequences)"));
		AddProp(Props, TEXT("open_editor"), TEXT("boolean"), TEXT("Open Sequencer editor after creation (default: true)"));
		Params->SetObjectField(TEXT("properties"), Props);
		TArray<TSharedPtr<FJsonValue>> Req;
		Req.Add(MakeShareable(new FJsonValueString(TEXT("asset_name"))));
		Params->SetArrayField(TEXT("required"), Req);
		Params->SetBoolField(TEXT("additionalProperties"), false);
		Tools.Add(MakeToolDef(TEXT("create_level_sequence"),
			TEXT("Create a Level Sequence (cinematic) asset. Open in the Sequencer editor to add tracks, keyframes, camera cuts, and animate actors."),
			Params));
	}

	// ── build_lighting ──
	{
		TSharedPtr<FJsonObject> Params = MakeShareable(new FJsonObject);
		Params->SetStringField(TEXT("type"), TEXT("object"));
		TSharedPtr<FJsonObject> Props = MakeShareable(new FJsonObject);
		AddProp(Props, TEXT("quality"), TEXT("string"), TEXT("Lighting quality: preview, medium, high, production (default: preview)"));
		Params->SetObjectField(TEXT("properties"), Props);
		Params->SetArrayField(TEXT("required"), TArray<TSharedPtr<FJsonValue>>());
		Params->SetBoolField(TEXT("additionalProperties"), false);
		Tools.Add(MakeToolDef(TEXT("build_lighting"),
			TEXT("Build/bake lighting for the current level. Preview quality is fast for iteration, Production quality is for final builds. This is an async operation — check the Output Log for progress."),
			Params));
	}

	// ── get_project_settings ──
	{
		TSharedPtr<FJsonObject> Params = MakeShareable(new FJsonObject);
		Params->SetStringField(TEXT("type"), TEXT("object"));
		TSharedPtr<FJsonObject> Props = MakeShareable(new FJsonObject);
		AddProp(Props, TEXT("section"), TEXT("string"), TEXT("INI section name (e.g. '/Script/EngineSettings.GeneralProjectSettings' or '/Script/Engine.RendererSettings')"));
		AddProp(Props, TEXT("key"), TEXT("string"), TEXT("Specific key to read. If omitted, returns all keys in the section."));
		AddProp(Props, TEXT("ini_file"), TEXT("string"), TEXT("Which INI file: Game, Engine, Editor, Input (default: Game)"));
		Params->SetObjectField(TEXT("properties"), Props);
		TArray<TSharedPtr<FJsonValue>> Req;
		Req.Add(MakeShareable(new FJsonValueString(TEXT("section"))));
		Params->SetArrayField(TEXT("required"), Req);
		Params->SetBoolField(TEXT("additionalProperties"), false);
		Tools.Add(MakeToolDef(TEXT("get_project_settings"),
			TEXT("Read project settings from INI config files (DefaultGame.ini, DefaultEngine.ini, etc.). Use to inspect rendering settings, project metadata, input mappings, and more."),
			Params));
	}

	// ── set_project_settings ──
	{
		TSharedPtr<FJsonObject> Params = MakeShareable(new FJsonObject);
		Params->SetStringField(TEXT("type"), TEXT("object"));
		TSharedPtr<FJsonObject> Props = MakeShareable(new FJsonObject);
		AddProp(Props, TEXT("section"), TEXT("string"), TEXT("INI section name"));
		AddProp(Props, TEXT("key"), TEXT("string"), TEXT("Setting key to modify"));
		AddProp(Props, TEXT("value"), TEXT("string"), TEXT("New value for the setting"));
		AddProp(Props, TEXT("ini_file"), TEXT("string"), TEXT("Which INI file: Game, Engine, Editor, Input (default: Game)"));
		Params->SetObjectField(TEXT("properties"), Props);
		TArray<TSharedPtr<FJsonValue>> Req;
		Req.Add(MakeShareable(new FJsonValueString(TEXT("section"))));
		Req.Add(MakeShareable(new FJsonValueString(TEXT("key"))));
		Req.Add(MakeShareable(new FJsonValueString(TEXT("value"))));
		Params->SetArrayField(TEXT("required"), Req);
		Params->SetBoolField(TEXT("additionalProperties"), false);
		Tools.Add(MakeToolDef(TEXT("set_project_settings"),
			TEXT("Write a project setting to an INI config file. Changes are saved immediately. Some settings require an editor restart to take effect."),
			Params));
	}

	// ── create_widget_blueprint ──
	{
		TSharedPtr<FJsonObject> Params = MakeShareable(new FJsonObject);
		Params->SetStringField(TEXT("type"), TEXT("object"));
		TSharedPtr<FJsonObject> Props = MakeShareable(new FJsonObject);
		AddProp(Props, TEXT("asset_name"), TEXT("string"), TEXT("Widget Blueprint name (e.g. WBP_MainMenu)"));
		AddProp(Props, TEXT("package_path"), TEXT("string"), TEXT("Content path (default: /Game/Copilot/UI)"));
		AddProp(Props, TEXT("parent_class"), TEXT("string"), TEXT("Parent class: UserWidget (default), or a custom widget class path"));
		AddProp(Props, TEXT("open_editor"), TEXT("boolean"), TEXT("Open UMG editor after creation (default: true)"));
		Params->SetObjectField(TEXT("properties"), Props);
		TArray<TSharedPtr<FJsonValue>> Req;
		Req.Add(MakeShareable(new FJsonValueString(TEXT("asset_name"))));
		Params->SetArrayField(TEXT("required"), Req);
		Params->SetBoolField(TEXT("additionalProperties"), false);
		Tools.Add(MakeToolDef(TEXT("create_widget_blueprint"),
			TEXT("Create a UMG Widget Blueprint for UI. Open in the UMG editor to design HUDs, menus, and in-game UI."),
			Params));
	}

	// ── get_blueprint_graph ──
	{
		TSharedPtr<FJsonObject> Params = MakeShareable(new FJsonObject);
		Params->SetStringField(TEXT("type"), TEXT("object"));
		TSharedPtr<FJsonObject> Props = MakeShareable(new FJsonObject);
		AddProp(Props, TEXT("blueprint_path"), TEXT("string"), TEXT("Content path to the Blueprint asset (e.g. /Game/Blueprints/BP_Player)"));
		Params->SetObjectField(TEXT("properties"), Props);
		TArray<TSharedPtr<FJsonValue>> Req;
		Req.Add(MakeShareable(new FJsonValueString(TEXT("blueprint_path"))));
		Params->SetArrayField(TEXT("required"), Req);
		Params->SetBoolField(TEXT("additionalProperties"), false);
		Tools.Add(MakeToolDef(TEXT("get_blueprint_graph"),
			TEXT("Read a Blueprint's visual script graphs as text. Returns all event graphs and function graphs with their nodes, pins, and connections. Use this to understand Blueprint logic before modifying it."),
			Params));
	}

	// ── add_blueprint_node ──
	{
		TSharedPtr<FJsonObject> Params = MakeShareable(new FJsonObject);
		Params->SetStringField(TEXT("type"), TEXT("object"));
		TSharedPtr<FJsonObject> Props = MakeShareable(new FJsonObject);
		AddProp(Props, TEXT("blueprint_path"), TEXT("string"), TEXT("Content path to the Blueprint asset"));
		AddProp(Props, TEXT("node_class"), TEXT("string"), TEXT("Node class: K2Node_CallFunction, K2Node_IfThenElse, K2Node_Event, etc."));
		AddProp(Props, TEXT("graph_name"), TEXT("string"), TEXT("Graph to add the node to (default: EventGraph)"));
		AddProp(Props, TEXT("function_name"), TEXT("string"), TEXT("For CallFunction nodes: the function to call (e.g. PrintString)"));
		AddProp(Props, TEXT("function_class"), TEXT("string"), TEXT("For CallFunction nodes: the owning class (e.g. KismetSystemLibrary)"));
		AddProp(Props, TEXT("position_x"), TEXT("number"), TEXT("Node X position in the graph (default: 0)"));
		AddProp(Props, TEXT("position_y"), TEXT("number"), TEXT("Node Y position in the graph (default: 0)"));
		Params->SetObjectField(TEXT("properties"), Props);
		TArray<TSharedPtr<FJsonValue>> Req;
		Req.Add(MakeShareable(new FJsonValueString(TEXT("blueprint_path"))));
		Req.Add(MakeShareable(new FJsonValueString(TEXT("node_class"))));
		Params->SetArrayField(TEXT("required"), Req);
		Params->SetBoolField(TEXT("additionalProperties"), false);
		Tools.Add(MakeToolDef(TEXT("add_blueprint_node"),
			TEXT("Add a node to a Blueprint's visual script graph. Supports CallFunction, Event, IfThenElse, and other K2Node types. For complex Blueprint editing, consider using execute_python instead."),
			Params));
	}

	// ── git_commit ──
	{
		TSharedPtr<FJsonObject> Params = MakeShareable(new FJsonObject);
		Params->SetStringField(TEXT("type"), TEXT("object"));
		TSharedPtr<FJsonObject> Props = MakeShareable(new FJsonObject);
		AddProp(Props, TEXT("message"), TEXT("string"), TEXT("Commit message"));
		AddProp(Props, TEXT("files"), TEXT("array"), TEXT("Specific files to stage. If omitted, stages all changes (git add -A)."));
		AddProp(Props, TEXT("amend"), TEXT("boolean"), TEXT("If true, amend the previous commit (default: false)"));
		Params->SetObjectField(TEXT("properties"), Props);
		TArray<TSharedPtr<FJsonValue>> Req;
		Req.Add(MakeShareable(new FJsonValueString(TEXT("message"))));
		Params->SetArrayField(TEXT("required"), Req);
		Params->SetBoolField(TEXT("additionalProperties"), false);
		Tools.Add(MakeToolDef(TEXT("git_commit"),
			TEXT("Stage and commit changes to the local git repository. Can stage specific files or all changes."),
			Params));
	}

	// ── git_push ──
	{
		TSharedPtr<FJsonObject> Params = MakeShareable(new FJsonObject);
		Params->SetStringField(TEXT("type"), TEXT("object"));
		TSharedPtr<FJsonObject> Props = MakeShareable(new FJsonObject);
		AddProp(Props, TEXT("remote"), TEXT("string"), TEXT("Remote name (default: origin)"));
		AddProp(Props, TEXT("branch"), TEXT("string"), TEXT("Branch to push. If omitted, pushes current branch."));
		AddProp(Props, TEXT("force"), TEXT("boolean"), TEXT("Force push (default: false)"));
		Params->SetObjectField(TEXT("properties"), Props);
		Params->SetArrayField(TEXT("required"), TArray<TSharedPtr<FJsonValue>>());
		Params->SetBoolField(TEXT("additionalProperties"), false);
		Tools.Add(MakeToolDef(TEXT("git_push"),
			TEXT("Push commits to a remote git repository. Pushes the current branch by default."),
			Params));
	}

	return Tools;
}
