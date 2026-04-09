// Copyright GitHub, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GitHubCopilotUETypes.h"

/**
 * Service for reading, writing, creating, and backing up source files
 * within the Unreal project tree. Enforces write-root safety constraints.
 */
class GITHUBCOPILOTUE_API FGitHubCopilotUEFileService
{
public:
	FGitHubCopilotUEFileService();

	/** Read the contents of a file. Returns true on success. */
	bool ReadFile(const FString& FilePath, FString& OutContent) const;

	/** Write content to a file. Creates backup first. Returns true on success. */
	bool WriteFile(const FString& FilePath, const FString& Content, FString& OutError);

	/** Create a new file (must not already exist). Returns true on success. */
	bool CreateNewFile(const FString& FilePath, const FString& Content, FString& OutError);

	/** Create a backup of a file before mutation. Returns backup path or empty on failure. */
	FString CreateBackup(const FString& FilePath);

	/** Validate that a path is within allowed write roots. */
	bool IsPathWithinAllowedRoots(const FString& FilePath) const;

	/** Get the absolute project root directory. */
	FString GetProjectRoot() const;

	/** Enumerate source files under a directory. */
	TArray<FString> EnumerateSourceFiles(const FString& Directory, const FString& Extension = TEXT(".cpp")) const;

	/** Delete a file (used for rollback). */
	bool DeleteFile(const FString& FilePath);

	/** Check if a file exists. */
	bool FileExists(const FString& FilePath) const;

private:
	/** Normalize a relative path to absolute within the project root. */
	FString NormalizePath(const FString& InPath) const;
};
