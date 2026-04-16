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
#include "Widgets/Images/SThrobber.h"
#include "Widgets/Text/STextBlock.h"
#include "HAL/PlatformApplicationMisc.h"
#include "HAL/FileManager.h"
#include "Widgets/Input/SComboBox.h"
#include "DesktopPlatformModule.h"
#include "IDesktopPlatform.h"
#include "Framework/Application/SlateApplication.h"
#include "Misc/DateTime.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "Async/Async.h"
#include "InputCoreTypes.h"
#include "IImageWrapperModule.h"
#include "IImageWrapper.h"
#include "Brushes/SlateColorBrush.h"
#include "Styling/AppStyle.h"

#include "Windows/AllowWindowsPlatformTypes.h"
#include <Windows.h>
#include "Windows/HideWindowsPlatformTypes.h"

#define LOCTEXT_NAMESPACE "SGitHubCopilotUEPanel"

void SGitHubCopilotUEPanel::Construct(const FArguments& InArgs)
{
	CommandRouter = InArgs._CommandRouter;
	ContextService = InArgs._ContextService;
	BridgeService = InArgs._BridgeService;
	PatchService = InArgs._PatchService;

	// Retrieve or create a persistent conversation ID from the BridgeService
	// This survives panel recreation (e.g. tab close/reopen, layout restore)
	if (BridgeService.IsValid())
	{
		ConversationId = BridgeService->GetOrCreateConversationId();

		// Restore chat transcript from previous session
		FString SavedTranscript = BridgeService->GetChatTranscript();
		if (!SavedTranscript.IsEmpty())
		{
			ChatTranscriptBuffer = SavedTranscript;
		}
	}
	else
	{
		ConversationId = FGuid::NewGuid().ToString(EGuidFormats::Short);
	}

	// Wire up delegates — store handles for safe cleanup in destructor

	// Initialize reasoning effort options
	ReasoningOptions.Add(MakeShareable(new FString(TEXT("low"))));
	ReasoningOptions.Add(MakeShareable(new FString(TEXT("medium"))));
	ReasoningOptions.Add(MakeShareable(new FString(TEXT("high"))));
	SelectedReasoningOption = ReasoningOptions[1]; // default: medium
	if (CommandRouter.IsValid())
	{
		ResponseDelegateHandle = CommandRouter->OnResponseReceived.AddRaw(this, &SGitHubCopilotUEPanel::OnResponseReceived);
		RouterLogDelegateHandle = CommandRouter->OnLogMessage.AddRaw(this, &SGitHubCopilotUEPanel::OnLogMessageReceived);
	}

	if (BridgeService.IsValid())
	{
		ConnectionStatusDelegateHandle = BridgeService->OnConnectionStatusChanged.AddRaw(this, &SGitHubCopilotUEPanel::OnConnectionStatusChanged);
		BridgeLogDelegateHandle = BridgeService->OnLogMessage.AddRaw(this, &SGitHubCopilotUEPanel::OnLogMessageReceived);
		ActiveModelChangedDelegateHandle = BridgeService->OnActiveModelChanged.AddRaw(this, &SGitHubCopilotUEPanel::OnActiveModelChanged);
		ToolActivityDelegateHandle = BridgeService->OnToolActivity.AddRaw(this, &SGitHubCopilotUEPanel::OnToolActivity);
	}

	// --- Initialize dark mode text box styles ---
	{
		DarkTextBoxStyle = FEditableTextBoxStyle::GetDefault();
		DarkTextBoxStyle.SetBackgroundImageNormal(FSlateColorBrush(FLinearColor(0.04f, 0.04f, 0.04f)));
		DarkTextBoxStyle.SetBackgroundImageHovered(FSlateColorBrush(FLinearColor(0.06f, 0.06f, 0.06f)));
		DarkTextBoxStyle.SetBackgroundImageFocused(FSlateColorBrush(FLinearColor(0.08f, 0.08f, 0.08f)));
		DarkTextBoxStyle.SetBackgroundImageReadOnly(FSlateColorBrush(FLinearColor(0.04f, 0.04f, 0.04f)));
		DarkTextBoxStyle.SetForegroundColor(FSlateColor(FLinearColor::White));
		DarkTextBoxStyle.SetReadOnlyForegroundColor(FSlateColor(FLinearColor::White));
		DarkTextBoxStyle.SetFocusedForegroundColor(FSlateColor(FLinearColor::White));
		DarkTextBoxStyle.SetBackgroundColor(FSlateColor(FLinearColor(0.04f, 0.04f, 0.04f)));
		DarkTextBoxStyle.TextStyle.SetColorAndOpacity(FSlateColor(FLinearColor::White));
		DarkTextBoxStyle.TextStyle.SetFont(FSlateFontInfo(TEXT("C:\\Windows\\Fonts\\consolab.ttf"), 12));

		DarkLogTextBoxStyle = DarkTextBoxStyle;
		DarkLogTextBoxStyle.SetForegroundColor(FSlateColor(FLinearColor(0.6f, 0.6f, 0.6f)));
		DarkLogTextBoxStyle.SetReadOnlyForegroundColor(FSlateColor(FLinearColor(0.6f, 0.6f, 0.6f)));
		DarkLogTextBoxStyle.TextStyle.SetColorAndOpacity(FSlateColor(FLinearColor(0.6f, 0.6f, 0.6f)));

		DarkButtonStyle = FButtonStyle::GetDefault();
		DarkButtonStyle.SetNormal(FSlateColorBrush(FLinearColor(0.12f, 0.12f, 0.12f)));
		DarkButtonStyle.SetHovered(FSlateColorBrush(FLinearColor(0.22f, 0.22f, 0.25f)));
		DarkButtonStyle.SetPressed(FSlateColorBrush(FLinearColor(0.06f, 0.06f, 0.06f)));
		DarkButtonStyle.SetDisabled(FSlateColorBrush(FLinearColor(0.08f, 0.08f, 0.08f)));
		DarkButtonStyle.SetNormalForeground(FSlateColor(FLinearColor::White));
		DarkButtonStyle.SetHoveredForeground(FSlateColor(FLinearColor::White));
		DarkButtonStyle.SetPressedForeground(FSlateColor(FLinearColor(0.8f, 0.8f, 0.8f)));
		DarkButtonStyle.SetDisabledForeground(FSlateColor(FLinearColor(0.4f, 0.4f, 0.4f)));
	}

	ChildSlot
	[
		SNew(SBorder)
		.BorderImage(FCoreStyle::Get().GetBrush("GenericWhiteBox"))
		.BorderBackgroundColor(FLinearColor(0.02f, 0.02f, 0.02f, 1.0f))
		.ForegroundColor(FLinearColor::White)
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
			.SeparatorImage(FCoreStyle::Get().GetBrush("GenericWhiteBox"))
			.ColorAndOpacity(FLinearColor(0.15f, 0.15f, 0.15f))
		]

		// === Main content area (scrollable) ===
		+ SVerticalBox::Slot()
		.FillHeight(1.0f)
		.Padding(4)
		[
			SAssignNew(ChatScrollBox, SScrollBox)

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
				.SeparatorImage(FCoreStyle::Get().GetBrush("GenericWhiteBox"))
				.ColorAndOpacity(FLinearColor(0.15f, 0.15f, 0.15f))
			]

			// Prompt Input
			+ SScrollBox::Slot()
			.Padding(2)
			[
				BuildPromptInput()
			]

			// Response Output
			+ SScrollBox::Slot()
			.Padding(2)
			[
				BuildResponseArea()
			]

			// Action Buttons
			+ SScrollBox::Slot()
			.Padding(2)
			[
				BuildActionButtons()
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
			.SeparatorImage(FCoreStyle::Get().GetBrush("GenericWhiteBox"))
			.ColorAndOpacity(FLinearColor(0.15f, 0.15f, 0.15f))
		]

		// === Execution Log Footer ===
		+ SVerticalBox::Slot()
		.AutoHeight()
		.MaxHeight(150)
		.Padding(4)
		[
			BuildExecutionLog()
		]
		] // close SBorder
	];

	// Restore chat transcript to UI if we loaded one from disk
	if (!ChatTranscriptBuffer.IsEmpty())
	{
		SetResponseText(ChatTranscriptBuffer);
	}

	// Wire auth delegates
	if (BridgeService.IsValid())
	{
		DeviceCodeDelegateHandle = BridgeService->OnDeviceCodeReceived.AddRaw(this, &SGitHubCopilotUEPanel::OnDeviceCodeReceived);
		AuthCompleteDelegateHandle = BridgeService->OnAuthComplete.AddRaw(this, &SGitHubCopilotUEPanel::OnAuthComplete);
		ModelsLoadedDelegateHandle = BridgeService->OnModelsLoaded.AddRaw(this, &SGitHubCopilotUEPanel::OnModelsLoaded);
	}

	// Refresh context on load
	OnRefreshContext();
	UpdateThinkingIndicator();
	RefreshUploadSummary();

	// Restore saved chat transcript to the display
	if (!ChatTranscriptBuffer.IsEmpty())
	{
		SetResponseText(ChatTranscriptBuffer);
		AppendToLog(TEXT("Restored previous conversation from disk"));
	}
	else
	{
		AppendToLog(TEXT("GitHub Copilot UE panel initialized"));
	}

	// Update auth status
	if (BridgeService.IsValid())
	{
		CurrentConnectionStatus = BridgeService->GetConnectionStatus();

		if (BridgeService->IsAuthenticated())
		{
			if (AuthStatusText.IsValid())
			{
				AuthStatusText->SetText(FText::FromString(FString::Printf(TEXT("Signed in as: %s"), *BridgeService->GetUsername())));
			}

			AppendToLog(FString::Printf(TEXT("Signed in as: %s"), *BridgeService->GetUsername()));

			const TArray<FCopilotModel>& CachedModels = BridgeService->GetAvailableModels();
			if (CachedModels.Num() > 0)
			{
				OnModelsLoaded(CachedModels);
			}
			else
			{
				OnRefreshModels();
			}
		}
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
		BridgeService->OnActiveModelChanged.Remove(ActiveModelChangedDelegateHandle);
		BridgeService->OnToolActivity.Remove(ToolActivityDelegateHandle);
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
				.ColorAndOpacity(FSlateColor(FLinearColor(0.3f, 0.7f, 1.0f)))
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
				.ButtonStyle(&DarkButtonStyle)
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
				.ButtonStyle(&DarkButtonStyle)
				.Text(LOCTEXT("SignOutBtn", "Sign Out"))
				.OnClicked_Lambda([this]() -> FReply { OnSignOut(); return FReply::Handled(); })
			]

			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(4, 0)
			[
				SNew(SButton)
				.ButtonStyle(&DarkButtonStyle)
				.Text(LOCTEXT("RefreshModelsBtn", "Refresh Models"))
				.ToolTipText(LOCTEXT("RefreshModelsTip", "Reload model list from your Copilot subscription"))
				.OnClicked_Lambda([this]() -> FReply { OnRefreshModels(); return FReply::Handled(); })
			]

			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(4, 0)
			[
				SNew(SButton)
				.ButtonStyle(&DarkButtonStyle)
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
				.ColorAndOpacity(FSlateColor(FLinearColor(0.85f, 0.85f, 0.85f)))
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
				.ColorAndOpacity(FSlateColor(FLinearColor(0.85f, 0.85f, 0.85f)))
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

			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(8, 0, 4, 0)
			[
				SNew(STextBlock)
				.Text(LOCTEXT("ReasoningLabel", "Reasoning:"))
				.ColorAndOpacity(FSlateColor(FLinearColor(0.85f, 0.85f, 0.85f)))
			]

			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			[
				SAssignNew(ReasoningComboBox, SComboBox<TSharedPtr<FString>>)
				.OptionsSource(&ReasoningOptions)
				.OnSelectionChanged(this, &SGitHubCopilotUEPanel::OnReasoningSelected)
				.OnGenerateWidget_Lambda([](TSharedPtr<FString> Item) -> TSharedRef<SWidget>
				{
					return SNew(STextBlock).Text(FText::FromString(*Item));
				})
				.Content()
				[
					SNew(STextBlock)
					.Text_Lambda([this]() -> FText
					{
						return SelectedReasoningOption.IsValid()
							? FText::FromString(*SelectedReasoningOption)
							: LOCTEXT("DefaultReasoning", "medium");
					})
				]
			]
		];
}

