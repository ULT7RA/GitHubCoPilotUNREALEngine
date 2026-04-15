// Copyright GitHub, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GitHubCopilotUETypes.h"
#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "Dom/JsonValue.h"

class FGitHubCopilotUEToolExecutor;

// GitHub Copilot OAuth and API endpoints (static auth endpoints only)
namespace CopilotEndpoints
{
	static const FString DeviceCodeURL      = TEXT("https://github.com/login/device/code");
	static const FString AccessTokenURL     = TEXT("https://github.com/login/oauth/access_token");
	static const FString CopilotTokenURL    = TEXT("https://api.github.com/copilot_internal/v2/token");
	static const FString GraphQLURL         = TEXT("https://api.github.com/graphql");
	static const FString DefaultAPIBase     = TEXT("https://api.githubcopilot.com");
	static const FString ClientID           = TEXT("Iv1.b507a08c87ecfe98");
}

/** Represents a model available in the user's Copilot subscription */
struct FCopilotModel
{
	FString Id;               // API model ID to use in requests (e.g. "claude-opus-4.6")
	FString DisplayName;      // Human-readable name (e.g. "Claude Opus 4.6")
	FString Vendor;           // "Anthropic", "OpenAI", "Google", "xAI", etc.
	FString Family;           // Model family (e.g. "gpt-4o", "claude-opus-4.6")
	FString Category;         // "versatile", "powerful", "lightweight", "fast"

	// Capabilities
	bool bSupportsToolCalls = false;
	bool bSupportsParallelToolCalls = false;
	bool bSupportsStreaming = true;
	bool bSupportsVision = false;
	bool bSupportsStructuredOutput = false;

	// Endpoint routing
	bool bSupportsChatCompletions = true;
	bool bSupportsResponses = false;   // /responses endpoint (Codex models)
	bool bSupportsMessages = false;    // /v1/messages (Anthropic native)

	// Token limits (from /models)
	int32 MaxContextWindowTokens = 0;
	int32 MaxPromptTokens = 0;
	int32 MaxOutputTokens = 0;

	// Billing
	float PremiumMultiplier = 0.0f;    // 0 = included/free, 1.0 = standard premium, 3.0 = expensive
	bool bIsPremium = false;

	// UI/picker
	bool bIsPickerEnabled = true;
	bool bIsChatDefault = false;       // true = used when no model specified
	bool bIsChatFallback = false;      // true = fallback when requested model unavailable
	bool bIsEnabled = true;            // policy.state == "enabled"
	bool bIsPreview = false;

	/** Get the correct endpoint path for this model */
	FString GetEndpointPath() const
	{
		// ALWAYS prefer /chat/completions when the model supports it,
		// since our request body is built for that format.
		if (bSupportsChatCompletions)
		{
			return TEXT("/chat/completions");
		}
		if (bSupportsResponses)
		{
			return TEXT("/responses");
		}
		return TEXT("/chat/completions");
	}

	/** True when the model requires /responses endpoint format */
	bool RequiresResponsesFormat() const
	{
		return !bSupportsChatCompletions && bSupportsResponses;
	}
};

/** Auth state for the device code flow */
enum class ECopilotAuthState : uint8
{
	NotAuthenticated,
	WaitingForUserCode,
	PollingForToken,
	Authenticated,
	Error
};

DECLARE_MULTICAST_DELEGATE_TwoParams(FOnCopilotDeviceCode, const FString& /*UserCode*/, const FString& /*VerificationURI*/);
DECLARE_MULTICAST_DELEGATE(FOnCopilotAuthComplete);
DECLARE_MULTICAST_DELEGATE_OneParam(FOnCopilotModelsLoaded, const TArray<FCopilotModel>& /*Models*/);
DECLARE_MULTICAST_DELEGATE_OneParam(FOnCopilotActiveModelChanged, const FString& /*ModelId*/);
DECLARE_MULTICAST_DELEGATE_OneParam(FOnCopilotToolActivity, const FString& /*ActivityMessage*/);

/**
 * Bridge service implementing real GitHub Copilot API integration.
 * Handles: Device Code OAuth -> GitHub token -> Copilot token -> chat completions.
 */
