// Copyright GitHub, Inc. All Rights Reserved.

#include "Widgets/SGitHubCopilotUEPanel.h"
#include "Services/GitHubCopilotUECommandRouter.h"
#include "Services/GitHubCopilotUEContextService.h"
#include "Services/GitHubCopilotUEBridgeService.h"
#include "Services/GitHubCopilotUEPatchService.h"
#include "GitHubCopilotUESettings.h"
#include "GitHubCopilotUEModule.h"
#include "Services/GitHubCopilotUESlashCommands.h"
#include "Widgets/Input/SMultiLineEditableTextBox.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SSplitter.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SSeparator.h"
#include "Widgets/Layout/SExpandableArea.h"
#include "Widgets/Text/STextBlock.h"
#include "HAL/PlatformApplicationMisc.h"
#include "Widgets/Input/SComboBox.h"
#include "Misc/DateTime.h"
#include "Async/Async.h"

#define LOCTEXT_NAMESPACE "SGitHubCopilotUEPanel"

void SGitHubCopilotUEPanel::Construct(const FArguments& InArgs)
{
	CommandRouter = InArgs._CommandRouter;
	ContextService = InArgs._ContextService;
	BridgeService = InArgs._BridgeService;
	PatchService = InArgs._PatchService;

	// Wire up delegates — store handles for safe cleanup in destructor
	if (CommandRouter.IsValid())
	{
		ResponseDelegateHandle = CommandRouter->OnResponseReceived.AddRaw(this, &SGitHubCopilotUEPanel::OnResponseReceived);
		RouterLogDelegateHandle = CommandRouter->OnLogMessage.AddRaw(this, &SGitHubCopilotUEPanel::OnLogMessageReceived);
	}

	if (BridgeService.IsValid())
	{
		ConnectionStatusDelegateHandle = BridgeService->OnConnectionStatusChanged.AddRaw(this, &SGitHubCopilotUEPanel::OnConnectionStatusChanged);
		BridgeLogDelegateHandle = BridgeService->OnLogMessage.AddRaw(this, &SGitHubCopilotUEPanel::OnLogMessageReceived);
	}

	ChildSlot
	[
		SNew(SVerticalBox)

		// === Status Bar ===
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(4)
		[
			BuildStatusBar()
		]

		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			SNew(SSeparator)
		]

		// === Main content area (scrollable) ===
		+ SVerticalBox::Slot()
		.FillHeight(1.0f)
		.Padding(4)
		[
			SNew(SScrollBox)

			// Project Context Box
			+ SScrollBox::Slot()
			.Padding(2)
			[
				BuildProjectContextBox()
			]

			// VR Context Box
			+ SScrollBox::Slot()
			.Padding(2)
			[
				BuildVRContextBox()
			]

			+ SScrollBox::Slot()
			.Padding(2)
			[
				SNew(SSeparator)
			]

			// Prompt Input
			+ SScrollBox::Slot()
			.Padding(2)
			[
				BuildPromptInput()
			]

			// Action Buttons
			+ SScrollBox::Slot()
			.Padding(2)
			[
				BuildActionButtons()
			]

			+ SScrollBox::Slot()
			.Padding(2)
			[
				SNew(SSeparator)
			]

			// Response Output
			+ SScrollBox::Slot()
			.Padding(2)
			[
				BuildResponseArea()
			]

			// Diff Preview
			+ SScrollBox::Slot()
			.Padding(2)
			[
				BuildDiffPreviewArea()
			]
		]

		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			SNew(SSeparator)
		]

		// === Execution Log Footer ===
		+ SVerticalBox::Slot()
		.AutoHeight()
		.MaxHeight(150)
		.Padding(4)
		[
			BuildExecutionLog()
		]
	];

	// Wire auth delegates
	if (BridgeService.IsValid())
	{
		DeviceCodeDelegateHandle = BridgeService->OnDeviceCodeReceived.AddRaw(this, &SGitHubCopilotUEPanel::OnDeviceCodeReceived);
		AuthCompleteDelegateHandle = BridgeService->OnAuthComplete.AddRaw(this, &SGitHubCopilotUEPanel::OnAuthComplete);
		ModelsLoadedDelegateHandle = BridgeService->OnModelsLoaded.AddRaw(this, &SGitHubCopilotUEPanel::OnModelsLoaded);
	}

	// Refresh context on load
	OnRefreshContext();
	AppendToLog(TEXT("GitHub Copilot UE panel initialized"));

	// Update auth status
	if (BridgeService.IsValid() && BridgeService->IsAuthenticated())
	{
		AppendToLog(FString::Printf(TEXT("Signed in as: %s"), *BridgeService->GetUsername()));
	}
}

