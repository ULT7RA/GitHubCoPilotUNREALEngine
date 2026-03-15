// Copyright GitHub, Inc. All Rights Reserved.

#include "Services/GitHubCopilotUEConsoleExecutor.h"
#include "Services/GitHubCopilotUEBridgeService.h"
#include "Services/GitHubCopilotUESlashCommands.h"
#include "Services/GitHubCopilotUEConsoleCommands.h"
#include "Services/GitHubCopilotUECommandRouter.h"
#include "Services/GitHubCopilotUEContextService.h"
#include "Services/GitHubCopilotUETypes.h"
#include "Features/IModularFeatures.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Editor.h"
#include "Framework/Commands/InputChord.h"

// The OutputLog module exposes IConsoleCommandExecutor, which is how
// "Cmd", "Python", "Python (REPL)" appear in the dropdown.
#include "OutputLogModule.h"

// ============================================================================
// FExecutorImpl — the real IConsoleCommandExecutor that registers with UE
// ============================================================================

class FGitHubCopilotUEConsoleExecutor::FExecutorImpl : public IConsoleCommandExecutor
{
public:
	FExecutorImpl() {}
	virtual ~FExecutorImpl() {}

	// Service references (set by owner)
	TSharedPtr<FGitHubCopilotUEBridgeService> BridgeService;
	TSharedPtr<FGitHubCopilotUECommandRouter> CommandRouter;
	TSharedPtr<FGitHubCopilotUEContextService> ContextService;
	TSharedPtr<FGitHubCopilotUESlashCommands> SlashCommands;
	TSharedPtr<FGitHubCopilotUEConsoleCommands> ConsoleCommands;
	TArray<FString> History;

	// ── IConsoleCommandExecutor interface ──

	virtual FName GetName() const override
	{
		static const FName Name(TEXT("Copilot"));
		return Name;
	}

	virtual FText GetDisplayName() const override
	{
		return FText::FromString(TEXT("Copilot"));
	}

	virtual FText GetDescription() const override
	{
		return FText::FromString(TEXT("GitHub Copilot AI assistant. Type anything to chat, or use /commands."));
	}

	virtual FText GetHintText() const override
	{
		return FText::FromString(TEXT("Ask Copilot anything, or type /help for commands..."));
	}

	virtual void GetSuggestedCompletions(const TCHAR* Input, TArray<FConsoleSuggestion>& Out) override
	{
		FString InputStr(Input);

		if (InputStr.StartsWith(TEXT("/")))
		{
			// Slash command autocomplete
			if (SlashCommands.IsValid())
			{
				const TArray<FCopilotSlashCommand>& AllCmds = SlashCommands->GetAllCommands();
				for (const FCopilotSlashCommand& Cmd : AllCmds)
				{
					FString Full = TEXT("/") + Cmd.Name;
					if (Full.StartsWith(InputStr))
					{
						Out.Add(FConsoleSuggestion(Full, Cmd.Description));
					}
					for (const FString& Alias : Cmd.Aliases)
					{
						FString FullAlias = TEXT("/") + Alias;
						if (FullAlias.StartsWith(InputStr))
						{
							Out.Add(FConsoleSuggestion(FullAlias, FString::Printf(TEXT("Alias for /%s"), *Cmd.Name)));
						}
					}
				}
			}
		}
		else if (InputStr.Len() < 2)
		{
			Out.Add(FConsoleSuggestion(TEXT("/help"), TEXT("Show all available commands")));
			Out.Add(FConsoleSuggestion(TEXT("/model"), TEXT("Select AI model")));
			Out.Add(FConsoleSuggestion(TEXT("/knowledge"), TEXT("Scan project and generate project.md")));
			Out.Add(FConsoleSuggestion(TEXT("/context"), TEXT("Show project context")));
			Out.Add(FConsoleSuggestion(TEXT("/login"), TEXT("Log in to GitHub Copilot")));
		}
	}

	virtual void GetExecHistory(TArray<FString>& Out) override
	{
		Out = History;
	}

