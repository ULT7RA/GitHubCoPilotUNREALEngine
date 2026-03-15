// Copyright GitHub, Inc. All Rights Reserved.

#include "Services/GitHubCopilotUEPatchService.h"
#include "Services/GitHubCopilotUEFileService.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

FGitHubCopilotUEPatchService::FGitHubCopilotUEPatchService()
{
}

void FGitHubCopilotUEPatchService::SetFileService(TSharedPtr<FGitHubCopilotUEFileService> InFileService)
{
	FileService = InFileService;
}

bool FGitHubCopilotUEPatchService::CaptureSnapshot(const FString& FilePath, FString& OutSnapshot)
{
	if (!FileService.IsValid())
	{
		Log(TEXT("PatchService: FileService not set"));
		return false;
	}
	return FileService->ReadFile(FilePath, OutSnapshot);
}

// ============================================================================
// LCS-based unified diff
// ============================================================================

TArray<TArray<int32>> FGitHubCopilotUEPatchService::ComputeLCSTable(const TArray<FString>& A, const TArray<FString>& B) const
{
	const int32 M = A.Num();
	const int32 N = B.Num();

	// dp[i][j] = length of LCS of A[0..i-1] and B[0..j-1]
	TArray<TArray<int32>> DP;
	DP.SetNum(M + 1);
	for (int32 i = 0; i <= M; i++)
	{
		DP[i].SetNumZeroed(N + 1);
	}

	for (int32 i = 1; i <= M; i++)
	{
		for (int32 j = 1; j <= N; j++)
		{
			if (A[i - 1] == B[j - 1])
			{
				DP[i][j] = DP[i - 1][j - 1] + 1;
			}
			else
			{
				DP[i][j] = FMath::Max(DP[i - 1][j], DP[i][j - 1]);
			}
		}
	}

	return DP;
}

FString FGitHubCopilotUEPatchService::BuildUnifiedDiffFromLCS(
	const FString& FilePath,
	const TArray<FString>& OldLines,
	const TArray<FString>& NewLines,
	int32 ContextLines) const
{
	// Backtrack through the LCS table to produce edit operations
	const TArray<TArray<int32>> DP = ComputeLCSTable(OldLines, NewLines);

	// Generate edit script: ' ' = keep, '-' = remove, '+' = add
	struct FEditOp
	{
		TCHAR Op; // ' ', '-', '+'
		FString Line;
		int32 OldIdx; // 1-based, 0 means N/A
		int32 NewIdx; // 1-based, 0 means N/A
	};

	TArray<FEditOp> EditScript;
	int32 i = OldLines.Num();
	int32 j = NewLines.Num();

	while (i > 0 || j > 0)
	{
		if (i > 0 && j > 0 && OldLines[i - 1] == NewLines[j - 1])
		{
			EditScript.Insert(FEditOp{TEXT(' '), OldLines[i - 1], i, j}, 0);
			i--; j--;
		}
		else if (j > 0 && (i == 0 || DP[i][j - 1] >= DP[i - 1][j]))
		{
			EditScript.Insert(FEditOp{TEXT('+'), NewLines[j - 1], 0, j}, 0);
			j--;
		}
		else
		{
			EditScript.Insert(FEditOp{TEXT('-'), OldLines[i - 1], i, 0}, 0);
			i--;
		}
	}

	// Build hunks: group changes with context lines
	FString Result;
	Result += FString::Printf(TEXT("--- a/%s\n"), *FPaths::GetCleanFilename(FilePath));
	Result += FString::Printf(TEXT("+++ b/%s\n"), *FPaths::GetCleanFilename(FilePath));

	// Identify change regions and group into hunks
	TArray<int32> ChangeIndices;
	for (int32 Idx = 0; Idx < EditScript.Num(); Idx++)
	{
		if (EditScript[Idx].Op != TEXT(' '))
		{
			ChangeIndices.Add(Idx);
		}
	}

	if (ChangeIndices.Num() == 0)
	{
		Result += TEXT("(No differences detected)\n");
		return Result;
	}

	// Merge nearby changes into hunks
	int32 HunkStart = -1;
	int32 HunkEnd = -1;

	auto FlushHunk = [&]()
	{
		if (HunkStart < 0) return;

		// Expand to include context
		int32 Start = FMath::Max(0, HunkStart - ContextLines);
		int32 End = FMath::Min(EditScript.Num() - 1, HunkEnd + ContextLines);

		// Count old/new line ranges
		int32 OldStart = 0, OldCount = 0, NewStart = 0, NewCount = 0;
		bool bFoundOldStart = false, bFoundNewStart = false;

		for (int32 Idx = Start; Idx <= End; Idx++)
		{
			const FEditOp& Op = EditScript[Idx];
			if (Op.Op == TEXT(' ') || Op.Op == TEXT('-'))
			{
				OldCount++;
				if (!bFoundOldStart && Op.OldIdx > 0) { OldStart = Op.OldIdx; bFoundOldStart = true; }
			}
			if (Op.Op == TEXT(' ') || Op.Op == TEXT('+'))
			{
				NewCount++;
				if (!bFoundNewStart && Op.NewIdx > 0) { NewStart = Op.NewIdx; bFoundNewStart = true; }
			}
		}

		if (OldStart == 0) OldStart = 1;
		if (NewStart == 0) NewStart = 1;

		Result += FString::Printf(TEXT("@@ -%d,%d +%d,%d @@\n"), OldStart, OldCount, NewStart, NewCount);

		for (int32 Idx = Start; Idx <= End; Idx++)
		{
			const FEditOp& Op = EditScript[Idx];
			Result += FString::Printf(TEXT("%c%s\n"), Op.Op, *Op.Line);
		}

		HunkStart = -1;
		HunkEnd = -1;
	};

	for (int32 CI = 0; CI < ChangeIndices.Num(); CI++)
	{
		int32 ChangeIdx = ChangeIndices[CI];

		if (HunkStart < 0)
		{
			HunkStart = ChangeIdx;
			HunkEnd = ChangeIdx;
		}
		else if (ChangeIdx - HunkEnd <= ContextLines * 2 + 1)
		{
			// Merge into current hunk (close enough)
			HunkEnd = ChangeIdx;
		}
		else
		{
			// Flush current hunk and start new one
			FlushHunk();
			HunkStart = ChangeIdx;
			HunkEnd = ChangeIdx;
		}
	}

	FlushHunk();

	return Result;
}

