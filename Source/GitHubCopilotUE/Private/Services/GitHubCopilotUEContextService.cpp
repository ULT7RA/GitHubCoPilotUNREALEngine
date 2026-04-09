// Copyright GitHub, Inc. All Rights Reserved.

#include "Services/GitHubCopilotUEContextService.h"
#include "GitHubCopilotUESettings.h"
#include "Misc/Paths.h"
#include "Misc/App.h"
#include "HAL/FileManager.h"
#include "Engine/World.h"
#include "Editor.h"
#include "Engine/Selection.h"
#include "LevelEditor.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "ContentBrowserModule.h"
#include "IContentBrowserSingleton.h"
#include "Interfaces/IPluginManager.h"

FGitHubCopilotUEContextService::FGitHubCopilotUEContextService()
{
}

FCopilotProjectContext FGitHubCopilotUEContextService::GatherProjectContext() const
{
	FCopilotProjectContext Ctx;
	Ctx.ProjectName = GetProjectName();
	Ctx.EngineVersion = GetEngineVersion();
	Ctx.CurrentMapName = GetCurrentMapName();
	Ctx.SelectedAssets = GetSelectedAssets();
	Ctx.SelectedActors = GetSelectedActors();
	Ctx.EnabledPlugins = GetEnabledPlugins();
	Ctx.EnabledXRPlugins = GetEnabledXRPlugins();
	Ctx.ActivePlatform = GetActivePlatform();
	Ctx.ProjectSourceDirectories = GetProjectSourceDirectories();
	Ctx.ModuleNames = GetModuleNames();

	UE_LOG(LogGitHubCopilotUE, Verbose, TEXT("Gathered project context: %s, Map: %s, %d assets selected, %d actors selected"),
		*Ctx.ProjectName, *Ctx.CurrentMapName, Ctx.SelectedAssets.Num(), Ctx.SelectedActors.Num());

	return Ctx;
}

FString FGitHubCopilotUEContextService::GetProjectName() const
{
	return FApp::GetProjectName();
}

FString FGitHubCopilotUEContextService::GetEngineVersion() const
{
	return FEngineVersion::Current().ToString();
}

FString FGitHubCopilotUEContextService::GetCurrentMapName() const
{
	if (GEditor && GEditor->GetEditorWorldContext().World())
	{
		return GEditor->GetEditorWorldContext().World()->GetMapName();
	}
	return TEXT("None");
}

TArray<FString> FGitHubCopilotUEContextService::GetSelectedAssets() const
{
	TArray<FString> Result;

	FContentBrowserModule& ContentBrowserModule = FModuleManager::LoadModuleChecked<FContentBrowserModule>("ContentBrowser");
	TArray<FAssetData> SelectedAssetData;
	ContentBrowserModule.Get().GetSelectedAssets(SelectedAssetData);

	for (const FAssetData& Asset : SelectedAssetData)
	{
		Result.Add(Asset.GetObjectPathString());
	}

	return Result;
}

TArray<FString> FGitHubCopilotUEContextService::GetSelectedActors() const
{
	TArray<FString> Result;

	if (GEditor)
	{
		USelection* Selection = GEditor->GetSelectedActors();
		if (Selection)
		{
			for (int32 i = 0; i < Selection->Num(); i++)
			{
				UObject* Obj = Selection->GetSelectedObject(i);
				if (AActor* Actor = Cast<AActor>(Obj))
				{
					Result.Add(FString::Printf(TEXT("%s (%s)"), *Actor->GetActorLabel(), *Actor->GetClass()->GetName()));
				}
			}
		}
	}

	return Result;
}

TArray<FString> FGitHubCopilotUEContextService::GetEnabledPlugins() const
{
	TArray<FString> Result;

	TArray<TSharedRef<IPlugin>> Plugins = IPluginManager::Get().GetEnabledPlugins();
	for (const TSharedRef<IPlugin>& Plugin : Plugins)
	{
		Result.Add(Plugin->GetName());
	}

	return Result;
}