SGitHubCopilotUEPanel::~SGitHubCopilotUEPanel()
{
	// Remove delegate bindings to prevent dangling pointer callbacks
	if (CommandRouter.IsValid())
	{
		CommandRouter->OnResponseReceived.Remove(ResponseDelegateHandle);
		CommandRouter->OnLogMessage.Remove(RouterLogDelegateHandle);
	}

	if (BridgeService.IsValid())
	{
		BridgeService->OnConnectionStatusChanged.Remove(ConnectionStatusDelegateHandle);
		BridgeService->OnLogMessage.Remove(BridgeLogDelegateHandle);
		BridgeService->OnDeviceCodeReceived.Remove(DeviceCodeDelegateHandle);
		BridgeService->OnAuthComplete.Remove(AuthCompleteDelegateHandle);
		BridgeService->OnModelsLoaded.Remove(ModelsLoadedDelegateHandle);
	}
}

// ============================================================================
// UI Construction
// ============================================================================

TSharedRef<SWidget> SGitHubCopilotUEPanel::BuildStatusBar()
{
	return SNew(SVerticalBox)

		// Row 1: Title + Connection + Auth
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(0, 2)
		[
			SNew(SHorizontalBox)

			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(4, 0)
			[
				SNew(STextBlock)
				.Text(LOCTEXT("StatusLabel", "GitHub Copilot UE"))
				.Font(FCoreStyle::GetDefaultFontStyle("Bold", 12))
			]

			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(8, 0)
			[
				SAssignNew(StatusText, STextBlock)
				.Text(this, &SGitHubCopilotUEPanel::GetConnectionStatusText)
				.ColorAndOpacity(this, &SGitHubCopilotUEPanel::GetConnectionStatusColor)
			]

			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			[
				SNullWidget::NullWidget
			]

			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(4, 0)
			[
				SNew(SButton)
				.Text(LOCTEXT("SignInBtn", "Sign in with GitHub"))
				.ToolTipText(LOCTEXT("SignInTip", "Authenticate with your GitHub account to use Copilot"))
				.OnClicked_Lambda([this]() -> FReply { OnSignIn(); return FReply::Handled(); })
			]

			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(2, 0)
			[
				SNew(SButton)
				.Text(LOCTEXT("SignOutBtn", "Sign Out"))
				.OnClicked_Lambda([this]() -> FReply { OnSignOut(); return FReply::Handled(); })
			]

			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(4, 0)
			[
				SNew(SButton)
				.Text(LOCTEXT("RefreshBtn", "Refresh"))
				.OnClicked_Lambda([this]() -> FReply { OnRefreshContext(); return FReply::Handled(); })
			]
		]

		// Row 2: Auth status + Device code + Model picker
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(4, 2)
		[
			SNew(SHorizontalBox)

			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(0, 0, 8, 0)
			[
				SAssignNew(AuthStatusText, STextBlock)
				.Text(LOCTEXT("AuthNotSignedIn", "Not signed in"))
			]

			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(0, 0, 8, 0)
			[
				SAssignNew(DeviceCodeText, STextBlock)
				.Text(FText::GetEmpty())
				.Font(FCoreStyle::GetDefaultFontStyle("Bold", 11))
				.ColorAndOpacity(FSlateColor(FLinearColor::Yellow))
			]

			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			[
				SNullWidget::NullWidget
			]

			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(4, 0)
			[
				SNew(STextBlock)
				.Text(LOCTEXT("ModelLabel", "Model:"))
			]

			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			[
				SAssignNew(ModelComboBox, SComboBox<TSharedPtr<FString>>)
				.OptionsSource(&ModelOptions)
				.OnSelectionChanged(this, &SGitHubCopilotUEPanel::OnModelSelected)
				.OnGenerateWidget(this, &SGitHubCopilotUEPanel::MakeModelComboRow)
				.Content()
				[
					SNew(STextBlock)
					.Text_Lambda([this]() -> FText
					{
						return SelectedModelOption.IsValid()
							? FText::FromString(*SelectedModelOption)
							: LOCTEXT("NoModel", "Sign in to load models");
					})
				]
			]
		];
}