	virtual bool Exec(const TCHAR* Input) override
	{
		FString InputStr(Input);
		InputStr.TrimStartAndEndInline();

		if (InputStr.IsEmpty())
		{
			return true;
		}

		// Add to history
		History.Insert(InputStr, 0);
		if (History.Num() > 100)
		{
			History.SetNum(100);
		}

		// Echo input
		UE_LOG(LogGitHubCopilotUE, Display, TEXT("> %s"), *InputStr);

		// ── Slash commands ──
		if (InputStr.StartsWith(TEXT("/")))
		{
			if (SlashCommands.IsValid())
			{
				FString Result;
				if (SlashCommands->ExecuteSlashCommand(InputStr, Result))
				{
					if (!Result.IsEmpty())
					{
						TArray<FString> Lines;
						Result.ParseIntoArrayLines(Lines);
						for (const FString& Line : Lines)
						{
							UE_LOG(LogGitHubCopilotUE, Display, TEXT("  %s"), *Line);
						}
					}
				}
				else
				{
					UE_LOG(LogGitHubCopilotUE, Display, TEXT("  Unknown command: %s. Type /help for available commands."), *InputStr);
				}
			}
			else
			{
				UE_LOG(LogGitHubCopilotUE, Warning, TEXT("SlashCommands not available"));
			}
			return true;
		}

		// ── Copilot.* commands pass to engine ──
		if (InputStr.StartsWith(TEXT("Copilot.")) || InputStr.StartsWith(TEXT("copilot.")))
		{
			GEngine->Exec(GEditor ? GEditor->GetEditorWorldContext().World() : nullptr, *InputStr);
			return true;
		}

		// ── Everything else → AI chat ──
		if (!CommandRouter.IsValid() || !BridgeService.IsValid())
		{
			UE_LOG(LogGitHubCopilotUE, Display, TEXT("  Not connected. Use /login first."));
			return true;
		}

		FCopilotRequest Request;
		Request.RequestId = FGuid::NewGuid().ToString();
		Request.CommandType = ECopilotCommandType::Ask;
		Request.UserPrompt = InputStr;
		Request.Timestamp = FDateTime::Now().ToString();

		if (ContextService.IsValid())
		{
			Request.ProjectContext = ContextService->GatherProjectContext();
		}

		CommandRouter->RouteCommand(Request);
		return true;
	}

	virtual bool AllowHotKeyClose() const override
	{
		return true;
	}

	virtual bool AllowMultiLine() const override
	{
		return true;
	}

	virtual FInputChord GetHotKey() const override
	{
		// No dedicated hotkey to switch to Copilot mode
		return FInputChord();
	}

	virtual FInputChord GetIterateExecutorHotKey() const override
	{
		// No hotkey to cycle executors
		return FInputChord();
	}
};

// ============================================================================
// FGitHubCopilotUEConsoleExecutor — outer wrapper
// ============================================================================

FGitHubCopilotUEConsoleExecutor::FGitHubCopilotUEConsoleExecutor()
{
}

FGitHubCopilotUEConsoleExecutor::~FGitHubCopilotUEConsoleExecutor()
{
	Shutdown();
}

void FGitHubCopilotUEConsoleExecutor::Initialize(
	TSharedPtr<FGitHubCopilotUEBridgeService> InBridgeService,
	TSharedPtr<FGitHubCopilotUECommandRouter> InCommandRouter,
	TSharedPtr<FGitHubCopilotUEContextService> InContextService,
	TSharedPtr<FGitHubCopilotUESlashCommands> InSlashCommands,
	TSharedPtr<FGitHubCopilotUEConsoleCommands> InConsoleCommands)
{
	Impl = MakeShareable(new FExecutorImpl());
	Impl->BridgeService = InBridgeService;
	Impl->CommandRouter = InCommandRouter;
	Impl->ContextService = InContextService;
	Impl->SlashCommands = InSlashCommands;
	Impl->ConsoleCommands = InConsoleCommands;

	// Register with UE's modular features — this makes "Copilot" appear in the dropdown
	IModularFeatures::Get().RegisterModularFeature(
		IConsoleCommandExecutor::ModularFeatureName(), Impl.Get());
	bRegistered = true;

	UE_LOG(LogGitHubCopilotUE, Verbose, TEXT("ConsoleExecutor: Registered 'Copilot' in Output Log dropdown"));
}

void FGitHubCopilotUEConsoleExecutor::Shutdown()
{
	if (bRegistered && Impl.IsValid())
	{
		IModularFeatures::Get().UnregisterModularFeature(
			IConsoleCommandExecutor::ModularFeatureName(), Impl.Get());
		bRegistered = false;
		UE_LOG(LogGitHubCopilotUE, Verbose, TEXT("ConsoleExecutor: Unregistered from Output Log dropdown"));
	}
	Impl.Reset();
}
