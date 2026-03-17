// Copyright GitHub, Inc. All Rights Reserved.

#include "GitHubCopilotUEModule.h"
#include "GitHubCopilotUECommands.h"
#include "GitHubCopilotUEStyle.h"
#include "GitHubCopilotUESettings.h"
#include "Services/GitHubCopilotUETypes.h"
#include "Services/GitHubCopilotUEContextService.h"
#include "Services/GitHubCopilotUEFileService.h"
#include "Services/GitHubCopilotUEPatchService.h"
#include "Services/GitHubCopilotUECommandRouter.h"
#include "Services/GitHubCopilotUEBridgeService.h"
#include "Services/GitHubCopilotUECompileService.h"
#include "Services/GitHubCopilotUEQuestService.h"
#include "Services/GitHubCopilotUESlashCommands.h"
#include "Services/GitHubCopilotUEConsoleCommands.h"
#include "Services/GitHubCopilotUEConsoleExecutor.h"
#include "Services/GitHubCopilotUEToolExecutor.h"
#include "Widgets/SGitHubCopilotUEPanel.h"

#include "ToolMenus.h"
#include "Widgets/Docking/SDockTab.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Text/STextBlock.h"
#include "WorkspaceMenuStructure.h"
#include "WorkspaceMenuStructureModule.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "LevelEditor.h"

#define LOCTEXT_NAMESPACE "FGitHubCopilotUEModule"

const FName FGitHubCopilotUEModule::CopilotTabName(TEXT("GitHubCopilotUE"));

void FGitHubCopilotUEModule::StartupModule()
{
	UE_LOG(LogGitHubCopilotUE, Verbose, TEXT("GitHubCopilotUE: Starting up..."));

	// Initialize style and commands
	FGitHubCopilotUEStyle::Initialize();
	FGitHubCopilotUEStyle::ReloadTextures();
	FGitHubCopilotUECommands::Register();

	// Create command list and map actions
	PluginCommands = MakeShareable(new FUICommandList);
	PluginCommands->MapAction(
		FGitHubCopilotUECommands::Get().OpenPluginWindow,
		FExecuteAction::CreateRaw(this, &FGitHubCopilotUEModule::PluginButtonClicked),
		FCanExecuteAction());

	// Initialize services before tab registration.
	// This prevents restored tabs from spawning with null service pointers.
	InitializeServices();

	// Register menus via UToolMenus (deferred to ensure ToolMenus is ready)
	UToolMenus::RegisterStartupCallback(FSimpleMulticastDelegate::FDelegate::CreateRaw(this, &FGitHubCopilotUEModule::RegisterMenus));

	// Register the tab spawner
	RegisterTabSpawner();

	UE_LOG(LogGitHubCopilotUE, Verbose, TEXT("GitHubCopilotUE: Startup complete"));
}

void FGitHubCopilotUEModule::ShutdownModule()
{
	UE_LOG(LogGitHubCopilotUE, Verbose, TEXT("GitHubCopilotUE: Shutting down..."));

	// Cleanup
	UToolMenus::UnRegisterStartupCallback(this);
	UToolMenus::UnregisterOwner(this);

	FGitHubCopilotUECommands::Unregister();
	FGitHubCopilotUEStyle::Shutdown();

	UnregisterTabSpawner();
	ShutdownServices();

	UE_LOG(LogGitHubCopilotUE, Verbose, TEXT("GitHubCopilotUE: Shutdown complete"));
}

FGitHubCopilotUEModule& FGitHubCopilotUEModule::Get()
{
	return FModuleManager::LoadModuleChecked<FGitHubCopilotUEModule>("GitHubCopilotUE");
}

void FGitHubCopilotUEModule::RegisterTabSpawner()
{
	FGlobalTabmanager::Get()->RegisterNomadTabSpawner(
		CopilotTabName,
		FOnSpawnTab::CreateRaw(this, &FGitHubCopilotUEModule::OnSpawnPluginTab))
		.SetDisplayName(LOCTEXT("TabTitle", "GitHub Copilot"))
		.SetMenuType(ETabSpawnerMenuType::Hidden)
		.SetIcon(FSlateIcon(FGitHubCopilotUEStyle::GetStyleSetName(), "GitHubCopilotUE.OpenPluginWindow.Small"))
		.SetGroup(WorkspaceMenu::GetMenuStructure().GetToolsCategory());

	UE_LOG(LogGitHubCopilotUE, Verbose, TEXT("GitHubCopilotUE: Tab spawner registered"));
}

void FGitHubCopilotUEModule::UnregisterTabSpawner()
{
	FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(CopilotTabName);
}

TSharedRef<SDockTab> FGitHubCopilotUEModule::OnSpawnPluginTab(const FSpawnTabArgs& SpawnTabArgs)
{
	UE_LOG(LogGitHubCopilotUE, Verbose, TEXT("GitHubCopilotUE: Spawning tab..."));

	if (!CommandRouter.IsValid() || !ContextService.IsValid() || !BridgeService.IsValid() || !PatchService.IsValid())
	{
		UE_LOG(LogGitHubCopilotUE, Warning, TEXT("GitHubCopilotUE: Services were not ready at tab spawn. Reinitializing now."));
		InitializeServices();
	}

	return SNew(SDockTab)
		.TabRole(ETabRole::NomadTab)
		[
			SNew(SGitHubCopilotUEPanel)
			.CommandRouter(CommandRouter)
			.ContextService(ContextService)
			.BridgeService(BridgeService)
			.PatchService(PatchService)
		];
}

