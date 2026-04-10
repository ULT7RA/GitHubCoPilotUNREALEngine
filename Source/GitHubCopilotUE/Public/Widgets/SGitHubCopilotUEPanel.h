// Copyright GitHub, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/Input/SMultiLineEditableTextBox.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Text/STextBlock.h"
#include "Services/GitHubCopilotUETypes.h"

class FGitHubCopilotUECommandRouter;
class FGitHubCopilotUEContextService;
class FGitHubCopilotUEBridgeService;
class FGitHubCopilotUEPatchService;
class SHorizontalBox;
template<typename> class SComboBox;
struct FCopilotModel;

/**
 * Main Slate panel widget for the GitHub Copilot UE dockable tab.
 * Contains: status bar, context boxes, prompt input, action buttons,
 * response area, diff preview, and execution log.
 */
class SGitHubCopilotUEPanel : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SGitHubCopilotUEPanel) {}
		SLATE_ARGUMENT(TSharedPtr<FGitHubCopilotUECommandRouter>, CommandRouter)
		SLATE_ARGUMENT(TSharedPtr<FGitHubCopilotUEContextService>, ContextService)
		SLATE_ARGUMENT(TSharedPtr<FGitHubCopilotUEBridgeService>, BridgeService)
		SLATE_ARGUMENT(TSharedPtr<FGitHubCopilotUEPatchService>, PatchService)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	virtual ~SGitHubCopilotUEPanel();

private:
	// --- UI construction helpers ---
	TSharedRef<SWidget> BuildStatusBar();
	TSharedRef<SWidget> BuildProjectContextBox();
	TSharedRef<SWidget> BuildVRContextBox();
	TSharedRef<SWidget> BuildPromptInput();
	TSharedRef<SWidget> BuildActionButtons();
	TSharedRef<SWidget> BuildResponseArea();
	TSharedRef<SWidget> BuildDiffPreviewArea();
	TSharedRef<SWidget> BuildExecutionLog();

	// --- Action handlers ---
	void OnSendPrompt();
	void OnActionButton(ECopilotCommandType CommandType);
	void OnUploadFiles();
	void OnClearUploads();
	bool TryPasteClipboardImage();
	void OnCopyResponse();
	void OnClearAll();
	void OnApplyPatch();
	void OnRollbackPatch();
	void OnRefreshContext();
	void OnSignIn();
	void OnSignOut();
	void OnRefreshModels();
	void OnModelSelected(TSharedPtr<FString> NewModel, ESelectInfo::Type SelectInfo);
	TSharedRef<SWidget> MakeModelComboRow(TSharedPtr<FString> Item);

	// --- Event handlers ---
	void OnDeviceCodeReceived(const FString& UserCode, const FString& VerificationURI);
	void OnAuthComplete();
	void OnModelsLoaded(const TArray<FCopilotModel>& Models);
	void OnActiveModelChanged(const FString& ModelId);
	void OnToolActivity(const FString& ActivityMessage);
	void OnResponseReceived(const FCopilotResponse& Response);
	void OnConnectionStatusChanged(ECopilotConnectionStatus NewStatus);
	void OnLogMessageReceived(const FString& Message);

	// --- Helper methods ---
	FCopilotRequest BuildRequest(ECopilotCommandType CommandType) const;
	void PopulateRequestTargetsFromUI(FCopilotRequest& Request, ECopilotCommandType CommandType, const FString& PromptText) const;
	FText GetConnectionStatusText() const;
	FSlateColor GetConnectionStatusColor() const;
	void AppendToLog(const FString& Message);
	void AppendChatTurn(const FString& Speaker, const FString& Message);
	void MarkBackendRequestPending(const FString& RequestId);
	void MarkBackendRequestCompleted(const FString& RequestId);
	void UpdateThinkingIndicator();
	void AppendPendingUploadsToRequest(FCopilotRequest& Request) const;
	FString BuildUploadSummaryLabel() const;
	void RefreshUploadSummary();
	FString GetUserHandle() const;
	void SetResponseText(const FString& Text);
	void SetDiffText(const FString& Text);

	// --- Service references ---
	TSharedPtr<FGitHubCopilotUECommandRouter> CommandRouter;
	TSharedPtr<FGitHubCopilotUEContextService> ContextService;
	TSharedPtr<FGitHubCopilotUEBridgeService> BridgeService;
	TSharedPtr<FGitHubCopilotUEPatchService> PatchService;

	// --- UI State ---
	TSharedPtr<SMultiLineEditableTextBox> PromptTextBox;
	TSharedPtr<SMultiLineEditableTextBox> ResponseTextBox;
	TSharedPtr<SMultiLineEditableTextBox> DiffPreviewTextBox;
	TSharedPtr<SMultiLineEditableTextBox> LogTextBox;
	TSharedPtr<STextBlock> UploadSummaryText;
	TSharedPtr<SHorizontalBox> ThinkingIndicatorRow;
	TSharedPtr<SScrollBox> ChatScrollBox;
	TSharedPtr<SEditableTextBox> TargetPathTextBox;
	TSharedPtr<SEditableTextBox> TargetLineTextBox;
	TSharedPtr<STextBlock> StatusText;
	TSharedPtr<STextBlock> ProjectContextText;
	TSharedPtr<STextBlock> VRContextText;

	ECopilotConnectionStatus CurrentConnectionStatus = ECopilotConnectionStatus::Disconnected;
	FCopilotDiffPreview CurrentDiffPreview;
	FString ChatTranscriptBuffer;
	FString LogBuffer;
	TMap<FString, ECopilotCommandType> PendingCommandTypes;
	TMap<FString, FString> PendingPromptByRequestId;
	TSet<FString> PendingBackendRequestIds;
	TArray<FString> PendingUploadPaths;
	bool bThinkingVisible = false;
	FString ConversationId; // Persistent across messages in the same chat session

	// Auth & model state
	TSharedPtr<STextBlock> AuthStatusText;
	TSharedPtr<STextBlock> DeviceCodeText;
	TArray<TSharedPtr<FString>> ModelOptions;
	TSharedPtr<FString> SelectedModelOption;
	TSharedPtr<SComboBox<TSharedPtr<FString>>> ModelComboBox;

	// Delegate handles for safe unbinding on destroy
	FDelegateHandle ResponseDelegateHandle;
	FDelegateHandle RouterLogDelegateHandle;
	FDelegateHandle ConnectionStatusDelegateHandle;
	FDelegateHandle BridgeLogDelegateHandle;
	FDelegateHandle DeviceCodeDelegateHandle;
	FDelegateHandle AuthCompleteDelegateHandle;
	FDelegateHandle ModelsLoadedDelegateHandle;
	FDelegateHandle ActiveModelChangedDelegateHandle;
	FDelegateHandle ToolActivityDelegateHandle;
};