TSharedRef<SWidget> SGitHubCopilotUEPanel::BuildProjectContextBox()
{
	return SNew(SExpandableArea)
		.AreaTitle(LOCTEXT("ProjectContext", "Project Context"))
		.InitiallyCollapsed(false)
		.BodyContent()
		[
			SAssignNew(ProjectContextText, STextBlock)
			.Text(LOCTEXT("ProjectContextLoading", "Loading..."))
			.AutoWrapText(true)
		];
}

TSharedRef<SWidget> SGitHubCopilotUEPanel::BuildVRContextBox()
{
	return SNew(SExpandableArea)
		.AreaTitle(LOCTEXT("VRContext", "VR / Quest Context"))
		.InitiallyCollapsed(true)
		.BodyContent()
		[
			SAssignNew(VRContextText, STextBlock)
			.Text(LOCTEXT("VRContextLoading", "Click 'Analyze VR Setup' to load"))
			.AutoWrapText(true)
		];
}

TSharedRef<SWidget> SGitHubCopilotUEPanel::BuildPromptInput()
{
	return SNew(SVerticalBox)
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(0, 2)
		[
			SNew(STextBlock)
			.Text(LOCTEXT("PromptLabel", "Prompt / Command Input:"))
			.Font(FCoreStyle::GetDefaultFontStyle("Bold", 10))
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			SNew(SBox)
			.MinDesiredHeight(80)
			.MaxDesiredHeight(200)
			[
				SAssignNew(PromptTextBox, SMultiLineEditableTextBox)
				.HintText(LOCTEXT("PromptHint", "Type a prompt or / for commands (e.g., /help, /login, /model)..."))
				.AutoWrapText(true)
			]
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(0, 4)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			[
				SNew(SButton)
				.Text(LOCTEXT("SendBtn", "Send"))
				.OnClicked_Lambda([this]() -> FReply { OnSendPrompt(); return FReply::Handled(); })
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.Padding(4, 0)
			[
				SNew(SButton)
				.Text(LOCTEXT("ClearBtn", "Clear"))
				.OnClicked_Lambda([this]() -> FReply { OnClearAll(); return FReply::Handled(); })
			]
		];
}

TSharedRef<SWidget> SGitHubCopilotUEPanel::BuildActionButtons()
{
	// Helper lambda to create action buttons
	auto MakeButton = [this](const FText& Label, ECopilotCommandType CmdType) -> TSharedRef<SWidget>
	{
		return SNew(SButton)
			.Text(Label)
			.OnClicked_Lambda([this, CmdType]() -> FReply { OnActionButton(CmdType); return FReply::Handled(); });
	};

	return SNew(SExpandableArea)
		.AreaTitle(LOCTEXT("Actions", "Actions"))
		.InitiallyCollapsed(false)
		.BodyContent()
		[
			SNew(SVerticalBox)

			// Row 1: Analysis
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0, 2)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().Padding(2, 0)
				[ MakeButton(LOCTEXT("AnalyzeSelectionBtn", "Analyze Selection"), ECopilotCommandType::AnalyzeSelection) ]
				+ SHorizontalBox::Slot().AutoWidth().Padding(2, 0)
				[ MakeButton(LOCTEXT("ExplainCodeBtn", "Explain Code"), ECopilotCommandType::ExplainCode) ]
				+ SHorizontalBox::Slot().AutoWidth().Padding(2, 0)
				[ MakeButton(LOCTEXT("SuggestRefactorBtn", "Suggest Refactor"), ECopilotCommandType::SuggestRefactor) ]
				+ SHorizontalBox::Slot().AutoWidth().Padding(2, 0)
				[ MakeButton(LOCTEXT("AnalyzeProjectBtn", "Analyze Project"), ECopilotCommandType::AnalyzeProject) ]
			]

			// Row 2: Generation
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0, 2)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().Padding(2, 0)
				[ MakeButton(LOCTEXT("GenCppClassBtn", "Generate C++ Class"), ECopilotCommandType::CreateCppClass) ]
				+ SHorizontalBox::Slot().AutoWidth().Padding(2, 0)
				[ MakeButton(LOCTEXT("GenActorCompBtn", "Generate Actor Component"), ECopilotCommandType::CreateActorComponent) ]
				+ SHorizontalBox::Slot().AutoWidth().Padding(2, 0)
				[ MakeButton(LOCTEXT("GenBPFuncLibBtn", "Generate BP Func Library"), ECopilotCommandType::CreateBlueprintFunctionLibrary) ]
				+ SHorizontalBox::Slot().AutoWidth().Padding(2, 0)
				[ MakeButton(LOCTEXT("GenEdUtilBtn", "Generate Editor Utility"), ECopilotCommandType::GenerateEditorUtilityHelper) ]
			]

			// Row 3: File operations
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0, 2)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().Padding(2, 0)
				[ MakeButton(LOCTEXT("PreviewPatchBtn", "Preview Patch"), ECopilotCommandType::PatchFile) ]
				+ SHorizontalBox::Slot().AutoWidth().Padding(2, 0)
				[
					SNew(SButton)
					.Text(LOCTEXT("ApplyPatchBtn", "Apply Patch"))
					.ToolTipText(LOCTEXT("ApplyPatchTip", "Approve and apply the pending diff preview"))
					.OnClicked_Lambda([this]() -> FReply { OnApplyPatch(); return FReply::Handled(); })
				]
				+ SHorizontalBox::Slot().AutoWidth().Padding(2, 0)
				[
					SNew(SButton)
					.Text(LOCTEXT("RollbackBtn", "Rollback"))
					.ToolTipText(LOCTEXT("RollbackTip", "Restore the last patched file from its backup"))
					.OnClicked_Lambda([this]() -> FReply { OnRollbackPatch(); return FReply::Handled(); })
				]
				+ SHorizontalBox::Slot().AutoWidth().Padding(2, 0)
				[ MakeButton(LOCTEXT("InsertFileBtn", "Insert Into Active File"), ECopilotCommandType::InsertIntoFile) ]
				+ SHorizontalBox::Slot().AutoWidth().Padding(2, 0)
				[ MakeButton(LOCTEXT("OpenRelatedBtn", "Open Related File"), ECopilotCommandType::OpenFile) ]
			]

			// Row 4: Compile & Test
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0, 2)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().Padding(2, 0)
				[ MakeButton(LOCTEXT("CompileBtn", "Trigger Compile"), ECopilotCommandType::TriggerCompile) ]
				+ SHorizontalBox::Slot().AutoWidth().Padding(2, 0)
				[ MakeButton(LOCTEXT("LiveCodingBtn", "Trigger Live Coding"), ECopilotCommandType::TriggerLiveCoding) ]
				+ SHorizontalBox::Slot().AutoWidth().Padding(2, 0)
				[ MakeButton(LOCTEXT("RunTestsBtn", "Run Automation Tests"), ECopilotCommandType::RunAutomationTests) ]
			]

			// Row 5: VR/Quest
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0, 2)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().Padding(2, 0)
				[ MakeButton(LOCTEXT("AnalyzeVRBtn", "Analyze VR Setup"), ECopilotCommandType::GatherVRContext) ]
				+ SHorizontalBox::Slot().AutoWidth().Padding(2, 0)
				[ MakeButton(LOCTEXT("QuestAuditBtn", "Analyze Meta Quest Readiness"), ECopilotCommandType::RunQuestAudit) ]
				+ SHorizontalBox::Slot().AutoWidth().Padding(2, 0)
				[
					SNew(SButton)
					.Text(LOCTEXT("CopyResponseBtn", "Copy Response"))
					.OnClicked_Lambda([this]() -> FReply { OnCopyResponse(); return FReply::Handled(); })
				]
			]
		];
}

