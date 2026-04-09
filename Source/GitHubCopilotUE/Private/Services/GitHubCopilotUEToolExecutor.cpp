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

	// Delete stale capture so we can detect if the new one succeeds
	if (FPaths::FileExists(OutputPath))
	{
		IFileManager::Get().Delete(*OutputPath);
	}

	// Get the active top-level Slate window
	TSharedPtr<SWindow> ActiveWindow = FSlateApplication::Get().GetActiveTopLevelWindow();
	if (!ActiveWindow.IsValid())
	{
		const TArray<TSharedRef<SWindow>>& AllWindows = FSlateApplication::Get().GetInteractiveTopLevelWindows();
		if (AllWindows.Num() > 0)
		{
			ActiveWindow = AllWindows[0];
		}
	}

	if (ActiveWindow.IsValid())
	{
		// Use Slate's TakeScreenshot to capture the window content widget
		TArray<FColor> PixelData;
		FIntVector OutSize;
		TSharedRef<SWidget> WindowContent = ActiveWindow->GetContent();
		bool bCaptured = FSlateApplication::Get().TakeScreenshot(WindowContent, PixelData, OutSize);
		if (bCaptured && PixelData.Num() > 0 && OutSize.X > 0 && OutSize.Y > 0)
		{
			// Encode to PNG
			IImageWrapperModule& ImageWrapperModule = FModuleManager::LoadModuleChecked<IImageWrapperModule>(FName("ImageWrapper"));
			TSharedPtr<IImageWrapper> PngWrapper = ImageWrapperModule.CreateImageWrapper(EImageFormat::PNG);
			if (PngWrapper.IsValid() && PngWrapper->SetRaw(PixelData.GetData(), PixelData.Num() * sizeof(FColor), OutSize.X, OutSize.Y, ERGBFormat::BGRA, 8))
			{
				const TArray64<uint8>& PngData = PngWrapper->GetCompressed();
				if (FFileHelper::SaveArrayToFile(PngData, *OutputPath))
				{
					return FString::Printf(TEXT("__RENDER_IMAGE__:%s\nEditor window captured: %s (%dx%d)"), *OutputPath, *OutputPath, OutSize.X, OutSize.Y);
				}
			}
		}
	}

	// Fallback: use FScreenshotRequest for game viewport
	FScreenshotRequest::RequestScreenshot(OutputPath, false, false);
	FPlatformProcess::Sleep(1.5f);

	if (FPaths::FileExists(OutputPath))
	{
		return FString::Printf(TEXT("__RENDER_IMAGE__:%s\nViewport captured (game viewport): %s"), *OutputPath, *OutputPath);
	}

	return TEXT("Error: Failed to capture editor window. No active viewport available.");
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
		AddProp(Props, TEXT("resolution_x"), TEXT("integer"), TEXT("Screenshot width in pixels (default: 1280)"));
		AddProp(Props, TEXT("resolution_y"), TEXT("integer"), TEXT("Screenshot height in pixels (default: 720)"));
		Params->SetObjectField(TEXT("properties"), Props);
		Params->SetArrayField(TEXT("required"), TArray<TSharedPtr<FJsonValue>>());
		Params->SetBoolField(TEXT("additionalProperties"), false);
		Tools.Add(MakeToolDef(TEXT("capture_viewport"),
			TEXT("Capture a screenshot of the current editor viewport. The image will be sent back to you for visual analysis so you can see what the scene looks like and make informed decisions about design, layout, materials, and lighting."),
			Params));
	}

	return Tools;
}
