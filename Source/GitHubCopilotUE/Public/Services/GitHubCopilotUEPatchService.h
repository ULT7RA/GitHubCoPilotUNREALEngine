// Copyright GitHub, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GitHubCopilotUETypes.h"

class FGitHubCopilotUEFileService;

/**
 * Service for generating, previewing, validating, and applying file patches/diffs.
 * Uses a proper LCS-based unified diff algorithm. Tracks pending previews for
 * the approval flow and supports rollback on failure.
 */
class GITHUBCOPILOTUE_API FGitHubCopilotUEPatchService
{
public:
	FGitHubCopilotUEPatchService();

	/** Set the shared FileService reference (call once during initialization). */
	void SetFileService(TSharedPtr<FGitHubCopilotUEFileService> InFileService);

	/** Capture a snapshot of a file's current contents. */
	bool CaptureSnapshot(const FString& FilePath, FString& OutSnapshot);

	/** Generate a unified diff using LCS between original and proposed content. */
	FString GenerateUnifiedDiff(const FString& FilePath, const FString& OriginalContent, const FString& ProposedContent) const;

	/** Create a diff preview struct and register it as pending for approval. */
	FCopilotDiffPreview CreateDiffPreview(const FString& FilePath, const FString& OriginalContent, const FString& ProposedContent);

	/** Validate a patch before applying (path safety, content staleness, allowed roots). */
	bool ValidatePatch(const FCopilotDiffPreview& DiffPreview, FString& OutError) const;

	/**
	 * Apply a patch with full step-by-step execution:
	 * 1. Validate path and allowed roots
	 * 2. Verify file content hasn't changed since snapshot
	 * 3. Create backup
	 * 4. Write proposed content
	 * 5. Verify write succeeded
	 * 6. Rollback on any failure
	 * Returns a detailed FCopilotPatchResult.
	 */
	FCopilotPatchResult ApplyPatchWithSteps(const FCopilotDiffPreview& DiffPreview);

	/** Legacy apply (calls ApplyPatchWithSteps internally). */
	bool ApplyPatch(const FCopilotDiffPreview& DiffPreview, FString& OutError);

	/** Apply immediately without prior preview. */
	bool ApplyImmediate(const FString& FilePath, const FString& NewContent, FString& OutError);

	/** Attempt rollback using backup file. */
	bool Rollback(const FString& FilePath, FString& OutError);

	/** Insert content at a specific line in a file. */
	bool InsertAtLine(const FString& FilePath, int32 LineNumber, const FString& Content, FString& OutError);

	/** Check if there is a pending preview awaiting approval for a given file. */
	bool HasPendingPreview(const FString& FilePath) const;

	/** Get the pending preview for a file (for the approval flow). */
	bool GetPendingPreview(const FString& FilePath, FCopilotDiffPreview& OutPreview) const;

	/** Remove a pending preview (e.g., after apply or reject). */
	void ClearPendingPreview(const FString& FilePath);

	/** Get the most recently created preview (any file). */
	bool GetLastPreview(FCopilotDiffPreview& OutPreview) const;

	/** Delegate for step progress reporting */
	FOnCopilotPatchStepProgress OnPatchStepProgress;

	/** Delegate for log messages */
	mutable FOnCopilotLogMessage OnLogMessage;

private:
	/** Compute the LCS (Longest Common Subsequence) table for two line arrays. */
	TArray<TArray<int32>> ComputeLCSTable(const TArray<FString>& A, const TArray<FString>& B) const;

	/** Build unified diff hunks from an LCS table. */
	FString BuildUnifiedDiffFromLCS(const FString& FilePath, const TArray<FString>& OldLines, const TArray<FString>& NewLines, int32 ContextLines = 3) const;

	void Log(const FString& Message) const;

	/** Shared file service (set via SetFileService). */
	TSharedPtr<FGitHubCopilotUEFileService> FileService;

	/** Map of file path -> backup path for rollback. */
	TMap<FString, FString> BackupRegistry;

	/** Pending diff previews awaiting approval, keyed by normalized file path. */
	TMap<FString, FCopilotDiffPreview> PendingPreviews;

	/** The most recently created preview (any file). */
	FCopilotDiffPreview LastPreview;
};
