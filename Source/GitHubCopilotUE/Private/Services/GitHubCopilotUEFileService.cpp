// Copyright GitHub, Inc. All Rights Reserved.

#include "Services/GitHubCopilotUEFileService.h"
#include "GitHubCopilotUESettings.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/DateTime.h"

FGitHubCopilotUEFileService::FGitHubCopilotUEFileService()
{
}

FString FGitHubCopilotUEFileService::GetProjectRoot() const
{
	return FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
}

FString FGitHubCopilotUEFileService::NormalizePath(const FString& InPath) const
{
	FString Result = InPath;

	// If already absolute, validate it's within the project
	if (FPaths::IsRelative(Result))
	{
		Result = FPaths::Combine(GetProjectRoot(), Result);
	}

	FPaths::NormalizeFilename(Result);
	FPaths::CollapseRelativeDirectories(Result);
	return Result;
}

bool FGitHubCopilotUEFileService::IsPathWithinAllowedRoots(const FString& FilePath) const
{
	const UGitHubCopilotUESettings* Settings = UGitHubCopilotUESettings::Get();
	if (!Settings)
	{
		UE_LOG(LogGitHubCopilotUE, Error, TEXT("FileService: Settings not available, rejecting write"));
		return false;
	}

	FString NormalizedPath = NormalizePath(FilePath);
	FString ProjectRoot = GetProjectRoot();

	// Must be within project root
	if (!NormalizedPath.StartsWith(ProjectRoot))
	{
		UE_LOG(LogGitHubCopilotUE, Warning, TEXT("FileService: Path '%s' is outside project root '%s'"), *NormalizedPath, *ProjectRoot);
		return false;
	}

	// Check against allowed write roots
	FString RelativePath = NormalizedPath;
	FPaths::MakePathRelativeTo(RelativePath, *ProjectRoot);

	for (const FString& AllowedRoot : Settings->AllowedWriteRoots)
	{
		if (RelativePath.StartsWith(AllowedRoot))
		{
			return true;
		}
	}

	UE_LOG(LogGitHubCopilotUE, Warning, TEXT("FileService: Path '%s' is not within any allowed write root"), *RelativePath);
	return false;
}

bool FGitHubCopilotUEFileService::ReadFile(const FString& FilePath, FString& OutContent) const
{
	FString NormalizedPath = NormalizePath(FilePath);

	if (!FPaths::FileExists(NormalizedPath))
	{
		UE_LOG(LogGitHubCopilotUE, Warning, TEXT("FileService: File not found: %s"), *NormalizedPath);
		return false;
	}

	if (!FFileHelper::LoadFileToString(OutContent, *NormalizedPath))
	{
		UE_LOG(LogGitHubCopilotUE, Error, TEXT("FileService: Failed to read file: %s"), *NormalizedPath);
		return false;
	}

	UE_LOG(LogGitHubCopilotUE, Verbose, TEXT("FileService: Read %d chars from %s"), OutContent.Len(), *NormalizedPath);
	return true;
}

bool FGitHubCopilotUEFileService::WriteFile(const FString& FilePath, const FString& Content, FString& OutError)
{
	FString NormalizedPath = NormalizePath(FilePath);

	if (!IsPathWithinAllowedRoots(NormalizedPath))
	{
		OutError = FString::Printf(TEXT("Path '%s' is outside allowed write roots"), *FilePath);
		UE_LOG(LogGitHubCopilotUE, Error, TEXT("FileService: %s"), *OutError);
		return false;
	}

	// Create backup before writing
	if (FPaths::FileExists(NormalizedPath))
	{
		FString BackupPath = CreateBackup(NormalizedPath);
		if (BackupPath.IsEmpty())
		{
			OutError = FString::Printf(TEXT("Failed to create backup of '%s'"), *FilePath);
			UE_LOG(LogGitHubCopilotUE, Error, TEXT("FileService: %s"), *OutError);
			return false;
		}
		UE_LOG(LogGitHubCopilotUE, Log, TEXT("FileService: Backup created at %s"), *BackupPath);
	}

	if (!FFileHelper::SaveStringToFile(Content, *NormalizedPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		OutError = FString::Printf(TEXT("Failed to write file '%s'"), *FilePath);
		UE_LOG(LogGitHubCopilotUE, Error, TEXT("FileService: %s"), *OutError);
		return false;
	}

	UE_LOG(LogGitHubCopilotUE, Log, TEXT("FileService: Wrote %d chars to %s"), Content.Len(), *NormalizedPath);
	return true;
}

bool FGitHubCopilotUEFileService::CreateNewFile(const FString& FilePath, const FString& Content, FString& OutError)
{
	FString NormalizedPath = NormalizePath(FilePath);

	if (!IsPathWithinAllowedRoots(NormalizedPath))
	{
		OutError = FString::Printf(TEXT("Path '%s' is outside allowed write roots"), *FilePath);
		return false;
	}

	if (FPaths::FileExists(NormalizedPath))
	{
		OutError = FString::Printf(TEXT("File already exists: '%s'"), *FilePath);
		return false;
	}

	// Ensure directory exists
	FString Directory = FPaths::GetPath(NormalizedPath);
	if (!FPaths::DirectoryExists(Directory))
	{
		IFileManager::Get().MakeDirectory(*Directory, true);
	}

	if (!FFileHelper::SaveStringToFile(Content, *NormalizedPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		OutError = FString::Printf(TEXT("Failed to create file '%s'"), *FilePath);
		return false;
	}

	UE_LOG(LogGitHubCopilotUE, Log, TEXT("FileService: Created new file %s (%d chars)"), *NormalizedPath, Content.Len());
	return true;
}

FString FGitHubCopilotUEFileService::CreateBackup(const FString& FilePath)
{
	FString NormalizedPath = NormalizePath(FilePath);

	if (!FPaths::FileExists(NormalizedPath))
	{
		return FString();
	}

	FString BackupDir = FPaths::ProjectSavedDir() / TEXT("CopilotBackups");
	IFileManager::Get().MakeDirectory(*BackupDir, true);

	FString FileName = FPaths::GetCleanFilename(NormalizedPath);
	FString Timestamp = FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S"));
	FString BackupPath = BackupDir / FString::Printf(TEXT("%s.%s.bak"), *FileName, *Timestamp);

	if (IFileManager::Get().Copy(*BackupPath, *NormalizedPath) == COPY_OK)
	{
		UE_LOG(LogGitHubCopilotUE, Log, TEXT("FileService: Backup created: %s"), *BackupPath);
		return BackupPath;
	}

	UE_LOG(LogGitHubCopilotUE, Error, TEXT("FileService: Failed to create backup of %s"), *NormalizedPath);
	return FString();
}

TArray<FString> FGitHubCopilotUEFileService::EnumerateSourceFiles(const FString& Directory, const FString& Extension) const
{
	TArray<FString> Result;
	FString NormalizedDir = NormalizePath(Directory);

	TArray<FString> FoundFiles;
	IFileManager::Get().FindFilesRecursive(FoundFiles, *NormalizedDir, *(TEXT("*") + Extension), true, false);

	for (const FString& File : FoundFiles)
	{
		Result.Add(File);
	}

	return Result;
}

bool FGitHubCopilotUEFileService::DeleteFile(const FString& FilePath)
{
	FString NormalizedPath = NormalizePath(FilePath);
	return IFileManager::Get().Delete(*NormalizedPath);
}

bool FGitHubCopilotUEFileService::FileExists(const FString& FilePath) const
{
	FString NormalizedPath = NormalizePath(FilePath);
	return FPaths::FileExists(NormalizedPath);
}