TSharedRef<SWidget> SGitHubCopilotUEPanel::BuildResponseArea()
{
	return SNew(SExpandableArea)
		.AreaTitle(LOCTEXT("ResponseArea", "Response / Output"))
		.InitiallyCollapsed(false)
		.BodyContent()
		[
			SNew(SBox)
			.MinDesiredHeight(150.0f)
			.MaxDesiredHeight(400.0f)
			[
				SAssignNew(ResponseTextBox, SMultiLineEditableTextBox)
				.IsReadOnly(true)
				.AutoWrapText(true)
				.HintText(LOCTEXT("ResponseHint", "AI responses and command output will appear here..."))
			]
		];
}

TSharedRef<SWidget> SGitHubCopilotUEPanel::BuildDiffPreviewArea()
{
	return SNew(SExpandableArea)
		.AreaTitle(LOCTEXT("DiffPreview", "Diff Preview"))
		.InitiallyCollapsed(true)
		.BodyContent()
		[
			SNew(SBox)
			.MinDesiredHeight(100.0f)
			.MaxDesiredHeight(300.0f)
			[
				SAssignNew(DiffPreviewTextBox, SMultiLineEditableTextBox)
				.IsReadOnly(true)
				.AutoWrapText(false)
				.HintText(LOCTEXT("DiffHint", "Diff previews will appear here..."))
			]
		];
}