class GITHUBCOPILOTUE_API FGitHubCopilotUEBridgeService
{
public:
	FGitHubCopilotUEBridgeService();
	~FGitHubCopilotUEBridgeService();

	void Initialize();
	void Shutdown();

	// === Authentication ===
	/** Start the GitHub Device Code OAuth flow */
	void StartDeviceCodeAuth();

	/** Check if we're authenticated */
	bool IsAuthenticated() const;

	/** Get current auth state */
	ECopilotAuthState GetAuthState() const { return AuthState; }

	/** Sign out and clear tokens */
	void SignOut();

	/** Get the authenticated GitHub username */
	FString GetUsername() const { return GitHubUsername; }

	// === Models ===
	/** Fetch available models from the user's Copilot subscription */
	void FetchAvailableModels();

	/** Get cached models list */
	const TArray<FCopilotModel>& GetAvailableModels() const { return AvailableModels; }

	/** Set the active model for chat completions */
	void SetActiveModel(const FString& ModelId);

	/** Get the active model ID */
	FString GetActiveModel() const { return ActiveModelId; }

	/** Get the active model struct (or nullptr if not found) */
	const FCopilotModel* GetActiveModelInfo() const;

	/** Get the user's subscription tier (from token exchange sku field) */
	FString GetSubscriptionSku() const { return CopilotSku; }

	/** Get the discovered API base URL */
	FString GetAPIBase() const { return CopilotAPIBase; }

	/** Set the tool executor for agentic tool-calling */
	void SetToolExecutor(TSharedPtr<FGitHubCopilotUEToolExecutor> InToolExecutor) { ToolExecutor = InToolExecutor; }

	/** Clear a persistent conversation (e.g. when user clicks "Clear Chat") */
	void ClearConversation(const FString& ConversationId);

	/**
	 * Get or create a persistent conversation ID.
	 * If one already exists from a previous panel session, returns it.
	 * Otherwise creates a new one. This ensures conversation survives panel recreation.
	 */
	FString GetOrCreateConversationId();

	/** Reset the current conversation ID (used by Clear Chat) and return the new one */
	FString ResetConversationId();

	/** Get the current conversation's message count (for diagnostics) */
	int32 GetConversationMessageCount(const FString& ConversationId) const;

	/** Save conversation history to disk so it survives editor restarts */
	void SaveConversationCache();

	/** Load conversation history from disk */
	void LoadConversationCache();

	/** Save the UI chat transcript alongside conversation data */
	void SetChatTranscript(const FString& Transcript);

	/** Get the saved UI chat transcript */
	FString GetChatTranscript() const;

	/** Get the path to the conversation cache file */
	FString GetConversationCachePath() const;

	/** Save auth + model selection to disk */
	void SaveTokenCache();

	// === Chat ===
	/** Send a chat completion request to Copilot */
	void SendRequest(const FCopilotRequest& Request);

	/** Get connection status */
	ECopilotConnectionStatus GetConnectionStatus() const;

	void Connect();
	void Disconnect();
	void TestConnection();

	// === Delegates ===
	FOnCopilotResponseReceived OnResponseReceived;
	FOnCopilotConnectionStatusChanged OnConnectionStatusChanged;
	FOnCopilotLogMessage OnLogMessage;
	FOnCopilotDeviceCode OnDeviceCodeReceived;
	FOnCopilotAuthComplete OnAuthComplete;
	FOnCopilotModelsLoaded OnModelsLoaded;
	FOnCopilotActiveModelChanged OnActiveModelChanged;
	FOnCopilotToolActivity OnToolActivity;

private:
	// === Auth flow internals ===
	void OnDeviceCodeResponse(FHttpRequestPtr HttpReq, FHttpResponsePtr HttpResp, bool bSuccess);
	void PollForAccessToken();
	void OnAccessTokenResponse(FHttpRequestPtr HttpReq, FHttpResponsePtr HttpResp, bool bSuccess);
	void ExchangeForCopilotToken();
	void OnCopilotTokenResponse(FHttpRequestPtr HttpReq, FHttpResponsePtr HttpResp, bool bSuccess);
	void FetchGitHubUser();
	void OnGitHubUserResponse(FHttpRequestPtr HttpReq, FHttpResponsePtr HttpResp, bool bSuccess);
	bool IsCopilotTokenExpired() const;
	void RefreshCopilotTokenIfNeeded();
	void FlushQueuedRequests();
	void FlushQueuedRequestsAsFailure(const FString& ErrorMessage);