FString FGitHubCopilotUEPatchService::GenerateUnifiedDiff(
	const FString& FilePath,
	const FString& OriginalContent,
	const FString& ProposedContent) const
{
	TArray<FString> OldLines;
	TArray<FString> NewLines;
	OriginalContent.ParseIntoArrayLines(OldLines);
	ProposedContent.ParseIntoArrayLines(NewLines);

	return BuildUnifiedDiffFromLCS(FilePath, OldLines, NewLines, 3);
}

// ============================================================================
// Preview creation and pending tracking
// ============================================================================

FCopilotDiffPreview FGitHubCopilotUEPatchService::CreateDiffPreview(
	const FString& FilePath,
	const FString& OriginalContent,
	const FString& ProposedContent)
{
	FCopilotDiffPreview Preview;
	Preview.OriginalFilePath = FilePath;
	Preview.OriginalContent = OriginalContent;
	Preview.ProposedContent = ProposedContent;
	Preview.UnifiedDiff = GenerateUnifiedDiff(FilePath, OriginalContent, ProposedContent);
	Preview.bIsValid = true;

	// Register as pending for approval flow
	PendingPreviews.Add(FilePath, Preview);
	LastPreview = Preview;

	Log(FString::Printf(TEXT("PatchService: Created diff preview for %s (%d -> %d chars, pending approval)"),
		*FilePath, OriginalContent.Len(), ProposedContent.Len()));
	return Preview;
}

bool FGitHubCopilotUEPatchService::HasPendingPreview(const FString& FilePath) const
{
	return PendingPreviews.Contains(FilePath);
}

bool FGitHubCopilotUEPatchService::GetPendingPreview(const FString& FilePath, FCopilotDiffPreview& OutPreview) const
{
	const FCopilotDiffPreview* Found = PendingPreviews.Find(FilePath);
	if (Found)
	{
		OutPreview = *Found;
		return true;
	}
	return false;
}

void FGitHubCopilotUEPatchService::ClearPendingPreview(const FString& FilePath)
{
	PendingPreviews.Remove(FilePath);
}

bool FGitHubCopilotUEPatchService::GetLastPreview(FCopilotDiffPreview& OutPreview) const
{
	if (LastPreview.bIsValid)
	{
		OutPreview = LastPreview;
		return true;
	}
	return false;
}

// ============================================================================
// Validation
// ============================================================================

