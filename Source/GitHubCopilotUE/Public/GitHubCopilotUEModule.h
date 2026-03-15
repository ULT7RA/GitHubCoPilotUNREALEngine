// Copyright GitHub, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"

class FGitHubCopilotUEContextService;
class FGitHubCopilotUEFileService;
class FGitHubCopilotUEPatchService;
class FGitHubCopilotUECommandRouter;
class FGitHubCopilotUEBridgeService;
class FGitHubCopilotUECompileService;
class FGitHubCopilotUEQuestService;
class FGitHubCopilotUESlashCommands;
class FGitHubCopilotUEConsoleCommands;
class FGitHubCopilotUEConsoleExecutor;

/**
 * Editor module for the GitHub Copilot UE plugin.
 * Manages service lifecycle, tab spawning, menu/toolbar registration.
 */
class FGitHubCopilotUEModule : public IModuleInterface
{
public:
	/** IModuleInterface implementation */
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

	/** Get the module instance */
	static FGitHubCopilotUEModule& Get();

	/** Access to services */
	TSharedPtr<FGitHubCopilotUEContextService> GetContextService() const { return ContextService; }
	TSharedPtr<FGitHubCopilotUEFileService> GetFileService() const { return FileService; }
	TSharedPtr<FGitHubCopilotUEPatchService> GetPatchService() const { return PatchService; }
	TSharedPtr<FGitHubCopilotUECommandRouter> GetCommandRouter() const { return CommandRouter; }
	TSharedPtr<FGitHubCopilotUEBridgeService> GetBridgeService() const { return BridgeService; }
	TSharedPtr<FGitHubCopilotUECompileService> GetCompileService() const { return CompileService; }
	TSharedPtr<FGitHubCopilotUEQuestService> GetQuestService() const { return QuestService; }
	TSharedPtr<FGitHubCopilotUESlashCommands> GetSlashCommands() const { return SlashCommands; }
	TSharedPtr<FGitHubCopilotUEConsoleCommands> GetConsoleCommands() const { return ConsoleCommands; }

private:
	/** Register the dockable tab spawner */
	void RegisterTabSpawner();
	void UnregisterTabSpawner();

	/** Register menus and toolbar extensions */
	void RegisterMenus();

	/** Tab spawner callback */
	TSharedRef<class SDockTab> OnSpawnPluginTab(const class FSpawnTabArgs& SpawnTabArgs);

	/** Initialize all services */
	void InitializeServices();

	/** Shutdown all services */
	void ShutdownServices();

	/** Handle commands */
	void PluginButtonClicked();

	// Services
	TSharedPtr<FGitHubCopilotUEContextService> ContextService;
	TSharedPtr<FGitHubCopilotUEFileService> FileService;
	TSharedPtr<FGitHubCopilotUEPatchService> PatchService;
	TSharedPtr<FGitHubCopilotUECommandRouter> CommandRouter;
	TSharedPtr<FGitHubCopilotUEBridgeService> BridgeService;
	TSharedPtr<FGitHubCopilotUECompileService> CompileService;
	TSharedPtr<FGitHubCopilotUEQuestService> QuestService;
	TSharedPtr<FGitHubCopilotUESlashCommands> SlashCommands;
	TSharedPtr<FGitHubCopilotUEConsoleCommands> ConsoleCommands;
	TSharedPtr<FGitHubCopilotUEConsoleExecutor> ConsoleExecutor;

	TSharedPtr<class FUICommandList> PluginCommands;

	static const FName CopilotTabName;
};