	// === Endpoint Discovery ===
	void DiscoverAPIEndpoint();
	void OnDiscoverEndpointResponse(FHttpRequestPtr HttpReq, FHttpResponsePtr HttpResp, bool bSuccess);
	FString GetModelsURL() const;
	FString GetChatCompletionsURL() const;
	FString GetEndpointURLForModel(const FString& ModelId) const;

	// === Models ===
	void OnModelsResponse(FHttpRequestPtr HttpReq, FHttpResponsePtr HttpResp, bool bSuccess);

	// === Chat ===
	void SendChatCompletion(const FCopilotRequest& Request, bool bAllowToolCalls = true);
	void OnChatCompletionResponse(FHttpRequestPtr HttpReq, FHttpResponsePtr HttpResp, bool bSuccess, FString RequestId, FString ConversationId);
	FString BuildSystemPrompt(const FCopilotRequest& Request) const;
	FString CommandTypeToString(ECopilotCommandType Type) const;

	void SetConnectionStatus(ECopilotConnectionStatus NewStatus);
	void SetAuthState(ECopilotAuthState NewState);
	void CheckForTimeouts();
	void Log(const FString& Message);

	// === State ===
	ECopilotConnectionStatus ConnectionStatus = ECopilotConnectionStatus::Disconnected;
	ECopilotAuthState AuthState = ECopilotAuthState::NotAuthenticated;

	// OAuth tokens
	FString GitHubAccessToken;
	FString CopilotToken;
	double CopilotTokenExpiry = 0.0;
	double CopilotTokenRefreshIn = 0.0;   // seconds from issuance when we should refresh
	double CopilotTokenIssuedAt = 0.0;    // when token was obtained
	FString GitHubUsername;

	// Subscription info (from token exchange)
	FString CopilotSku;                   // e.g. "copilot_for_business", "copilot_pro"
	bool bCopilotIndividual = false;      // individual flag from token exchange
	bool bChatEnabled = false;            // chat_enabled from token exchange

	// Discovered API endpoint (from token exchange endpoints.api)
	FString CopilotAPIBase;

	// Device code flow state
	FString DeviceCode;
	int32 PollInterval = 5;
	FTimerHandle PollTimerHandle;

	// Models
	TArray<FCopilotModel> AvailableModels;
	FString ActiveModelId;

	// Request tracking
	TMap<FString, double> PendingRequestTimestamps;
	TMap<FString, int32> NoResponseRetryCounts;

	// Queued requests waiting for token refresh
	TArray<TPair<FCopilotRequest, bool>> QueuedRequestsAwaitingToken;
	bool bIsRefreshingToken = false;

	// Agentic tool-calling state
	TSharedPtr<FGitHubCopilotUEToolExecutor> ToolExecutor;
	TMap<FString, TArray<TSharedPtr<FJsonValue>>> ActiveConversations; // ConversationId -> messages array (persistent across turns)
	TMap<FString, int32> ToolCallIterations; // RequestId -> iteration count (safety limit)
	TSet<FString> ForcedFinalResponseRequestIds; // Requests that already switched to no-tool finalization
	TMap<FString, int32> LengthContinuationCounts; // RequestId -> how many times we auto-continued on finish_reason=length
	TMap<FString, FString> AccumulatedLengthContent; // RequestId -> accumulated partial content from length continuations

	// Persistent conversation ID — survives panel recreation
	FString CurrentConversationId;

	// Cached UI transcript (saved/loaded alongside conversation)
	FString CachedChatTranscript;

	// Token persistence path
	FString GetTokenCachePath() const;
	void LoadTokenCache();
};