TSharedRef<SWidget> SGitHubCopilotUEPanel::BuildProjectContextBox()
{
	return SNew(SExpandableArea)
		.BorderImage(FCoreStyle::Get().GetBrush("GenericWhiteBox"))
		.BorderBackgroundColor(FLinearColor(0.08f, 0.08f, 0.08f))
		.BodyBorderImage(FCoreStyle::Get().GetBrush("GenericWhiteBox"))
		.BodyBorderBackgroundColor(FLinearColor(0.03f, 0.03f, 0.03f))
		.AreaTitle(LOCTEXT("ProjectContext", "Project Context"))
		.InitiallyCollapsed(false)
		.BodyContent()
		[
			SAssignNew(ProjectContextText, STextBlock)
			.Text(LOCTEXT("ProjectContextLoading", "Loading..."))
			.AutoWrapText(true)
			.ColorAndOpacity(FSlateColor(FLinearColor(0.85f, 0.85f, 0.85f)))
		];
}

TSharedRef<SWidget> SGitHubCopilotUEPanel::BuildVRContextBox()
{
	return SNew(SExpandableArea)
		.BorderImage(FCoreStyle::Get().GetBrush("GenericWhiteBox"))
		.BorderBackgroundColor(FLinearColor(0.08f, 0.08f, 0.08f))
		.BodyBorderImage(FCoreStyle::Get().GetBrush("GenericWhiteBox"))
		.BodyBorderBackgroundColor(FLinearColor(0.03f, 0.03f, 0.03f))
		.AreaTitle(LOCTEXT("VRContext", "VR / Quest Context"))
		.InitiallyCollapsed(true)
		.BodyContent()
		[
			SAssignNew(VRContextText, STextBlock)
			.Text(LOCTEXT("VRContextLoading", "Click 'Analyze VR Setup' to load"))
			.AutoWrapText(true)
			.ColorAndOpacity(FSlateColor(FLinearColor(0.85f, 0.85f, 0.85f)))
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
			.Text(LOCTEXT("PromptLabel", "Chat Setup:"))
			.Font(FCoreStyle::GetDefaultFontStyle("Bold", 10))
			.ColorAndOpacity(FSlateColor(FLinearColor(0.85f, 0.85f, 0.85f)))
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			SNew(SBox)
			.MinDesiredHeight(36)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.FillWidth(1.0f)
				.Padding(0, 0, 4, 0)
				[
					SAssignNew(TargetPathTextBox, SEditableTextBox)
					.HintText(LOCTEXT("TargetPathHint", "Target path (file or package), e.g. Source/MyGame/MyClass.cpp or /Game/Blueprints"))
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				[
					SAssignNew(TargetLineTextBox, SEditableTextBox)
					.MinDesiredWidth(90.0f)
					.HintText(LOCTEXT("TargetLineHint", "Line"))
				]
			]
		];
}

