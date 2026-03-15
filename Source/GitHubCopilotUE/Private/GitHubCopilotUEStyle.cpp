// Copyright GitHub, Inc. All Rights Reserved.

#include "GitHubCopilotUEStyle.h"
#include "Styling/SlateStyleRegistry.h"
#include "Framework/Application/SlateApplication.h"
#include "Interfaces/IPluginManager.h"

#define IMAGE_BRUSH(RelativePath, ...) FSlateImageBrush(Style->RootToContentDir(RelativePath, TEXT(".png")), __VA_ARGS__)
#define BOX_BRUSH(RelativePath, ...) FSlateBoxBrush(Style->RootToContentDir(RelativePath, TEXT(".png")), __VA_ARGS__)

TSharedPtr<FSlateStyleSet> FGitHubCopilotUEStyle::StyleInstance = nullptr;

void FGitHubCopilotUEStyle::Initialize()
{
	if (!StyleInstance.IsValid())
	{
		StyleInstance = Create();
		FSlateStyleRegistry::RegisterSlateStyle(*StyleInstance);
	}
}

void FGitHubCopilotUEStyle::Shutdown()
{
	if (StyleInstance.IsValid())
	{
		FSlateStyleRegistry::UnRegisterSlateStyle(*StyleInstance);
		ensure(StyleInstance.IsUnique());
		StyleInstance.Reset();
	}
}

void FGitHubCopilotUEStyle::ReloadTextures()
{
	if (FSlateApplication::IsInitialized())
	{
		FSlateApplication::Get().GetRenderer()->ReloadTextureResources();
	}
}

const ISlateStyle& FGitHubCopilotUEStyle::Get()
{
	return *StyleInstance;
}

FName FGitHubCopilotUEStyle::GetStyleSetName()
{
	static FName StyleSetName(TEXT("GitHubCopilotUEStyle"));
	return StyleSetName;
}

const FSlateBrush* FGitHubCopilotUEStyle::GetBrush(FName PropertyName)
{
	return StyleInstance->GetBrush(PropertyName);
}

TSharedRef<FSlateStyleSet> FGitHubCopilotUEStyle::Create()
{
	TSharedRef<FSlateStyleSet> Style = MakeShareable(new FSlateStyleSet(GetStyleSetName()));

	// Set content root to the plugin's Resources directory
	TSharedPtr<IPlugin> ThisPlugin = IPluginManager::Get().FindPlugin(TEXT("GitHubCopilotUE"));
	if (ThisPlugin.IsValid())
	{
		Style->SetContentRoot(ThisPlugin->GetBaseDir() / TEXT("Resources"));
	}

	// Define default styles - use engine built-in brushes as fallback
	const FVector2D Icon16x16(16.0f, 16.0f);
	const FVector2D Icon20x20(20.0f, 20.0f);
	const FVector2D Icon40x40(40.0f, 40.0f);

	// Tab icon styles using default app icon as fallback
	Style->Set("GitHubCopilotUE.OpenPluginWindow", new FSlateImageBrush(
		FPaths::EngineContentDir() / TEXT("Editor/Slate/Icons/icon_tab_Toolbars_40x.png"), Icon40x40));
	Style->Set("GitHubCopilotUE.OpenPluginWindow.Small", new FSlateImageBrush(
		FPaths::EngineContentDir() / TEXT("Editor/Slate/Icons/icon_tab_Toolbars_16x.png"), Icon16x16));

	return Style;
}

#undef IMAGE_BRUSH
#undef BOX_BRUSH