bool FGitHubCopilotUEPatchService::ValidatePatch(const FCopilotDiffPreview& DiffPreview, FString& OutError) const
{
	if (!FileService.IsValid())
	{
		OutError = TEXT("FileService not available");
		return false;
	}

	if (!DiffPreview.bIsValid)
	{
		OutError = TEXT("Diff preview is marked invalid");
		return false;
	}

	if (DiffPreview.OriginalFilePath.IsEmpty())
	{
		OutError = TEXT("File path is empty");
		return false;
	}

	if (DiffPreview.ProposedContent.IsEmpty())
	{
		OutError = TEXT("Proposed content is empty");
		return false;
	}

	// Validate allowed write roots
	if (!FileService->IsPathWithinAllowedRoots(DiffPreview.OriginalFilePath))
	{
		OutError = FString::Printf(TEXT("REJECTED: Path '%s' is outside allowed write roots (check Project Settings -> Plugins -> GitHub Copilot UE -> Allowed Write Roots)"),
			*DiffPreview.OriginalFilePath);
		return false;
	}

	// Staleness check: verify file hasn't changed since snapshot
	FString CurrentContent;
	if (FileService->ReadFile(DiffPreview.OriginalFilePath, CurrentContent))
	{
		if (CurrentContent != DiffPreview.OriginalContent)
		{
			OutError = TEXT("CONFLICT: File has been modified since the snapshot was taken. Re-run Preview Patch to get a fresh diff.");
			return false;
		}
	}
	// If file doesn't exist yet (new file), that's OK — OriginalContent should be empty

	return true;
}

// ============================================================================
// Step-by-step apply
// ============================================================================