TSharedRef<SWidget> SGitHubCopilotUEPanel::BuildActionButtons()
{
	// Helper lambda to create action buttons
	auto MakeButton = [this](const FText& Label, ECopilotCommandType CmdType) -> TSharedRef<SWidget>
	{
		return SNew(SButton)
			.ButtonStyle(&DarkButtonStyle)
			.Text(Label)
			.OnClicked_Lambda([this, CmdType]() -> FReply { OnActionButton(CmdType); return FReply::Handled(); });
	};

	return SNew(SExpandableArea)
		.BorderImage(FCoreStyle::Get().GetBrush("GenericWhiteBox"))
		.BorderBackgroundColor(FLinearColor(0.08f, 0.08f, 0.08f))
		.BodyBorderImage(FCoreStyle::Get().GetBrush("GenericWhiteBox"))
		.BodyBorderBackgroundColor(FLinearColor(0.03f, 0.03f, 0.03f))
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
					.ButtonStyle(&DarkButtonStyle)
					.Text(LOCTEXT("ApplyPatchBtn", "Apply Patch"))
					.ToolTipText(LOCTEXT("ApplyPatchTip", "Approve and apply the pending diff preview"))
					.OnClicked_Lambda([this]() -> FReply { OnApplyPatch(); return FReply::Handled(); })
				]
				+ SHorizontalBox::Slot().AutoWidth().Padding(2, 0)
				[
					SNew(SButton)
					.ButtonStyle(&DarkButtonStyle)
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
					.ButtonStyle(&DarkButtonStyle)
					.Text(LOCTEXT("CopyResponseBtn", "Copy Response"))
					.OnClicked_Lambda([this]() -> FReply { OnCopyResponse(); return FReply::Handled(); })
				]
			]
		];
}