TSharedRef<SWidget> SGitHubCopilotUEPanel::BuildExecutionLog()
{
	return SNew(SExpandableArea)
		.AreaTitle(LOCTEXT("ExecLog", "Execution Log"))
		.InitiallyCollapsed(false)
		.Padding(FMargin(2.0f))
		.BodyContent()
		[
			SNew(SBox)
			.MaxDesiredHeight(120.0f)
			[
				SAssignNew(LogTextBox, SMultiLineEditableTextBox)
				.IsReadOnly(true)
				.AutoWrapText(true)
			]
		];
}

// ============================================================================
// Action Handlers
// ============================================================================

void SGitHubCopilotUEPanel::OnSendPrompt()
{
	if (!PromptTextBox.IsValid() || !CommandRouter.IsValid()) return;

	FString Prompt = PromptTextBox->GetText().ToString();
	if (Prompt.IsEmpty())
	{
		AppendToLog(TEXT("Cannot send empty prompt"));
		return;
	}

	// Check for slash commands first (e.g., /help, /login, /model)
	if (Prompt.TrimStartAndEnd().StartsWith(TEXT("/")))
	{
		// Route through the module's slash command system
		FGitHubCopilotUEModule& Module = FModuleManager::LoadModuleChecked<FGitHubCopilotUEModule>("GitHubCopilotUE");
		TSharedPtr<FGitHubCopilotUESlashCommands> SlashCmds = Module.GetSlashCommands();
		if (SlashCmds.IsValid())
		{
			FString Response;
			if (SlashCmds->ExecuteSlashCommand(Prompt.TrimStartAndEnd(), Response))
			{
				SetResponseText(Response);
				AppendToLog(FString::Printf(TEXT("Executed: %s"), *Prompt.TrimStartAndEnd()));
				PromptTextBox->SetText(FText::GetEmpty());
				return;
			}
		}
	}

	// Default to AnalyzeSelection for free-form prompts sent to backend
	FCopilotRequest Request = BuildRequest(ECopilotCommandType::AnalyzeSelection);
	Request.UserPrompt = Prompt;

	AppendToLog(FString::Printf(TEXT("Sending prompt (ID: %s)..."), *Request.RequestId));
	CommandRouter->RouteCommand(Request);

	// Clear the prompt
	PromptTextBox->SetText(FText::GetEmpty());
}

void SGitHubCopilotUEPanel::OnActionButton(ECopilotCommandType CommandType)
{
	if (!CommandRouter.IsValid()) return;

	FCopilotRequest Request = BuildRequest(CommandType);

	// Get prompt text as additional context
	if (PromptTextBox.IsValid())
	{
		Request.UserPrompt = PromptTextBox->GetText().ToString();
	}

	AppendToLog(FString::Printf(TEXT("Executing command %d (ID: %s)..."), (int32)CommandType, *Request.RequestId));
	CommandRouter->RouteCommand(Request);
}

void SGitHubCopilotUEPanel::OnCopyResponse()
{
	if (ResponseTextBox.IsValid())
	{
		FString ResponseText = ResponseTextBox->GetText().ToString();
		if (!ResponseText.IsEmpty())
		{
			FPlatformApplicationMisc::ClipboardCopy(*ResponseText);
			AppendToLog(TEXT("Response copied to clipboard"));
		}
	}
}

