// Copyright GitHub, Inc. All Rights Reserved.

#include "Services/GitHubCopilotUEQuestService.h"
#include "GitHubCopilotUESettings.h"
#include "Interfaces/IPluginManager.h"
#include "Editor.h"
#include "Engine/World.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/Character.h"
#include "Camera/CameraActor.h"
#include "EngineUtils.h"

FGitHubCopilotUEQuestService::FGitHubCopilotUEQuestService()
{
}

FCopilotQuestReadiness FGitHubCopilotUEQuestService::RunQuestAudit() const
{
	FCopilotQuestReadiness Readiness;

	const UGitHubCopilotUESettings* Settings = UGitHubCopilotUESettings::Get();
	if (!Settings || !Settings->bEnableQuestWorkflowAnalysis)
	{
		Readiness.Summary = TEXT("Quest workflow analysis is disabled in plugin settings");
		Log(TEXT("QuestService: Quest analysis disabled"));
		return Readiness;
	}

	Log(TEXT("QuestService: Running Quest readiness audit..."));

	Readiness.bOpenXRPluginEnabled = IsOpenXREnabled();
	Readiness.bMetaXRPluginEnabled = IsMetaXREnabled();
	Readiness.bAndroidPlatformConfigured = IsAndroidConfigured();
	Readiness.XRRelevantPlugins = GetXRPlugins();
	Readiness.VRRelevantActors = GetVRRelevantActors();
	Readiness.Summary = GenerateReadinessSummary(Readiness);

	Log(FString::Printf(TEXT("QuestService: Audit complete - OpenXR: %s, MetaXR: %s, Android: %s"),
		Readiness.bOpenXRPluginEnabled ? TEXT("YES") : TEXT("NO"),
		Readiness.bMetaXRPluginEnabled ? TEXT("YES") : TEXT("NO"),
		Readiness.bAndroidPlatformConfigured ? TEXT("YES") : TEXT("NO")));

	return Readiness;
}

bool FGitHubCopilotUEQuestService::IsOpenXREnabled() const
{
	TArray<TSharedRef<IPlugin>> Plugins = IPluginManager::Get().GetEnabledPlugins();
	for (const TSharedRef<IPlugin>& Plugin : Plugins)
	{
		if (Plugin->GetName().Contains(TEXT("OpenXR")))
		{
			return true;
		}
	}
	return false;
}

bool FGitHubCopilotUEQuestService::IsMetaXREnabled() const
{
	TArray<TSharedRef<IPlugin>> Plugins = IPluginManager::Get().GetEnabledPlugins();
	for (const TSharedRef<IPlugin>& Plugin : Plugins)
	{
		FString Name = Plugin->GetName();
		if (Name.Contains(TEXT("MetaXR")) || Name.Contains(TEXT("OculusXR")) || Name.Contains(TEXT("OculusVR")))
		{
			return true;
		}
	}
	return false;
}

bool FGitHubCopilotUEQuestService::IsAndroidConfigured() const
{
	// Heuristic: check if Android platform support appears configured
	// This checks for the existence of Android SDK paths in the engine config
	FString AndroidHome;
	bool bHasAndroidSDK = GConfig->GetString(TEXT("/Script/AndroidPlatformEditor.AndroidSDKSettings"), TEXT("SDKPath"), AndroidHome, GEngineIni);

	// Also check if Android is in the target platform list
	FString ProjectDir = FPaths::ProjectDir();
	FString AndroidConfigPath = ProjectDir / TEXT("Config") / TEXT("Android") / TEXT("AndroidEngine.ini");

	return bHasAndroidSDK || FPaths::FileExists(AndroidConfigPath);
}

