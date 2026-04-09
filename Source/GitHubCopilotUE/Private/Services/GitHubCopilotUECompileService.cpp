// Copyright GitHub, Inc. All Rights Reserved.

#include "Services/GitHubCopilotUECompileService.h"
#include "GitHubCopilotUESettings.h"
#include "Editor.h"
#include "Editor/UnrealEdEngine.h"
#include "UnrealEdGlobals.h"

// Live Coding support - version sensitive
// UE 5.0+ has ILiveCodingModule
#if WITH_LIVE_CODING
#include "ILiveCodingModule.h"
#endif

FGitHubCopilotUECompileService::FGitHubCopilotUECompileService()
{
}

FCopilotResponse FGitHubCopilotUECompileService::RequestCompile()
{
	FCopilotResponse Response;
	Response.RequestId = FGuid::NewGuid().ToString();
	Response.Timestamp = FDateTime::Now().ToString();

	const UGitHubCopilotUESettings* Settings = UGitHubCopilotUESettings::Get();
	if (!Settings || !Settings->bEnableCompileCommands)
	{
		Response.ResultStatus = ECopilotResultStatus::Failure;
		Response.ErrorMessage = TEXT("Compile commands are disabled in plugin settings");
		Log(TEXT("CompileService: Compile rejected - disabled in settings"));
		return Response;
	}

	if (IsCompiling())
	{
		Response.ResultStatus = ECopilotResultStatus::Failure;
		Response.ErrorMessage = TEXT("A compile is already in progress");
		Log(TEXT("CompileService: Compile rejected - already compiling"));
		return Response;
	}

	Log(TEXT("CompileService: Requesting editor recompile..."));

	// Trigger the editor's recompile mechanism
	// This calls the same path as the editor's "Recompile" button
	if (GEditor)
	{
		bool bCompileStarted = false;

		// Trigger recompile through the editor exec pathway.
		// Deferred to next tick via the world timer manager to avoid re-entrant issues.
		UWorld* EditorWorld = GEditor->GetEditorWorldContext().World();
		if (EditorWorld)
		{
			FTimerHandle TempHandle;
			EditorWorld->GetTimerManager().SetTimerForNextTick([](){ 
				if (GUnrealEd)
				{
					GUnrealEd->Exec(GEditor->GetEditorWorldContext().World(), TEXT("RECOMPILE"));
				}
			});
			bCompileStarted = true;
		}

		if (bCompileStarted)
		{
			Response.ResultStatus = ECopilotResultStatus::Success;
			Response.ResponseText = TEXT("Compile request submitted. Check the Output Log for results.");
			Log(TEXT("CompileService: Compile request submitted"));
		}
		else
		{
			Response.ResultStatus = ECopilotResultStatus::Failure;
			Response.ErrorMessage = TEXT("Failed to initiate compile - editor compile path not available");
			Log(TEXT("CompileService: Failed to start compile"));
		}
	}
	else
	{
		Response.ResultStatus = ECopilotResultStatus::Failure;
		Response.ErrorMessage = TEXT("GEditor not available");
		Log(TEXT("CompileService: GEditor not available"));
	}

	return Response;
}

FCopilotResponse FGitHubCopilotUECompileService::RequestLiveCodingPatch()
{
	FCopilotResponse Response;
	Response.RequestId = FGuid::NewGuid().ToString();
	Response.Timestamp = FDateTime::Now().ToString();

	const UGitHubCopilotUESettings* Settings = UGitHubCopilotUESettings::Get();
	if (!Settings || !Settings->bEnableLiveCodingCommands)
	{
		Response.ResultStatus = ECopilotResultStatus::Failure;
		Response.ErrorMessage = TEXT("Live Coding commands are disabled in plugin settings");
		Log(TEXT("CompileService: Live Coding rejected - disabled in settings"));
		return Response;
	}

	if (!IsLiveCodingAvailable())
	{
		Response.ResultStatus = ECopilotResultStatus::Failure;
		Response.ErrorMessage = TEXT("Live Coding is not available in this build");
		Log(TEXT("CompileService: Live Coding not available"));
		return Response;
	}

#if WITH_LIVE_CODING
	Log(TEXT("CompileService: Requesting Live Coding patch..."));

	ILiveCodingModule& LiveCoding = FModuleManager::LoadModuleChecked<ILiveCodingModule>(LIVE_CODING_MODULE_NAME);
	if (LiveCoding.IsEnabledForSession())
	{
		LiveCoding.EnableForSession(true);
		// Trigger a Live Coding compile
		LiveCoding.Compile();

		Response.ResultStatus = ECopilotResultStatus::Success;
		Response.ResponseText = TEXT("Live Coding patch requested. Watch for console output.");
		Log(TEXT("CompileService: Live Coding patch requested"));
	}
	else
	{
		Response.ResultStatus = ECopilotResultStatus::Failure;
		Response.ErrorMessage = TEXT("Live Coding is not enabled for this session. Enable it in Editor Preferences.");
		Log(TEXT("CompileService: Live Coding not enabled for session"));
	}
#else
	Response.ResultStatus = ECopilotResultStatus::Failure;
	Response.ErrorMessage = TEXT("Live Coding support not compiled in this build");
	Log(TEXT("CompileService: WITH_LIVE_CODING not defined"));
#endif

	return Response;
}

FCopilotResponse FGitHubCopilotUECompileService::RunAutomationTests(const FString& TestFilter)
{
	FCopilotResponse Response;
	Response.RequestId = FGuid::NewGuid().ToString();
	Response.Timestamp = FDateTime::Now().ToString();

	FString Filter = TestFilter.IsEmpty() ? TEXT("Project.") : TestFilter;

	Log(FString::Printf(TEXT("CompileService: Running automation tests with filter: %s"), *Filter));

	// Execute automation test command through the editor
	if (GEditor)
	{
		FString Command = FString::Printf(TEXT("Automation RunTests %s"), *Filter);
		GEditor->Exec(GEditor->GetEditorWorldContext().World(), *Command, *GLog);

		Response.ResultStatus = ECopilotResultStatus::Success;
		Response.ResponseText = FString::Printf(TEXT("Automation tests started with filter: %s\nCheck the Automation tab and Output Log for results."), *Filter);
		Log(TEXT("CompileService: Automation tests started"));
	}
	else
	{
		Response.ResultStatus = ECopilotResultStatus::Failure;
		Response.ErrorMessage = TEXT("GEditor not available to run automation tests");
		Log(TEXT("CompileService: GEditor not available"));
	}

	return Response;
}

bool FGitHubCopilotUECompileService::IsLiveCodingAvailable() const
{
#if WITH_LIVE_CODING
	return FModuleManager::Get().IsModuleLoaded(LIVE_CODING_MODULE_NAME);
#else
	return false;
#endif
}

bool FGitHubCopilotUECompileService::IsCompiling() const
{
	// UEditorEngine does not expose a public IsCompiling() method in UE5.
	// The engine will internally reject or queue concurrent compile requests,
	// so returning false here is safe — it allows the request to proceed and
	// the editor's own compile pipeline handles contention.
	return false;
}

void FGitHubCopilotUECompileService::Log(const FString& Message)
{
	UE_LOG(LogGitHubCopilotUE, Verbose, TEXT("%s"), *Message);
	OnLogMessage.Broadcast(Message);
}