void SGitHubCopilotUEPanel::OnClearAll()
{
	if (PromptTextBox.IsValid())
		PromptTextBox->SetText(FText::GetEmpty());
	if (ResponseTextBox.IsValid())
		ResponseTextBox->SetText(FText::GetEmpty());
	if (DiffPreviewTextBox.IsValid())
		DiffPreviewTextBox->SetText(FText::GetEmpty());

	CurrentDiffPreview = FCopilotDiffPreview();
	AppendToLog(TEXT("Cleared all fields"));
}

void SGitHubCopilotUEPanel::OnApplyPatch()
{
	if (!CommandRouter.IsValid())
	{
		AppendToLog(TEXT("ERROR: Command router not available"));
		return;
	}

	if (!CurrentDiffPreview.bIsValid)
	{
		// Try using the approval command which will find the last pending preview
		AppendToLog(TEXT("No explicit diff preview in panel — routing approval through command router"));
	}

	// Route through the command router's approval flow for proper step tracking
	FCopilotRequest Request = BuildRequest(ECopilotCommandType::ApproveAndApplyPatch);
	if (CurrentDiffPreview.bIsValid)
	{
		FCopilotFileTarget Target;
		Target.FilePath = CurrentDiffPreview.OriginalFilePath;
		Request.FileTargets.Add(Target);
	}

	AppendToLog(TEXT("Approving and applying pending patch..."));
	CommandRouter->RouteCommand(Request);

	// Clear local preview state (the response handler will update the UI)
	CurrentDiffPreview = FCopilotDiffPreview();
	SetDiffText(TEXT(""));
}

void SGitHubCopilotUEPanel::OnRollbackPatch()
{
	if (!CommandRouter.IsValid())
	{
		AppendToLog(TEXT("ERROR: Command router not available"));
		return;
	}

	FCopilotRequest Request = BuildRequest(ECopilotCommandType::RollbackPatch);

	// If we have a current diff preview, target that file
	if (CurrentDiffPreview.bIsValid)
	{
		FCopilotFileTarget Target;
		Target.FilePath = CurrentDiffPreview.OriginalFilePath;
		Request.FileTargets.Add(Target);
	}
	else if (PromptTextBox.IsValid())
	{
		// Allow user to type a file path in the prompt for rollback
		Request.UserPrompt = PromptTextBox->GetText().ToString();
	}

	AppendToLog(TEXT("Requesting rollback..."));
	CommandRouter->RouteCommand(Request);

	CurrentDiffPreview = FCopilotDiffPreview();
	SetDiffText(TEXT(""));
}

void SGitHubCopilotUEPanel::OnSignIn()
{
	if (!BridgeService.IsValid())
	{
		AppendToLog(TEXT("ERROR: Bridge service not available"));
		return;
	}

	if (BridgeService->IsAuthenticated())
	{
		AppendToLog(TEXT("Already signed in. Sign out first to switch accounts."));
		return;
	}

	AppendToLog(TEXT("Starting GitHub authentication..."));
	BridgeService->StartDeviceCodeAuth();
}

void SGitHubCopilotUEPanel::OnSignOut()
{
	if (!BridgeService.IsValid()) return;

	BridgeService->SignOut();
	ModelOptions.Empty();
	SelectedModelOption.Reset();
	if (ModelComboBox.IsValid())
	{
		ModelComboBox->RefreshOptions();
	}
	if (AuthStatusText.IsValid())
	{
		AuthStatusText->SetText(LOCTEXT("AuthNotSignedIn2", "Not signed in"));
	}
	if (DeviceCodeText.IsValid())
	{
		DeviceCodeText->SetText(FText::GetEmpty());
	}
	AppendToLog(TEXT("Signed out"));
}

void SGitHubCopilotUEPanel::OnDeviceCodeReceived(const FString& UserCode, const FString& VerificationURI)
{
	AsyncTask(ENamedThreads::GameThread, [this, UserCode, VerificationURI]()
	{
		if (DeviceCodeText.IsValid())
		{
			DeviceCodeText->SetText(FText::FromString(FString::Printf(TEXT("Enter code: %s"), *UserCode)));
		}
		if (AuthStatusText.IsValid())
		{
			AuthStatusText->SetText(FText::FromString(TEXT("Waiting for browser login...")));
		}
		AppendToLog(FString::Printf(TEXT("Go to %s and enter: %s"), *VerificationURI, *UserCode));
	});
}

