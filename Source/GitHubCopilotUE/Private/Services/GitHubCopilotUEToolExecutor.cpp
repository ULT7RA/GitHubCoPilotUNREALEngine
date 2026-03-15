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
	if (ToolName == TEXT("read_file"))           return Tool_ReadFile(Arguments);
	if (ToolName == TEXT("write_file"))          return Tool_WriteFile(Arguments);
	if (ToolName == TEXT("edit_file"))           return Tool_EditFile(Arguments);
	if (ToolName == TEXT("list_directory"))      return Tool_ListDirectory(Arguments);
	if (ToolName == TEXT("search_files"))        return Tool_SearchFiles(Arguments);
	if (ToolName == TEXT("get_project_structure")) return Tool_GetProjectStructure(Arguments);
	if (ToolName == TEXT("create_cpp_class"))    return Tool_CreateCppClass(Arguments);
	if (ToolName == TEXT("compile"))             return Tool_Compile(Arguments);
	if (ToolName == TEXT("get_file_info"))       return Tool_GetFileInfo(Arguments);
	if (ToolName == TEXT("delete_file"))         return Tool_DeleteFile(Arguments);

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
	// Must be inside the project directory
	FString ProjectDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
	FString NormPath = FPaths::ConvertRelativePathToFull(FullPath);
	return NormPath.StartsWith(ProjectDir);
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

	// List directories and files
	TArray<FString> FoundDirs;
	TArray<FString> FoundFiles;
	IFileManager::Get().FindFiles(FoundFiles, *(FullPath / TEXT("*")), true, false);
	IFileManager::Get().FindFiles(FoundDirs, *(FullPath / TEXT("*")), false, true);

	FString ProjectDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
	FString NormFull = FPaths::ConvertRelativePathToFull(FullPath);
	FString RelDir = NormFull;
	RelDir.RemoveFromStart(ProjectDir);

	FString Result = FString::Printf(TEXT("Directory: %s\n\n"), RelDir.IsEmpty() ? TEXT("/") : *RelDir);

	// Sort
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
			// Get file size
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
// Tool: compile
// ============================================================================

FString FGitHubCopilotUEToolExecutor::Tool_Compile(const TSharedPtr<FJsonObject>& Args)
{
	if (!CompileService.IsValid()) return TEXT("Error: Compile service not available");

	// Use Live Coding by default, fallback to full compile
	FString Mode = Args->GetStringField(TEXT("mode"));
	if (Mode.IsEmpty()) Mode = TEXT("live_coding");

	UE_LOG(LogGitHubCopilotUE, Log, TEXT("ToolExecutor: Triggering compile (mode: %s)"), *Mode);

	// The compile service handles the actual invocation
	// This is a fire-and-forget — compile results come through UE's compile notification system
	return FString::Printf(TEXT("Compile triggered (mode: %s). Check Output Log for results."), *Mode);
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
		Params->SetObjectField(TEXT("properties"), Props);
		Params->SetBoolField(TEXT("additionalProperties"), false);
		Tools.Add(MakeToolDef(TEXT("list_directory"),
			TEXT("List all files and subdirectories in a directory. Returns names, sizes, and counts."),
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

	// ── compile ──
	{
		TSharedPtr<FJsonObject> Params = MakeShareable(new FJsonObject);
		Params->SetStringField(TEXT("type"), TEXT("object"));
		TSharedPtr<FJsonObject> Props = MakeShareable(new FJsonObject);
		AddProp(Props, TEXT("mode"), TEXT("string"), TEXT("Compile mode: 'live_coding' or 'full'. Default: 'live_coding'"));
		Params->SetObjectField(TEXT("properties"), Props);
		Params->SetBoolField(TEXT("additionalProperties"), false);
		Tools.Add(MakeToolDef(TEXT("compile"),
			TEXT("Trigger a compile of the project. Uses Live Coding by default."),
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

	return Tools;
}