TSharedRef<SWidget> SGitHubCopilotUEPanel::BuildResponseArea()
{
	return SNew(SExpandableArea)
		.BorderImage(FCoreStyle::Get().GetBrush("GenericWhiteBox"))
		.BorderBackgroundColor(FLinearColor(0.08f, 0.08f, 0.08f))
		.BodyBorderImage(FCoreStyle::Get().GetBrush("GenericWhiteBox"))
		.BodyBorderBackgroundColor(FLinearColor(0.03f, 0.03f, 0.03f))
		.AreaTitle(LOCTEXT("ResponseArea", "Chat"))
		.InitiallyCollapsed(false)
		.BodyContent()
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot()
			.FillHeight(1.0f)
			[
				SNew(SBox)
				.MinDesiredHeight(180.0f)
				.MaxDesiredHeight(450.0f)
				[
					SAssignNew(ResponseTextBox, SMultiLineEditableTextBox)
					.Style(&DarkTextBoxStyle)
					.IsReadOnly(true)
					.AutoWrapText(true)
					.HintText(LOCTEXT("ResponseHint", "Running dialogue appears here (UserHandle + returned Model label)..."))
				]
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 6.0f, 0.0f, 0.0f)
			[
				SAssignNew(ThinkingIndicatorRow, SHorizontalBox)
				.Visibility(EVisibility::Collapsed)
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(0, 0, 6, 0)
				[
					SNew(SThrobber)
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Text(LOCTEXT("ThinkingIndicatorText", "Copilot is thinking..."))
					.ColorAndOpacity(FSlateColor(FLinearColor(0.5f, 0.8f, 1.0f)))
				]
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 6.0f, 0.0f, 0.0f)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.FillWidth(1.0f)
				[
					SAssignNew(PromptTextBox, SMultiLineEditableTextBox)
					.Style(&DarkTextBoxStyle)
					.HintText(LOCTEXT("PromptHint", "Type a prompt or / command (Enter sends, Shift+Enter adds newline)..."))
					.AutoWrapText(true)
					.OnKeyDownHandler(FOnKeyDown::CreateLambda([this](const FGeometry&, const FKeyEvent& KeyEvent)
					{
						if (KeyEvent.GetKey() == EKeys::Enter && !KeyEvent.IsShiftDown())
						{
							OnSendPrompt();
							return FReply::Handled();
						}
						// Intercept Ctrl+V to check for clipboard image
						if (KeyEvent.GetKey() == EKeys::V && KeyEvent.IsControlDown())
						{
							if (TryPasteClipboardImage())
							{
								return FReply::Handled();
							}
						}
						return FReply::Unhandled();
					}))
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.Padding(6, 0, 0, 0)
				.VAlign(VAlign_Bottom)
				[
					SNew(SButton)
					.ButtonStyle(&DarkButtonStyle)
					.Text(LOCTEXT("SendBtn", "Send"))
					.OnClicked_Lambda([this]() -> FReply { OnSendPrompt(); return FReply::Handled(); })
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.Padding(4, 0, 0, 0)
				.VAlign(VAlign_Bottom)
				[
					SNew(SButton)
					.ButtonStyle(&DarkButtonStyle)
					.Text(LOCTEXT("ClearBtn", "Clear"))
					.OnClicked_Lambda([this]() -> FReply { OnClearAll(); return FReply::Handled(); })
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.Padding(4, 0, 0, 0)
				.VAlign(VAlign_Bottom)
				[
					SNew(SButton)
					.ButtonStyle(&DarkButtonStyle)
					.Text(LOCTEXT("UploadBtn", "Upload"))
					.OnClicked_Lambda([this]() -> FReply { OnUploadFiles(); return FReply::Handled(); })
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.Padding(4, 0, 0, 0)
				.VAlign(VAlign_Bottom)
				[
					SNew(SButton)
					.ButtonStyle(&DarkButtonStyle)
					.Text(LOCTEXT("ClearUploadsBtn", "Clear Uploads"))
					.OnClicked_Lambda([this]() -> FReply { OnClearUploads(); return FReply::Handled(); })
				]
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 4.0f, 0.0f, 0.0f)
			[
				SAssignNew(UploadSummaryText, STextBlock)
				.Text(LOCTEXT("UploadSummaryNone", "Attachments: none"))
				.AutoWrapText(true)
				.ColorAndOpacity(FSlateColor(FLinearColor(0.7f, 0.7f, 0.7f)))
			]
		];
}

TSharedRef<SWidget> SGitHubCopilotUEPanel::BuildDiffPreviewArea()
{
	return SNew(SExpandableArea)
		.BorderImage(FCoreStyle::Get().GetBrush("GenericWhiteBox"))
		.BorderBackgroundColor(FLinearColor(0.08f, 0.08f, 0.08f))
		.BodyBorderImage(FCoreStyle::Get().GetBrush("GenericWhiteBox"))
		.BodyBorderBackgroundColor(FLinearColor(0.03f, 0.03f, 0.03f))
		.AreaTitle(LOCTEXT("DiffPreview", "Diff Preview"))
		.InitiallyCollapsed(true)
		.BodyContent()
		[
			SNew(SBox)
			.MinDesiredHeight(100.0f)
			.MaxDesiredHeight(300.0f)
			[
				SAssignNew(DiffPreviewTextBox, SMultiLineEditableTextBox)
				.Style(&DarkTextBoxStyle)
				.IsReadOnly(true)
				.AutoWrapText(false)
				.HintText(LOCTEXT("DiffHint", "Diff previews will appear here..."))
			]
		];
}

TSharedRef<SWidget> SGitHubCopilotUEPanel::BuildExecutionLog()
{
	return SNew(SExpandableArea)
		.BorderImage(FCoreStyle::Get().GetBrush("GenericWhiteBox"))
		.BorderBackgroundColor(FLinearColor(0.08f, 0.08f, 0.08f))
		.BodyBorderImage(FCoreStyle::Get().GetBrush("GenericWhiteBox"))
		.BodyBorderBackgroundColor(FLinearColor(0.03f, 0.03f, 0.03f))
		.AreaTitle(LOCTEXT("ExecLog", "Execution Log"))
		.InitiallyCollapsed(false)
		.Padding(FMargin(2.0f))
		.BodyContent()
		[
			SNew(SBox)
			.MaxDesiredHeight(120.0f)
			[
				SAssignNew(LogTextBox, SMultiLineEditableTextBox)
				.Style(&DarkLogTextBoxStyle)
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

	FString Prompt = PromptTextBox->GetText().ToString().TrimStartAndEnd();
	if (Prompt.IsEmpty())
	{
		AppendToLog(TEXT("Cannot send empty prompt"));
		return;
	}

	// Check for slash commands first (e.g., /help, /login, /model)
	if (Prompt.TrimStartAndEnd().StartsWith(TEXT("/")))
	{
		const FString SlashInput = Prompt.TrimStartAndEnd();
		FString SlashBody = SlashInput;
		SlashBody.RemoveFromStart(TEXT("/"));

		FString SlashCommandName;
		FString UnusedArgs;
		if (!SlashBody.Split(TEXT(" "), &SlashCommandName, &UnusedArgs))
		{
			SlashCommandName = SlashBody;
		}
		SlashCommandName = SlashCommandName.ToLower().TrimStartAndEnd();

		// /clear and /new should clear the dockable panel immediately.
		if (SlashCommandName == TEXT("clear") || SlashCommandName == TEXT("new"))
		{
			OnClearAll();
			AppendToLog(FString::Printf(TEXT("Executed: %s"), *SlashInput));
			PromptTextBox->SetText(FText::GetEmpty());
			return;
		}

		// Route through the module's slash command system
		FGitHubCopilotUEModule& Module = FModuleManager::LoadModuleChecked<FGitHubCopilotUEModule>("GitHubCopilotUE");
		TSharedPtr<FGitHubCopilotUESlashCommands> SlashCmds = Module.GetSlashCommands();
		if (SlashCmds.IsValid())
		{
			FString Response;
			if (SlashCmds->ExecuteSlashCommand(SlashInput, Response))
			{
				AppendChatTurn(TEXT("System"), Response);
				AppendToLog(FString::Printf(TEXT("Executed: %s"), *SlashInput));
				PromptTextBox->SetText(FText::GetEmpty());
				return;
			}
		}
	}

	const FString UserHandle = GetUserHandle();
	FString UserTurnMessage = Prompt;
	if (PendingUploadPaths.Num() > 0)
	{
		UserTurnMessage += FString::Printf(TEXT("\n[%s]"), *BuildUploadSummaryLabel());
	}
	AppendChatTurn(UserHandle.IsEmpty() ? TEXT("You") : UserHandle, UserTurnMessage);

	// Default to AnalyzeSelection for free-form prompts sent to backend
	FCopilotRequest Request = BuildRequest(ECopilotCommandType::AnalyzeSelection);
	Request.UserPrompt = Prompt;
	AppendPendingUploadsToRequest(Request);

	PendingCommandTypes.Add(Request.RequestId, Request.CommandType);
	PendingPromptByRequestId.Add(Request.RequestId, Prompt);
	MarkBackendRequestPending(Request.RequestId);

	// Log conversation state for diagnostics
	const int32 MsgCount = BridgeService.IsValid() ? BridgeService->GetConversationMessageCount(ConversationId) : -1;
	AppendToLog(FString::Printf(TEXT("Sending prompt (ID: %s, ConvoID: %s, history: %d msgs)..."),
		*Request.RequestId, *ConversationId, MsgCount));

	CommandRouter->RouteCommand(Request);
	PendingUploadPaths.Empty();
	RefreshUploadSummary();

	// Clear the prompt
	PromptTextBox->SetText(FText::GetEmpty());
}

void SGitHubCopilotUEPanel::OnActionButton(ECopilotCommandType CommandType)
{
	if (!CommandRouter.IsValid()) return;

	auto FailAction = [this](const FString& Message)
	{
		AppendToLog(Message);
		AppendChatTurn(TEXT("System"), Message);
	};

	FCopilotRequest Request = BuildRequest(CommandType);
	const FString PromptText = PromptTextBox.IsValid() ? PromptTextBox->GetText().ToString().TrimStartAndEnd() : TEXT("");
	Request.UserPrompt = PromptText;
	PopulateRequestTargetsFromUI(Request, CommandType, PromptText);
	AppendPendingUploadsToRequest(Request);

	switch (CommandType)
	{
	case ECopilotCommandType::PatchFile:
		if (Request.FileTargets.Num() == 0 || Request.FileTargets[0].FilePath.IsEmpty())
		{
			FailAction(TEXT("Patch Preview requires a target file path. Enter one in the Target path field."));
			return;
		}
		if (Request.FileTargets[0].SelectedText.IsEmpty())
		{
			FailAction(TEXT("Patch Preview requires proposed content in the prompt box."));
			return;
		}
		Request.ExecutionMode = ECopilotExecutionMode::PreviewOnly;
		break;
	case ECopilotCommandType::InsertIntoFile:
		if (Request.FileTargets.Num() == 0 || Request.FileTargets[0].FilePath.IsEmpty())
		{
			FailAction(TEXT("Insert Into File requires a target file path."));
			return;
		}
		if (Request.FileTargets[0].SelectedText.IsEmpty())
		{
			FailAction(TEXT("Insert Into File requires prompt text to insert."));
			return;
		}
		break;
	case ECopilotCommandType::OpenFile:
		if (Request.FileTargets.Num() == 0 && Request.UserPrompt.IsEmpty())
		{
			FailAction(TEXT("Open Related File needs a file path (target path field or prompt)."));
			return;
		}
		break;
	default:
		break;
	}

	if (CommandRouter->RequiresBackend(CommandType) && !Request.UserPrompt.IsEmpty())
	{
		const FString UserHandle = GetUserHandle();
		FString UserTurnMessage = Request.UserPrompt;
		if (Request.Attachments.Num() > 0)
		{
			UserTurnMessage += FString::Printf(TEXT("\n[%s]"), *BuildUploadSummaryLabel());
		}
		AppendChatTurn(UserHandle.IsEmpty() ? TEXT("You") : UserHandle, UserTurnMessage);
		PendingPromptByRequestId.Add(Request.RequestId, Request.UserPrompt);
	}
	else if (CommandRouter->RequiresBackend(CommandType) && Request.Attachments.Num() > 0)
	{
		AppendChatTurn(TEXT("System"), FString::Printf(TEXT("%s"), *BuildUploadSummaryLabel()));
	}

	FString CommandName = TEXT("Action");
	if (const UEnum* CommandEnum = StaticEnum<ECopilotCommandType>())
	{
		CommandName = CommandEnum->GetDisplayNameTextByValue(static_cast<int64>(CommandType)).ToString();
	}
	AppendChatTurn(TEXT("System"), FString::Printf(TEXT("Running action: %s"), *CommandName));

	PendingCommandTypes.Add(Request.RequestId, CommandType);
	if (CommandRouter->RequiresBackend(CommandType))
	{
		MarkBackendRequestPending(Request.RequestId);
	}
	AppendToLog(FString::Printf(TEXT("Executing command %d (ID: %s)..."), (int32)CommandType, *Request.RequestId));
	CommandRouter->RouteCommand(Request);
	if (CommandRouter->RequiresBackend(CommandType))
	{
		PendingUploadPaths.Empty();
		RefreshUploadSummary();
	}
}

void SGitHubCopilotUEPanel::OnUploadFiles()
{
	IDesktopPlatform* DesktopPlatform = FDesktopPlatformModule::Get();
	if (!DesktopPlatform)
	{
		AppendToLog(TEXT("File upload is unavailable: Desktop platform module is not loaded."));
		return;
	}

	const void* ParentWindowHandle = nullptr;
	if (FSlateApplication::IsInitialized())
	{
		ParentWindowHandle = FSlateApplication::Get().FindBestParentWindowHandleForDialogs(nullptr);
	}

	const FString FileTypes =
		TEXT("Supported Files|*.png;*.jpg;*.jpeg;*.webp;*.bmp;*.gif;*.txt;*.md;*.json;*.ini;*.cpp;*.h;*.cs;*.log|")
		TEXT("Image Files|*.png;*.jpg;*.jpeg;*.webp;*.bmp;*.gif|")
		TEXT("Text + Code Files|*.txt;*.md;*.json;*.ini;*.cpp;*.h;*.cs;*.log|")
		TEXT("All Files|*.*");

	TArray<FString> PickedFiles;
	const bool bPickedFiles = DesktopPlatform->OpenFileDialog(
		ParentWindowHandle,
		TEXT("Attach files to Copilot request"),
		FPaths::ProjectDir(),
		TEXT(""),
		FileTypes,
		EFileDialogFlags::Multiple,
		PickedFiles);

	if (!bPickedFiles || PickedFiles.Num() == 0)
	{
		return;
	}

	int32 AddedCount = 0;
	for (const FString& Path : PickedFiles)
	{
		if (!FPaths::FileExists(Path))
		{
			AppendToLog(FString::Printf(TEXT("Skipped missing file: %s"), *Path));
			continue;
		}

		const FString NormalizedPath = FPaths::ConvertRelativePathToFull(Path);
		if (!PendingUploadPaths.Contains(NormalizedPath))
		{
			PendingUploadPaths.Add(NormalizedPath);
			++AddedCount;
		}
	}

	RefreshUploadSummary();
	if (AddedCount > 0)
	{
		AppendToLog(FString::Printf(TEXT("Attached %d file(s)."), AddedCount));
	}
}

void SGitHubCopilotUEPanel::OnClearUploads()
{
	if (PendingUploadPaths.Num() == 0)
	{
		return;
	}

	PendingUploadPaths.Empty();
	RefreshUploadSummary();
	AppendToLog(TEXT("Cleared attached files."));
}

bool SGitHubCopilotUEPanel::TryPasteClipboardImage()
{
	// Check Windows clipboard for image data (CF_BITMAP or CF_DIB)
	if (!OpenClipboard(NULL))
	{
		return false;
	}

	bool bHandled = false;

	if (IsClipboardFormatAvailable(CF_DIB) || IsClipboardFormatAvailable(CF_BITMAP))
	{
		HANDLE hData = GetClipboardData(CF_DIB);
		if (hData)
		{
			BITMAPINFOHEADER* pBIH = (BITMAPINFOHEADER*)GlobalLock(hData);
			if (pBIH)
			{
				int32 Width = pBIH->biWidth;
				int32 Height = FMath::Abs(pBIH->biHeight);
				bool bTopDown = (pBIH->biHeight < 0);

				if (Width > 0 && Height > 0 && pBIH->biBitCount >= 24)
				{
					int32 BitsPerPixel = pBIH->biBitCount;
					int32 RowBytes = ((Width * BitsPerPixel + 31) / 32) * 4;
					uint8* pPixels = (uint8*)pBIH + pBIH->biSize + pBIH->biClrUsed * 4;

					TArray<FColor> PixelData;
					PixelData.SetNumUninitialized(Width * Height);

					for (int32 y = 0; y < Height; ++y)
					{
						int32 SrcRow = bTopDown ? y : (Height - 1 - y);
						uint8* pRow = pPixels + SrcRow * RowBytes;
						for (int32 x = 0; x < Width; ++x)
						{
							int32 Idx = y * Width + x;
							if (BitsPerPixel == 32)
							{
								PixelData[Idx].B = pRow[x * 4 + 0];
								PixelData[Idx].G = pRow[x * 4 + 1];
								PixelData[Idx].R = pRow[x * 4 + 2];
								PixelData[Idx].A = pRow[x * 4 + 3];
							}
							else
							{
								PixelData[Idx].B = pRow[x * 3 + 0];
								PixelData[Idx].G = pRow[x * 3 + 1];
								PixelData[Idx].R = pRow[x * 3 + 2];
								PixelData[Idx].A = 255;
							}
						}
					}

					GlobalUnlock(hData);
					CloseClipboard();

					// Encode to PNG and save
					IImageWrapperModule& ImgModule = FModuleManager::LoadModuleChecked<IImageWrapperModule>(FName("ImageWrapper"));
					TSharedPtr<IImageWrapper> PngWrapper = ImgModule.CreateImageWrapper(EImageFormat::PNG);
					if (PngWrapper.IsValid() && PngWrapper->SetRaw(PixelData.GetData(), PixelData.Num() * sizeof(FColor), Width, Height, ERGBFormat::BGRA, 8))
					{
						const TArray64<uint8>& PngData = PngWrapper->GetCompressed();
						FString Timestamp = FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S"));
						FString SavePath = FPaths::Combine(FPaths::ProjectSavedDir(), FString::Printf(TEXT("CopilotPaste_%s.png"), *Timestamp));
						if (FFileHelper::SaveArrayToFile(PngData, *SavePath))
						{
							PendingUploadPaths.Add(SavePath);
							RefreshUploadSummary();
							AppendToLog(FString::Printf(TEXT("Pasted image from clipboard: %dx%d (%s)"), Width, Height, *FPaths::GetCleanFilename(SavePath)));
							return true;
						}
					}

					return false; // PNG encode failed
				}

				GlobalUnlock(hData);
				}
		}
	}

	CloseClipboard();
	return bHandled;
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
	ChatTranscriptBuffer.Empty();
	PendingPromptByRequestId.Empty();
	PendingUploadPaths.Empty();
	RefreshUploadSummary();

	// Clear the conversation in the bridge service and start a new one
	if (BridgeService.IsValid())
	{
		ConversationId = BridgeService->ResetConversationId();
		BridgeService->SetChatTranscript(TEXT(""));
		// Delete the conversation cache file
		IFileManager::Get().Delete(*BridgeService->GetConversationCachePath());
	}
	else
	{
		ConversationId = FGuid::NewGuid().ToString(EGuidFormats::Short);
	}

	AppendToLog(TEXT("Cleared all fields — new conversation started"));
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
	PendingCommandTypes.Add(Request.RequestId, Request.CommandType);
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
	PendingCommandTypes.Add(Request.RequestId, Request.CommandType);
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
	PendingCommandTypes.Empty();
	AppendToLog(TEXT("Signed out"));
}

void SGitHubCopilotUEPanel::OnRefreshModels()
{
	if (!BridgeService.IsValid())
	{
		AppendToLog(TEXT("ERROR: Bridge service not available"));
		return;
	}

	if (!BridgeService->IsAuthenticated())
	{
		AppendToLog(TEXT("Sign in first to load models."));
		return;
	}

	AppendToLog(TEXT("Refreshing available models..."));
	BridgeService->FetchAvailableModels();
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
		SelectedModelOption.Reset();
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
			if (!SelectedModelOption.IsValid() && ModelOptions.Num() > 0)
			{
				SelectedModelOption = ModelOptions[0];
			}
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
		const FString CurrentModel = BridgeService->GetActiveModel();
		if (CurrentModel != *NewModel)
		{
			BridgeService->SetActiveModel(*NewModel);
			BridgeService->SaveTokenCache();
			AppendToLog(FString::Printf(TEXT("Model changed to: %s"), **NewModel));
		}
	}
}

void SGitHubCopilotUEPanel::OnReasoningSelected(TSharedPtr<FString> NewEffort, ESelectInfo::Type SelectInfo)
{
	if (NewEffort.IsValid() && BridgeService.IsValid())
	{
		SelectedReasoningOption = NewEffort;
		BridgeService->SetReasoningEffort(*NewEffort);
		AppendToLog(FString::Printf(TEXT("Reasoning effort set to: %s"), **NewEffort));
	}
}

void SGitHubCopilotUEPanel::OnActiveModelChanged(const FString& ModelId)
{
	AsyncTask(ENamedThreads::GameThread, [this, ModelId]()
	{
		for (const TSharedPtr<FString>& Opt : ModelOptions)
		{
			if (Opt.IsValid() && *Opt == ModelId)
			{
				SelectedModelOption = Opt;
				if (ModelComboBox.IsValid())
				{
					ModelComboBox->SetSelectedItem(Opt);
				}
				break;
			}
		}
	});
}

void SGitHubCopilotUEPanel::OnToolActivity(const FString& ActivityMessage)
{
	AsyncTask(ENamedThreads::GameThread, [this, ActivityMessage]()
	{
		AppendChatTurn(TEXT("[tool]"), ActivityMessage);
	});
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
		MarkBackendRequestCompleted(Response.RequestId);

		ECopilotCommandType ResolvedCommandType = Response.CommandType;
		if (const ECopilotCommandType* PendingType = PendingCommandTypes.Find(Response.RequestId))
		{
			ResolvedCommandType = *PendingType;
			PendingCommandTypes.Remove(Response.RequestId);
		}
		PendingPromptByRequestId.Remove(Response.RequestId);

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

		FString Speaker = TEXT("System");
		if (const FString* ReturnedModel = Response.ProviderMetadata.Find(TEXT("model")))
		{
			const FString TrimmedModel = ReturnedModel->TrimStartAndEnd();
			Speaker = TrimmedModel.IsEmpty()
				? TEXT("Model (API: missing)")
				: FString::Printf(TEXT("Model (%s)"), *TrimmedModel);
		}
		else if (CommandRouter.IsValid() && CommandRouter->RequiresBackend(ResolvedCommandType))
		{
			Speaker = TEXT("Model (API: missing)");
		}

		FString DisplayText;
		if (Response.ResultStatus == ECopilotResultStatus::Success)
		{
			DisplayText = Response.ResponseText;

			if ((ResolvedCommandType == ECopilotCommandType::GatherVRContext || ResolvedCommandType == ECopilotCommandType::RunQuestAudit) && VRContextText.IsValid())
			{
				VRContextText->SetText(FText::FromString(Response.ResponseText));
			}
			else if (ResolvedCommandType == ECopilotCommandType::GatherProjectContext && ProjectContextText.IsValid())
			{
				ProjectContextText->SetText(FText::FromString(Response.ResponseText));
			}
		}
		else
		{
			DisplayText = FString::Printf(TEXT("[%s] %s\n%s"), *StatusStr, *Response.ErrorMessage, *Response.ResponseText);
		}

		if (DisplayText.IsEmpty())
		{
			DisplayText = TEXT("(no response content)");
		}
		AppendChatTurn(Speaker, DisplayText);

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

void SGitHubCopilotUEPanel::AppendPendingUploadsToRequest(FCopilotRequest& Request) const
{
	for (const FString& PendingPath : PendingUploadPaths)
	{
		if (PendingPath.IsEmpty())
		{
			continue;
		}

		FCopilotAttachment Attachment;
		Attachment.FilePath = PendingPath;
		Request.Attachments.Add(Attachment);
	}
}

FString SGitHubCopilotUEPanel::BuildUploadSummaryLabel() const
{
	if (PendingUploadPaths.Num() == 0)
	{
		return TEXT("Attachments: none");
	}

	const int32 MaxVisibleNames = 3;
	const int32 VisibleCount = FMath::Min(MaxVisibleNames, PendingUploadPaths.Num());
	TArray<FString> FileNames;
	FileNames.Reserve(VisibleCount);
	for (int32 Index = 0; Index < VisibleCount; ++Index)
	{
		FileNames.Add(FPaths::GetCleanFilename(PendingUploadPaths[Index]));
	}

	FString Suffix;
	if (PendingUploadPaths.Num() > VisibleCount)
	{
		Suffix = FString::Printf(TEXT(" +%d more"), PendingUploadPaths.Num() - VisibleCount);
	}

	return FString::Printf(
		TEXT("Attachments: %d (%s%s)"),
		PendingUploadPaths.Num(),
		*FString::Join(FileNames, TEXT(", ")),
		*Suffix);
}

void SGitHubCopilotUEPanel::RefreshUploadSummary()
{
	if (UploadSummaryText.IsValid())
	{
		UploadSummaryText->SetText(FText::FromString(BuildUploadSummaryLabel()));
	}
}

FCopilotRequest SGitHubCopilotUEPanel::BuildRequest(ECopilotCommandType CommandType) const
{
	FCopilotRequest Request;
	Request.RequestId = FGitHubCopilotUECommandRouter::GenerateRequestId();
	Request.ConversationId = ConversationId; // Persistent across messages
	Request.CommandType = CommandType;
	Request.ExecutionMode = ECopilotExecutionMode::PreviewOnly;
	Request.Timestamp = FDateTime::Now().ToString();

	if (ContextService.IsValid())
	{
		Request.ProjectContext = ContextService->GatherProjectContext();
	}

	return Request;
}

void SGitHubCopilotUEPanel::PopulateRequestTargetsFromUI(FCopilotRequest& Request, ECopilotCommandType CommandType, const FString& PromptText) const
{
	const FString TargetPath = TargetPathTextBox.IsValid() ? TargetPathTextBox->GetText().ToString().TrimStartAndEnd() : TEXT("");
	const FString TargetLineText = TargetLineTextBox.IsValid() ? TargetLineTextBox->GetText().ToString().TrimStartAndEnd() : TEXT("");
	const int32 TargetLine = TargetLineText.IsEmpty() ? 1 : FMath::Max(1, FCString::Atoi(*TargetLineText));

	switch (CommandType)
	{
	case ECopilotCommandType::PatchFile:
		if (!TargetPath.IsEmpty())
		{
			FCopilotFileTarget Target;
			Target.FilePath = TargetPath;
			Target.SelectedText = PromptText;
			Request.FileTargets.Add(Target);
		}
		break;
	case ECopilotCommandType::InsertIntoFile:
		if (!TargetPath.IsEmpty())
		{
			FCopilotFileTarget Target;
			Target.FilePath = TargetPath;
			Target.LineStart = TargetLine;
			Target.SelectedText = PromptText;
			Request.FileTargets.Add(Target);
		}
		break;
	case ECopilotCommandType::OpenFile:
		if (!TargetPath.IsEmpty())
		{
			FCopilotFileTarget Target;
			Target.FilePath = TargetPath;
			Request.FileTargets.Add(Target);
		}
		break;
	case ECopilotCommandType::OpenAsset:
		if (!TargetPath.IsEmpty())
		{
			Request.CommandArguments.Add(TEXT("AssetPath"), TargetPath);
		}
		break;
	case ECopilotCommandType::CreateBlueprintFunctionLibrary:
		if (!PromptText.IsEmpty())
		{
			Request.CommandArguments.Add(TEXT("ClassName"), PromptText);
		}
		if (!TargetPath.IsEmpty())
		{
			Request.CommandArguments.Add(TEXT("PackagePath"), TargetPath);
		}
		break;
	case ECopilotCommandType::CreateCppClass:
	case ECopilotCommandType::CreateActorComponent:
		if (!PromptText.IsEmpty())
		{
			Request.CommandArguments.Add(TEXT("ClassName"), PromptText);
		}
		break;
	case ECopilotCommandType::ApproveAndApplyPatch:
	case ECopilotCommandType::RollbackPatch:
		if (!TargetPath.IsEmpty())
		{
			FCopilotFileTarget Target;
			Target.FilePath = TargetPath;
			Request.FileTargets.Add(Target);
		}
		break;
	default:
		break;
	}
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

void SGitHubCopilotUEPanel::AppendChatTurn(const FString& Speaker, const FString& Message)
{
	const FString SafeSpeaker = Speaker.TrimStartAndEnd().IsEmpty() ? TEXT("System") : Speaker.TrimStartAndEnd();
	const FString SafeMessage = Message.IsEmpty() ? TEXT("(no response content)") : Message;

	if (!ChatTranscriptBuffer.IsEmpty())
	{
		ChatTranscriptBuffer += TEXT("\n\n");
	}
	ChatTranscriptBuffer += FString::Printf(TEXT("%s: %s"), *SafeSpeaker, *SafeMessage);

	// Keep transcript bounded so long sessions don't explode memory usage.
	if (ChatTranscriptBuffer.Len() > 120000)
	{
		ChatTranscriptBuffer = ChatTranscriptBuffer.Right(90000);
	}

	SetResponseText(ChatTranscriptBuffer);

	// Persist transcript + conversation to disk after every turn
	if (BridgeService.IsValid())
	{
		BridgeService->SetChatTranscript(ChatTranscriptBuffer);
		BridgeService->SaveConversationCache();
	}
}

void SGitHubCopilotUEPanel::MarkBackendRequestPending(const FString& RequestId)
{
	if (RequestId.IsEmpty())
	{
		return;
	}

	PendingBackendRequestIds.Add(RequestId);
	UpdateThinkingIndicator();
}

void SGitHubCopilotUEPanel::MarkBackendRequestCompleted(const FString& RequestId)
{
	if (RequestId.IsEmpty())
	{
		return;
	}

	PendingBackendRequestIds.Remove(RequestId);
	UpdateThinkingIndicator();
}

void SGitHubCopilotUEPanel::UpdateThinkingIndicator()
{
	const bool bShouldShow = PendingBackendRequestIds.Num() > 0;
	if (bThinkingVisible == bShouldShow)
	{
		return;
	}

	bThinkingVisible = bShouldShow;
	if (ThinkingIndicatorRow.IsValid())
	{
		ThinkingIndicatorRow->SetVisibility(bThinkingVisible ? EVisibility::Visible : EVisibility::Collapsed);
	}
}

FString SGitHubCopilotUEPanel::GetUserHandle() const
{
	if (BridgeService.IsValid())
	{
		FString GitHubUser = BridgeService->GetUsername();
		if (!GitHubUser.IsEmpty())
		{
			return GitHubUser;
		}
	}
	return TEXT("You");
}

void SGitHubCopilotUEPanel::SetResponseText(const FString& Text)
{
	if (ResponseTextBox.IsValid())
	{
		ResponseTextBox->SetText(FText::FromString(Text));
	}
	// Auto-scroll to bottom so latest messages are visible
	if (ChatScrollBox.IsValid())
	{
		ChatScrollBox->ScrollToEnd();
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