void SGitHubCopilotUEPanel::OnAuthComplete()
{
	AsyncTask(ENamedThreads::GameThread, [this]()
	{
		if (DeviceCodeText.IsValid())
		{
			DeviceCodeText->SetText(FText::GetEmpty());
		}
		if (AuthStatusText.IsValid() && BridgeService.IsValid())
		{
			FString User = BridgeService->GetUsername();
			AuthStatusText->SetText(FText::FromString(FString::Printf(TEXT("Signed in as: %s"), *User)));
		}
		AppendToLog(TEXT("Authentication successful! Loading models..."));
	});
}

void SGitHubCopilotUEPanel::OnModelsLoaded(const TArray<FCopilotModel>& Models)
{
	AsyncTask(ENamedThreads::GameThread, [this, Models]()
	{
		ModelOptions.Empty();
		for (const FCopilotModel& M : Models)
		{
			ModelOptions.Add(MakeShareable(new FString(M.Id)));
		}

		// Select the active model
		if (BridgeService.IsValid())
		{
			FString Active = BridgeService->GetActiveModel();
			for (const TSharedPtr<FString>& Opt : ModelOptions)
			{
				if (*Opt == Active)
				{
					SelectedModelOption = Opt;
					break;
				}
			}
		}

		if (ModelComboBox.IsValid())
		{
			ModelComboBox->RefreshOptions();
			if (SelectedModelOption.IsValid())
			{
				ModelComboBox->SetSelectedItem(SelectedModelOption);
			}
		}

		AppendToLog(FString::Printf(TEXT("Loaded %d models from your Copilot subscription"), Models.Num()));
	});
}

void SGitHubCopilotUEPanel::OnModelSelected(TSharedPtr<FString> NewModel, ESelectInfo::Type SelectInfo)
{
	if (NewModel.IsValid() && BridgeService.IsValid())
	{
		SelectedModelOption = NewModel;
		BridgeService->SetActiveModel(*NewModel);
		AppendToLog(FString::Printf(TEXT("Model changed to: %s"), **NewModel));
	}
}

TSharedRef<SWidget> SGitHubCopilotUEPanel::MakeModelComboRow(TSharedPtr<FString> Item)
{
	return SNew(STextBlock).Text(FText::FromString(Item.IsValid() ? *Item : TEXT("(none)")));
}

void SGitHubCopilotUEPanel::OnRefreshContext()
{
	if (!ContextService.IsValid()) return;

	FCopilotProjectContext Ctx = ContextService->GatherProjectContext();

	FString ContextStr;
	ContextStr += FString::Printf(TEXT("Project: %s | Engine: %s\n"), *Ctx.ProjectName, *Ctx.EngineVersion);
	ContextStr += FString::Printf(TEXT("Map: %s | Platform: %s\n"), *Ctx.CurrentMapName, *Ctx.ActivePlatform);
	ContextStr += FString::Printf(TEXT("Selected Assets: %d | Actors: %d\n"), Ctx.SelectedAssets.Num(), Ctx.SelectedActors.Num());
	ContextStr += FString::Printf(TEXT("Plugins: %d | XR Plugins: %d | Modules: %d"),
		Ctx.EnabledPlugins.Num(), Ctx.EnabledXRPlugins.Num(), Ctx.ModuleNames.Num());

	if (ProjectContextText.IsValid())
	{
		ProjectContextText->SetText(FText::FromString(ContextStr));
	}

	AppendToLog(TEXT("Context refreshed"));
}

// ============================================================================
// Event Handlers
// ============================================================================

void SGitHubCopilotUEPanel::OnResponseReceived(const FCopilotResponse& Response)
{
	// Must run on game thread for UI updates
	AsyncTask(ENamedThreads::GameThread, [this, Response]()
	{
		FString StatusStr;
		switch (Response.ResultStatus)
		{
		case ECopilotResultStatus::Success: StatusStr = TEXT("SUCCESS"); break;
		case ECopilotResultStatus::Failure: StatusStr = TEXT("FAILURE"); break;
		case ECopilotResultStatus::Timeout: StatusStr = TEXT("TIMEOUT"); break;
		case ECopilotResultStatus::Cancelled: StatusStr = TEXT("CANCELLED"); break;
		default: StatusStr = TEXT("PENDING"); break;
		}

		AppendToLog(FString::Printf(TEXT("Response [%s] %s"), *Response.RequestId, *StatusStr));

		if (Response.ResultStatus == ECopilotResultStatus::Success)
		{
			SetResponseText(Response.ResponseText);
		}
		else
		{
			SetResponseText(FString::Printf(TEXT("[%s] %s\n%s"), *StatusStr, *Response.ErrorMessage, *Response.ResponseText));
		}

		// Update diff preview if present
		if (Response.DiffPreview.bIsValid)
		{
			CurrentDiffPreview = Response.DiffPreview;
			SetDiffText(Response.DiffPreview.UnifiedDiff);
		}
	});
}