TArray<FString> FGitHubCopilotUEContextService::GetEnabledXRPlugins() const
{
	TArray<FString> Result;

	static const TArray<FString> XRKeywords = {
		TEXT("OpenXR"), TEXT("OculusXR"), TEXT("MetaXR"), TEXT("SteamVR"),
		TEXT("XR"), TEXT("VR"), TEXT("HeadMounted"), TEXT("Oculus")
	};

	TArray<FString> AllPlugins = GetEnabledPlugins();
	for (const FString& PluginName : AllPlugins)
	{
		for (const FString& Keyword : XRKeywords)
		{
			if (PluginName.Contains(Keyword))
			{
				Result.Add(PluginName);
				break;
			}
		}
	}

	return Result;
}

TArray<FString> FGitHubCopilotUEContextService::GetProjectSourceDirectories() const
{
	TArray<FString> Result;

	FString SourceDir = FPaths::ProjectDir() / TEXT("Source");
	if (FPaths::DirectoryExists(SourceDir))
	{
		TArray<FString> SubDirs;
		IFileManager::Get().FindFiles(SubDirs, *(SourceDir / TEXT("*")), false, true);
		for (const FString& Dir : SubDirs)
		{
			Result.Add(Dir);
		}
	}

	// Also scan plugin source dirs
	FString PluginsDir = FPaths::ProjectDir() / TEXT("Plugins");
	if (FPaths::DirectoryExists(PluginsDir))
	{
		Result.Add(TEXT("Plugins/"));
	}

	return Result;
}

TArray<FString> FGitHubCopilotUEContextService::GetModuleNames() const
{
	TArray<FString> Result;

	// Get module names from project source directories
	FString SourceDir = FPaths::ProjectDir() / TEXT("Source");
	if (FPaths::DirectoryExists(SourceDir))
	{
		TArray<FString> SubDirs;
		IFileManager::Get().FindFiles(SubDirs, *(SourceDir / TEXT("*")), false, true);
		for (const FString& Dir : SubDirs)
		{
			// Check if this directory has a .Build.cs file
			FString BuildCsPattern = FPaths::ProjectDir() / TEXT("Source") / Dir / (Dir + TEXT(".Build.cs"));
			if (FPaths::FileExists(BuildCsPattern))
			{
				Result.Add(Dir);
			}
		}
	}

	return Result;
}

FString FGitHubCopilotUEContextService::GetActivePlatform() const
{
	const UGitHubCopilotUESettings* Settings = UGitHubCopilotUESettings::Get();
	if (Settings && !Settings->DefaultTargetPlatform.IsEmpty())
	{
		return Settings->DefaultTargetPlatform;
	}

	// Fallback to current platform
	return FPlatformProperties::IniPlatformName();
}

TArray<FString> FGitHubCopilotUEContextService::GetSelectedBlueprintAssets() const
{
	TArray<FString> Result;

	const UGitHubCopilotUESettings* Settings = UGitHubCopilotUESettings::Get();
	if (!Settings || !Settings->bEnableBlueprintContextCollection)
	{
		return Result;
	}

	FContentBrowserModule& ContentBrowserModule = FModuleManager::LoadModuleChecked<FContentBrowserModule>("ContentBrowser");
	TArray<FAssetData> SelectedAssetData;
	ContentBrowserModule.Get().GetSelectedAssets(SelectedAssetData);

	for (const FAssetData& Asset : SelectedAssetData)
	{
		// Filter for Blueprint assets
		if (Asset.AssetClassPath.GetAssetName() == FName(TEXT("Blueprint")) ||
			Asset.AssetClassPath.GetAssetName() == FName(TEXT("WidgetBlueprint")) ||
			Asset.AssetClassPath.GetAssetName() == FName(TEXT("AnimBlueprint")))
		{
			Result.Add(Asset.GetObjectPathString());
		}
	}

	return Result;
}

TArray<FString> FGitHubCopilotUEContextService::GetRecentOutputLogLines(int32 MaxLines) const
{
	TArray<FString> Result;
	// Output log access: In production, this would hook into FOutputDevice.
	// For now, we return an informational placeholder. A full implementation
	// would use a custom FOutputDevice registered at startup to capture lines.
	Result.Add(TEXT("[Output log capture requires custom FOutputDevice hook - see GitHubCopilotUE docs]"));
	return Result;
}