void FGitHubCopilotUEModule::RegisterMenus()
{
	// Owner for menu cleanup
	FToolMenuOwnerScoped OwnerScoped(this);

	// Add to the main Tools menu
	{
		UToolMenu* Menu = UToolMenus::Get()->ExtendMenu("LevelEditor.MainMenu.Tools");
		FToolMenuSection& Section = Menu->FindOrAddSection("GitHubCopilot");
		Section.AddMenuEntryWithCommandList(
			FGitHubCopilotUECommands::Get().OpenPluginWindow,
			PluginCommands);
	}

	// Add toolbar button
	{
		UToolMenu* ToolbarMenu = UToolMenus::Get()->ExtendMenu("LevelEditor.LevelEditorToolBar.PlayToolBar");
		FToolMenuSection& Section = ToolbarMenu->FindOrAddSection("GitHubCopilot");

		FToolMenuEntry& Entry = Section.AddEntry(FToolMenuEntry::InitToolBarButton(FGitHubCopilotUECommands::Get().OpenPluginWindow));
		Entry.SetCommandList(PluginCommands);
	}

	UE_LOG(LogGitHubCopilotUE, Verbose, TEXT("GitHubCopilotUE: Menus registered"));
}

void FGitHubCopilotUEModule::InitializeServices()
{
	// Create services
	ContextService = MakeShareable(new FGitHubCopilotUEContextService());
	FileService = MakeShareable(new FGitHubCopilotUEFileService());
	PatchService = MakeShareable(new FGitHubCopilotUEPatchService());
	BridgeService = MakeShareable(new FGitHubCopilotUEBridgeService());
	CompileService = MakeShareable(new FGitHubCopilotUECompileService());
	QuestService = MakeShareable(new FGitHubCopilotUEQuestService());
	CommandRouter = MakeShareable(new FGitHubCopilotUECommandRouter());

	// Wire PatchService to use the shared FileService
	PatchService->SetFileService(FileService);

	// Initialize bridge
	BridgeService->Initialize();

	// Create tool executor and give it to the bridge (makes AI agentic)
	TSharedPtr<FGitHubCopilotUEToolExecutor> ToolExecutor = MakeShareable(new FGitHubCopilotUEToolExecutor());
	ToolExecutor->Initialize(FileService, ContextService, CompileService);
	BridgeService->SetToolExecutor(ToolExecutor);

	// Initialize command router with all services
	CommandRouter->Initialize(
		ContextService,
		FileService,
		PatchService,
		BridgeService,
		CompileService,
		QuestService
	);

	// Create and initialize slash command system
	SlashCommands = MakeShareable(new FGitHubCopilotUESlashCommands());
	SlashCommands->Initialize(CommandRouter, BridgeService, ContextService, FileService);

	// Wire slash command AI prompts to the command router
	SlashCommands->OnSendPrompt.BindLambda([this](ECopilotCommandType Type, const FString& Prompt)
	{
		if (CommandRouter.IsValid())
		{
			FCopilotRequest Request;
			Request.RequestId = FGuid::NewGuid().ToString();
			Request.CommandType = Type;
			Request.UserPrompt = Prompt;
			Request.Timestamp = FDateTime::Now().ToString();
			if (ContextService.IsValid())
			{
				Request.ProjectContext = ContextService->GatherProjectContext();
			}
			CommandRouter->RouteCommand(Request);
		}
	});

	// Create and initialize console commands (the CLI-in-UE experience)
	ConsoleCommands = MakeShareable(new FGitHubCopilotUEConsoleCommands());
	ConsoleCommands->Initialize(
		BridgeService,
		CommandRouter,
		ContextService,
		FileService,
		CompileService,
		QuestService,
		PatchService,
		SlashCommands
	);

	// Create the Copilot console executor (REPL mode in Output Log dropdown)
	ConsoleExecutor = MakeShareable(new FGitHubCopilotUEConsoleExecutor());
	ConsoleExecutor->Initialize(BridgeService, CommandRouter, ContextService, SlashCommands, ConsoleCommands);

	UE_LOG(LogGitHubCopilotUE, Verbose, TEXT("GitHubCopilotUE: All services initialized. Select 'Copilot' in Output Log dropdown, or type ai <question> in console."));
}

void FGitHubCopilotUEModule::ShutdownServices()
{
	if (ConsoleExecutor.IsValid())
	{
		ConsoleExecutor->Shutdown();
	}

	if (ConsoleCommands.IsValid())
	{
		ConsoleCommands->Shutdown();
	}

	if (BridgeService.IsValid())
	{
		BridgeService->Shutdown();
	}

	ConsoleExecutor.Reset();
	ConsoleCommands.Reset();
	SlashCommands.Reset();
	CommandRouter.Reset();
	BridgeService.Reset();
	CompileService.Reset();
	QuestService.Reset();
	PatchService.Reset();
	FileService.Reset();
	ContextService.Reset();

	UE_LOG(LogGitHubCopilotUE, Verbose, TEXT("GitHubCopilotUE: All services shut down"));
}

void FGitHubCopilotUEModule::PluginButtonClicked()
{
	FGlobalTabmanager::Get()->TryInvokeTab(CopilotTabName);
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FGitHubCopilotUEModule, GitHubCopilotUE)
