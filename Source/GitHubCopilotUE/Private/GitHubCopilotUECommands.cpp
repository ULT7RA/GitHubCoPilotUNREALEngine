// Copyright GitHub, Inc. All Rights Reserved.

#include "GitHubCopilotUECommands.h"

#define LOCTEXT_NAMESPACE "FGitHubCopilotUEModule"

FGitHubCopilotUECommands::FGitHubCopilotUECommands()
	: TCommands<FGitHubCopilotUECommands>(
		TEXT("GitHubCopilotUE"),
		NSLOCTEXT("Contexts", "GitHubCopilotUE", "GitHub Copilot UE Plugin"),
		NAME_None,
		FGitHubCopilotUEStyle::GetStyleSetName())
{
}

void FGitHubCopilotUECommands::RegisterCommands()
{
	UI_COMMAND(OpenPluginWindow, "GitHub Copilot", "Open the GitHub Copilot assistant panel", EUserInterfaceActionType::Button, FInputChord(EModifierKey::Control | EModifierKey::Shift, EKeys::G));
	UI_COMMAND(AnalyzeSelection, "Analyze Selection", "Analyze the current selection with Copilot", EUserInterfaceActionType::Button, FInputChord());
	UI_COMMAND(ExplainCode, "Explain Code", "Ask Copilot to explain selected code", EUserInterfaceActionType::Button, FInputChord());
	UI_COMMAND(TriggerCompile, "Trigger Compile", "Trigger a project compile via Copilot", EUserInterfaceActionType::Button, FInputChord());
	UI_COMMAND(TriggerLiveCoding, "Trigger Live Coding", "Trigger Live Coding patch via Copilot", EUserInterfaceActionType::Button, FInputChord());
}

#undef LOCTEXT_NAMESPACE