FCopilotPatchResult FGitHubCopilotUEPatchService::ApplyPatchWithSteps(const FCopilotDiffPreview& DiffPreview)
{
	FCopilotPatchResult Result;
	Result.FilePath = DiffPreview.OriginalFilePath;

	if (!FileService.IsValid())
	{
		Result.LogStep(ECopilotPatchStep::Failed, TEXT("FileService not available"));
		Result.ErrorMessage = TEXT("FileService not available");
		return Result;
	}

	// Step 1: Validate path
	Result.LogStep(ECopilotPatchStep::ValidatingPath, FString::Printf(TEXT("Validating path: %s"), *DiffPreview.OriginalFilePath));
	OnPatchStepProgress.Broadcast(DiffPreview.OriginalFilePath, ECopilotPatchStep::ValidatingPath);

	if (DiffPreview.OriginalFilePath.IsEmpty() || !DiffPreview.bIsValid)
	{
		Result.LogStep(ECopilotPatchStep::Failed, TEXT("Invalid diff preview (empty path or marked invalid)"));
		Result.ErrorMessage = TEXT("Invalid diff preview");
		return Result;
	}

	// Step 2: Validate allowed roots
	Result.LogStep(ECopilotPatchStep::ValidatingRoots, TEXT("Checking allowed write roots..."));
	OnPatchStepProgress.Broadcast(DiffPreview.OriginalFilePath, ECopilotPatchStep::ValidatingRoots);

	if (!FileService->IsPathWithinAllowedRoots(DiffPreview.OriginalFilePath))
	{
		FString Msg = FString::Printf(TEXT("REJECTED: '%s' is outside allowed write roots"), *DiffPreview.OriginalFilePath);
		Result.LogStep(ECopilotPatchStep::Failed, Msg);
		Result.ErrorMessage = Msg;
		return Result;
	}
	Result.LogStep(ECopilotPatchStep::ValidatingRoots, TEXT("Path is within allowed write roots"));

	// Step 3: Read original and check staleness
	Result.LogStep(ECopilotPatchStep::ReadingOriginal, TEXT("Reading current file content..."));
	OnPatchStepProgress.Broadcast(DiffPreview.OriginalFilePath, ECopilotPatchStep::ReadingOriginal);

	if (FileService->FileExists(DiffPreview.OriginalFilePath))
	{
		FString CurrentContent;
		if (FileService->ReadFile(DiffPreview.OriginalFilePath, CurrentContent))
		{
			if (CurrentContent != DiffPreview.OriginalContent)
			{
				FString Msg = TEXT("CONFLICT: File modified since snapshot — re-run Preview to get fresh diff");
				Result.LogStep(ECopilotPatchStep::Failed, Msg);
				Result.ErrorMessage = Msg;
				return Result;
			}
			Result.LogStep(ECopilotPatchStep::ReadingOriginal, TEXT("Content matches snapshot (no external changes)"));
		}
		else
		{
			Result.LogStep(ECopilotPatchStep::Failed, TEXT("Failed to read current file content"));
			Result.ErrorMessage = TEXT("Cannot read file");
			return Result;
		}
	}
	else
	{
		Result.LogStep(ECopilotPatchStep::ReadingOriginal, TEXT("File does not yet exist (new file creation)"));
	}

	// Step 4: Create backup
	Result.LogStep(ECopilotPatchStep::CreatingBackup, TEXT("Creating backup..."));
	OnPatchStepProgress.Broadcast(DiffPreview.OriginalFilePath, ECopilotPatchStep::CreatingBackup);

	if (FileService->FileExists(DiffPreview.OriginalFilePath))
	{
		FString BackupPath = FileService->CreateBackup(DiffPreview.OriginalFilePath);
		if (BackupPath.IsEmpty())
		{
			FString Msg = TEXT("Failed to create backup — aborting to protect original file");
			Result.LogStep(ECopilotPatchStep::Failed, Msg);
			Result.ErrorMessage = Msg;
			return Result;
		}
		Result.BackupPath = BackupPath;
		BackupRegistry.Add(DiffPreview.OriginalFilePath, BackupPath);
		Result.LogStep(ECopilotPatchStep::CreatingBackup, FString::Printf(TEXT("Backup created: %s"), *BackupPath));
	}
	else
	{
		Result.LogStep(ECopilotPatchStep::CreatingBackup, TEXT("No backup needed (new file)"));
	}

	// Step 5: Write proposed content
	Result.LogStep(ECopilotPatchStep::WritingFile, TEXT("Writing proposed content..."));
	OnPatchStepProgress.Broadcast(DiffPreview.OriginalFilePath, ECopilotPatchStep::WritingFile);

	FString WriteError;
	// Use CreateNewFile for new files, WriteFile for existing (WriteFile does its own backup, 
	// but we already backed up, so we bypass to avoid double-backup by writing directly)
	bool bWriteOk = false;
	if (FileService->FileExists(DiffPreview.OriginalFilePath))
	{
		// Write directly — backup already created above
		FString AbsPath = FPaths::IsRelative(DiffPreview.OriginalFilePath)
			? FPaths::Combine(FileService->GetProjectRoot(), DiffPreview.OriginalFilePath)
			: DiffPreview.OriginalFilePath;
		FPaths::NormalizeFilename(AbsPath);
		bWriteOk = FFileHelper::SaveStringToFile(DiffPreview.ProposedContent, *AbsPath,
			FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
		if (!bWriteOk) WriteError = TEXT("FFileHelper::SaveStringToFile failed");
	}
	else
	{
		bWriteOk = FileService->CreateNewFile(DiffPreview.OriginalFilePath, DiffPreview.ProposedContent, WriteError);
	}

	if (!bWriteOk)
	{
		FString Msg = FString::Printf(TEXT("Write failed: %s — attempting rollback"), *WriteError);
		Result.LogStep(ECopilotPatchStep::Failed, Msg);

		// Step 5b: Rollback on failure
		if (!Result.BackupPath.IsEmpty())
		{
			Result.LogStep(ECopilotPatchStep::RollingBack, TEXT("Rolling back from backup..."));
			OnPatchStepProgress.Broadcast(DiffPreview.OriginalFilePath, ECopilotPatchStep::RollingBack);

			FString RollbackError;
			if (Rollback(DiffPreview.OriginalFilePath, RollbackError))
			{
				Result.LogStep(ECopilotPatchStep::RollingBack, TEXT("Rollback succeeded — original file restored"));
			}
			else
			{
				Result.LogStep(ECopilotPatchStep::Failed, FString::Printf(TEXT("ROLLBACK FAILED: %s — backup at: %s"), *RollbackError, *Result.BackupPath));
			}
		}

		Result.ErrorMessage = Msg;
		return Result;
	}

	Result.LogStep(ECopilotPatchStep::WritingFile, FString::Printf(TEXT("Wrote %d chars"), DiffPreview.ProposedContent.Len()));

	// Step 6: Verify write
	Result.LogStep(ECopilotPatchStep::VerifyingWrite, TEXT("Verifying written content..."));
	OnPatchStepProgress.Broadcast(DiffPreview.OriginalFilePath, ECopilotPatchStep::VerifyingWrite);

	FString VerifyContent;
	if (FileService->ReadFile(DiffPreview.OriginalFilePath, VerifyContent))
	{
		if (VerifyContent == DiffPreview.ProposedContent)
		{
			Result.LogStep(ECopilotPatchStep::VerifyingWrite, TEXT("Verification passed — content matches"));
		}
		else
		{
			// Content mismatch after write — something went wrong
			FString Msg = TEXT("VERIFICATION FAILED: Written content does not match proposed content");
			Result.LogStep(ECopilotPatchStep::Failed, Msg);

			// Attempt rollback
			if (!Result.BackupPath.IsEmpty())
			{
				FString RollbackError;
				Rollback(DiffPreview.OriginalFilePath, RollbackError);
				Result.LogStep(ECopilotPatchStep::RollingBack, RollbackError.IsEmpty() ? TEXT("Rolled back") : *RollbackError);
			}

			Result.ErrorMessage = Msg;
			return Result;
		}
	}

	// Step 7: Complete
	Result.LogStep(ECopilotPatchStep::Complete, FString::Printf(TEXT("Patch applied successfully to %s"), *DiffPreview.OriginalFilePath));
	OnPatchStepProgress.Broadcast(DiffPreview.OriginalFilePath, ECopilotPatchStep::Complete);
	Result.bSuccess = true;

	// Clear from pending previews
	ClearPendingPreview(DiffPreview.OriginalFilePath);

	Log(FString::Printf(TEXT("PatchService: Patch applied to %s (backup: %s)"), *DiffPreview.OriginalFilePath, *Result.BackupPath));
	return Result;
}

// ============================================================================
// Legacy / convenience wrappers
// ============================================================================

bool FGitHubCopilotUEPatchService::ApplyPatch(const FCopilotDiffPreview& DiffPreview, FString& OutError)
{
	FCopilotPatchResult Result = ApplyPatchWithSteps(DiffPreview);
	if (!Result.bSuccess)
	{
		OutError = Result.ErrorMessage;
	}
	return Result.bSuccess;
}

bool FGitHubCopilotUEPatchService::ApplyImmediate(const FString& FilePath, const FString& NewContent, FString& OutError)
{
	if (!FileService.IsValid())
	{
		OutError = TEXT("FileService not available");
		return false;
	}

	FString OriginalContent;
	FileService->ReadFile(FilePath, OriginalContent);

	FCopilotDiffPreview Preview = CreateDiffPreview(FilePath, OriginalContent, NewContent);
	return ApplyPatch(Preview, OutError);
}

bool FGitHubCopilotUEPatchService::Rollback(const FString& FilePath, FString& OutError)
{
	if (!FileService.IsValid())
	{
		OutError = TEXT("FileService not available");
		return false;
	}

	FString* BackupPath = BackupRegistry.Find(FilePath);
	if (!BackupPath || BackupPath->IsEmpty())
	{
		OutError = FString::Printf(TEXT("No backup found for '%s'"), *FilePath);
		return false;
	}

	FString BackupContent;
	if (!FileService->ReadFile(*BackupPath, BackupContent))
	{
		OutError = FString::Printf(TEXT("Failed to read backup file '%s'"), **BackupPath);
		return false;
	}

	// Write backup content back (bypassing the normal WriteFile to avoid re-backup)
	FString NormalizedPath = FPaths::IsRelative(FilePath)
		? FPaths::Combine(FileService->GetProjectRoot(), FilePath) : FilePath;
	FPaths::NormalizeFilename(NormalizedPath);

	if (!FFileHelper::SaveStringToFile(BackupContent, *NormalizedPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		OutError = FString::Printf(TEXT("Failed to restore from backup to '%s'"), *FilePath);
		return false;
	}

	BackupRegistry.Remove(FilePath);
	ClearPendingPreview(FilePath);

	Log(FString::Printf(TEXT("PatchService: Rolled back %s from backup %s"), *FilePath, **BackupPath));
	return true;
}

bool FGitHubCopilotUEPatchService::InsertAtLine(const FString& FilePath, int32 LineNumber, const FString& Content, FString& OutError)
{
	if (!FileService.IsValid())
	{
		OutError = TEXT("FileService not available");
		return false;
	}

	FString FileContent;
	if (!FileService->ReadFile(FilePath, FileContent))
	{
		OutError = FString::Printf(TEXT("Failed to read file '%s'"), *FilePath);
		return false;
	}

	TArray<FString> Lines;
	FileContent.ParseIntoArrayLines(Lines);

	if (LineNumber < 0 || LineNumber > Lines.Num())
	{
		OutError = FString::Printf(TEXT("Line number %d is out of range (0-%d)"), LineNumber, Lines.Num());
		return false;
	}

	TArray<FString> NewContentLines;
	Content.ParseIntoArrayLines(NewContentLines);

	for (int32 i = NewContentLines.Num() - 1; i >= 0; i--)
	{
		Lines.Insert(NewContentLines[i], LineNumber);
	}

	FString NewFileContent = FString::Join(Lines, TEXT("\n"));
	return ApplyImmediate(FilePath, NewFileContent, OutError);
}

void FGitHubCopilotUEPatchService::Log(const FString& Message) const
{
	UE_LOG(LogGitHubCopilotUE, Verbose, TEXT("%s"), *Message);
	OnLogMessage.Broadcast(Message);
}
