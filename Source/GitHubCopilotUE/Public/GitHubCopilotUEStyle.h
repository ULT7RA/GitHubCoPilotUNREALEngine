// Copyright GitHub, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Styling/SlateStyle.h"

/**
 * Manages the Slate style set for the GitHub Copilot UE plugin.
 */
class FGitHubCopilotUEStyle
{
public:
	static void Initialize();
	static void Shutdown();
	static void ReloadTextures();

	static const ISlateStyle& Get();
	static FName GetStyleSetName();

	static const FSlateBrush* GetBrush(FName PropertyName);

private:
	static TSharedRef<FSlateStyleSet> Create();
	static TSharedPtr<FSlateStyleSet> StyleInstance;
};