void SGitHubCopilotUEPanel::OnConnectionStatusChanged(ECopilotConnectionStatus NewStatus)
{
	AsyncTask(ENamedThreads::GameThread, [this, NewStatus]()
	{
		CurrentConnectionStatus = NewStatus;

		FString StatusStr;
		switch (NewStatus)
		{
		case ECopilotConnectionStatus::Connected: StatusStr = TEXT("Connected"); break;
		case ECopilotConnectionStatus::Connecting: StatusStr = TEXT("Connecting..."); break;
		case ECopilotConnectionStatus::Error: StatusStr = TEXT("Error"); break;
		default: StatusStr = TEXT("Disconnected"); break;
		}

		AppendToLog(FString::Printf(TEXT("Connection status: %s"), *StatusStr));
	});
}

void SGitHubCopilotUEPanel::OnLogMessageReceived(const FString& Message)
{
	AsyncTask(ENamedThreads::GameThread, [this, Message]()
	{
		AppendToLog(Message);
	});
}

// ============================================================================
// Helpers
// ============================================================================

FCopilotRequest SGitHubCopilotUEPanel::BuildRequest(ECopilotCommandType CommandType) const
{
	FCopilotRequest Request;
	Request.RequestId = FGitHubCopilotUECommandRouter::GenerateRequestId();
	Request.CommandType = CommandType;
	Request.ExecutionMode = ECopilotExecutionMode::PreviewOnly;
	Request.Timestamp = FDateTime::Now().ToString();

	if (ContextService.IsValid())
	{
		Request.ProjectContext = ContextService->GatherProjectContext();
	}

	return Request;
}

FText SGitHubCopilotUEPanel::GetConnectionStatusText() const
{
	switch (CurrentConnectionStatus)
	{
	case ECopilotConnectionStatus::Connected: return LOCTEXT("StatusConnected", "Connected");
	case ECopilotConnectionStatus::Connecting: return LOCTEXT("StatusConnecting", "Connecting...");
	case ECopilotConnectionStatus::Error: return LOCTEXT("StatusError", "Error");
	default: return LOCTEXT("StatusDisconnected", "Disconnected");
	}
}

FSlateColor SGitHubCopilotUEPanel::GetConnectionStatusColor() const
{
	switch (CurrentConnectionStatus)
	{
	case ECopilotConnectionStatus::Connected: return FSlateColor(FLinearColor::Green);
	case ECopilotConnectionStatus::Connecting: return FSlateColor(FLinearColor::Yellow);
	case ECopilotConnectionStatus::Error: return FSlateColor(FLinearColor::Red);
	default: return FSlateColor(FLinearColor::Gray);
	}
}

void SGitHubCopilotUEPanel::AppendToLog(const FString& Message)
{
	FString Timestamp = FDateTime::Now().ToString(TEXT("%H:%M:%S"));
	FString LogLine = FString::Printf(TEXT("[%s] %s\n"), *Timestamp, *Message);
	LogBuffer += LogLine;

	// Keep buffer manageable
	if (LogBuffer.Len() > 50000)
	{
		LogBuffer = LogBuffer.Right(30000);
	}

	if (LogTextBox.IsValid())
	{
		LogTextBox->SetText(FText::FromString(LogBuffer));
	}
}

void SGitHubCopilotUEPanel::SetResponseText(const FString& Text)
{
	if (ResponseTextBox.IsValid())
	{
		ResponseTextBox->SetText(FText::FromString(Text));
	}
}

void SGitHubCopilotUEPanel::SetDiffText(const FString& Text)
{
	if (DiffPreviewTextBox.IsValid())
	{
		DiffPreviewTextBox->SetText(FText::FromString(Text));
	}
}

#undef LOCTEXT_NAMESPACE
