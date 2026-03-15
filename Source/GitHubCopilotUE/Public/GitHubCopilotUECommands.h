// Copyright GitHub, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Framework/Commands/Commands.h"
#include "GitHubCopilotUEStyle.h"

/**
 * UI command definitions for the GitHub Copilot UE plugin.
 */
class FGitHubCopilotUECommands : public TCommands<FGitHubCopilotUECommands>
{
public:
	FGitHubCopilotUECommands();

	// TCommands<> interface
	virtual void RegisterCommands() override;

	TSharedPtr<FUICommandInfo> OpenPluginWindow;
	TSharedPtr<FUICommandInfo> AnalyzeSelection;
	TSharedPtr<FUICommandInfo> ExplainCode;
	TSharedPtr<FUICommandInfo> TriggerCompile;
	TSharedPtr<FUICommandInfo> TriggerLiveCoding;
};