TArray<FString> FGitHubCopilotUEQuestService::GetVRRelevantActors() const
{
	TArray<FString> Result;

	if (!GEditor)
	{
		return Result;
	}

	UWorld* World = GEditor->GetEditorWorldContext().World();
	if (!World)
	{
		return Result;
	}

	// Find VR-relevant actors: Pawns, Characters, Cameras, MotionControllers
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* Actor = *It;
		if (!Actor) continue;

		FString ClassName = Actor->GetClass()->GetName();
		bool bIsVRRelevant = false;

		// Pawns and Characters are always VR-relevant
		if (Actor->IsA<APawn>() || Actor->IsA<ACharacter>())
		{
			bIsVRRelevant = true;
		}
		// Camera actors
		else if (Actor->IsA<ACameraActor>())
		{
			bIsVRRelevant = true;
		}
		// Check for VR/XR keywords in class name
		else if (ClassName.Contains(TEXT("VR")) || ClassName.Contains(TEXT("XR")) ||
				 ClassName.Contains(TEXT("Motion")) || ClassName.Contains(TEXT("Hand")) ||
				 ClassName.Contains(TEXT("HMD")) || ClassName.Contains(TEXT("Teleport")))
		{
			bIsVRRelevant = true;
		}

		if (bIsVRRelevant)
		{
			Result.Add(FString::Printf(TEXT("%s (%s)"), *Actor->GetActorLabel(), *ClassName));
		}
	}

	return Result;
}

TArray<FString> FGitHubCopilotUEQuestService::GetXRPlugins() const
{
	TArray<FString> Result;

	static const TArray<FString> XRKeywords = {
		TEXT("OpenXR"), TEXT("OculusXR"), TEXT("MetaXR"), TEXT("SteamVR"),
		TEXT("XR"), TEXT("VR"), TEXT("HeadMounted"), TEXT("Oculus"),
		TEXT("HandTracking"), TEXT("EyeTracking")
	};

	TArray<TSharedRef<IPlugin>> Plugins = IPluginManager::Get().GetEnabledPlugins();
	for (const TSharedRef<IPlugin>& Plugin : Plugins)
	{
		FString Name = Plugin->GetName();
		for (const FString& Keyword : XRKeywords)
		{
			if (Name.Contains(Keyword))
			{
				Result.Add(Name);
				break;
			}
		}
	}

	return Result;
}

FString FGitHubCopilotUEQuestService::GenerateReadinessSummary(const FCopilotQuestReadiness& Readiness) const
{
	FString Summary;

	Summary += TEXT("=== Meta Quest Readiness Summary ===\n\n");

	// OpenXR status
	Summary += FString::Printf(TEXT("OpenXR Plugin: %s\n"),
		Readiness.bOpenXRPluginEnabled ? TEXT("ENABLED ✓") : TEXT("NOT FOUND ✗"));

	// MetaXR status
	Summary += FString::Printf(TEXT("MetaXR/OculusXR Plugin: %s\n"),
		Readiness.bMetaXRPluginEnabled ? TEXT("ENABLED ✓") : TEXT("NOT FOUND ✗"));

	// Android status
	Summary += FString::Printf(TEXT("Android Platform: %s\n"),
		Readiness.bAndroidPlatformConfigured ? TEXT("CONFIGURED ✓") : TEXT("NOT CONFIGURED ✗"));

	// XR plugins
	Summary += FString::Printf(TEXT("\nEnabled XR Plugins (%d):\n"), Readiness.XRRelevantPlugins.Num());
	for (const FString& Plugin : Readiness.XRRelevantPlugins)
	{
		Summary += FString::Printf(TEXT("  - %s\n"), *Plugin);
	}

	// VR actors
	Summary += FString::Printf(TEXT("\nVR-Relevant Actors in Level (%d):\n"), Readiness.VRRelevantActors.Num());
	for (const FString& Actor : Readiness.VRRelevantActors)
	{
		Summary += FString::Printf(TEXT("  - %s\n"), *Actor);
	}

	// Overall assessment
	Summary += TEXT("\n--- Assessment ---\n");
	int32 Score = 0;
	if (Readiness.bOpenXRPluginEnabled) Score++;
	if (Readiness.bMetaXRPluginEnabled) Score++;
	if (Readiness.bAndroidPlatformConfigured) Score++;

	if (Score >= 3)
	{
		Summary += TEXT("Status: READY for Quest development\n");
	}
	else if (Score >= 1)
	{
		Summary += TEXT("Status: PARTIAL setup - some components missing\n");
	}
	else
	{
		Summary += TEXT("Status: NOT CONFIGURED for Quest development\n");
	}

	return Summary;
}

void FGitHubCopilotUEQuestService::Log(const FString& Message) const
{
	UE_LOG(LogGitHubCopilotUE, Verbose, TEXT("%s"), *Message);
	OnLogMessage.Broadcast(Message);
}
