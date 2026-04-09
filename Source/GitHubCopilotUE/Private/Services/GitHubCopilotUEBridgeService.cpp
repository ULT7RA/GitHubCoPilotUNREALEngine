// Copyright GitHub, Inc. All Rights Reserved.

#include "Services/GitHubCopilotUEBridgeService.h"
#include "Services/GitHubCopilotUEToolExecutor.h"
#include "GitHubCopilotUESettings.h"
#include "HttpModule.h"
#include "Interfaces/IHttpResponse.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Misc/DateTime.h"
#include "Misc/Base64.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/PlatformProcess.h"
#include "GenericPlatform/GenericPlatformHttp.h"

namespace
{
constexpr int32 MaxNoResponseRetries = 2;

FString ResolveAttachmentMimeType(const FCopilotAttachment& Attachment)
{
	if (!Attachment.MimeType.IsEmpty())
	{
		return Attachment.MimeType;
	}

	const FString Extension = FPaths::GetExtension(Attachment.FilePath, false).ToLower();
	if (Extension == TEXT("png")) return TEXT("image/png");
	if (Extension == TEXT("jpg") || Extension == TEXT("jpeg")) return TEXT("image/jpeg");
	if (Extension == TEXT("webp")) return TEXT("image/webp");
	if (Extension == TEXT("gif")) return TEXT("image/gif");
	if (Extension == TEXT("bmp")) return TEXT("image/bmp");
	if (Extension == TEXT("txt")) return TEXT("text/plain");
	if (Extension == TEXT("md")) return TEXT("text/markdown");
	if (Extension == TEXT("json")) return TEXT("application/json");
	if (Extension == TEXT("ini")) return TEXT("text/plain");
	if (Extension == TEXT("cpp") || Extension == TEXT("h") || Extension == TEXT("hpp") || Extension == TEXT("cs"))
	{
		return TEXT("text/plain");
	}

	return TEXT("application/octet-stream");
}

bool IsLikelyTextAttachment(const FString& FilePath)
{
	const FString Extension = FPaths::GetExtension(FilePath, false).ToLower();
	return Extension == TEXT("txt")
		|| Extension == TEXT("md")
		|| Extension == TEXT("json")
		|| Extension == TEXT("ini")
		|| Extension == TEXT("log")
		|| Extension == TEXT("cpp")
		|| Extension == TEXT("h")
		|| Extension == TEXT("hpp")
		|| Extension == TEXT("cs")
		|| Extension == TEXT("py")
		|| Extension == TEXT("js")
		|| Extension == TEXT("ts");
}

bool IsImageMimeType(const FString& MimeType)
{
	return MimeType.StartsWith(TEXT("image/"));
}
} // namespace

FGitHubCopilotUEBridgeService::FGitHubCopilotUEBridgeService()
{
}

FGitHubCopilotUEBridgeService::~FGitHubCopilotUEBridgeService()
{
	Shutdown();
}

void FGitHubCopilotUEBridgeService::Initialize()
{
	Log(TEXT("BridgeService: Initializing GitHub Copilot integration..."));
	LoadTokenCache();
	LoadConversationCache();

	if (!GitHubAccessToken.IsEmpty())
	{
		Log(TEXT("BridgeService: Found cached GitHub token, verifying..."));
		ExchangeForCopilotToken();
	}
	else
	{
		Log(TEXT("BridgeService: No cached token. Click 'Sign in with GitHub' to authenticate."));
	UE_LOG(LogGitHubCopilotUE, Display, TEXT("GitHub Copilot: Not signed in. Use /login to authenticate."));
		SetConnectionStatus(ECopilotConnectionStatus::Disconnected);
	}
}

void FGitHubCopilotUEBridgeService::Shutdown()
{
	Log(TEXT("BridgeService: Shutting down..."));
	SaveConversationCache();
	PendingRequestTimestamps.Empty();
	NoResponseRetryCounts.Empty();
}

// ============================================================================
// Token cache persistence
// ============================================================================

FString FGitHubCopilotUEBridgeService::GetTokenCachePath() const
{
	return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("CopilotAuth.json"));
}

void FGitHubCopilotUEBridgeService::SaveTokenCache()
{
	TSharedPtr<FJsonObject> Json = MakeShareable(new FJsonObject);
	Json->SetStringField(TEXT("access_token"), GitHubAccessToken);
	Json->SetStringField(TEXT("username"), GitHubUsername);
	Json->SetStringField(TEXT("active_model"), ActiveModelId);
	Json->SetStringField(TEXT("api_endpoint"), CopilotAPIBase);
	Json->SetStringField(TEXT("sku"), CopilotSku);

	FString Output;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Output);
	FJsonSerializer::Serialize(Json.ToSharedRef(), Writer);

	FFileHelper::SaveStringToFile(Output, *GetTokenCachePath());
	Log(TEXT("BridgeService: Token cache saved"));
}

void FGitHubCopilotUEBridgeService::LoadTokenCache()
{
	FString Content;
	if (!FFileHelper::LoadFileToString(Content, *GetTokenCachePath()))
	{
		return;
	}

	TSharedPtr<FJsonObject> Json;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Content);
	if (FJsonSerializer::Deserialize(Reader, Json) && Json.IsValid())
	{
		GitHubAccessToken = Json->GetStringField(TEXT("access_token"));
		GitHubUsername = Json->GetStringField(TEXT("username"));
		ActiveModelId = Json->HasField(TEXT("active_model")) ? Json->GetStringField(TEXT("active_model")) : TEXT("");
		CopilotAPIBase = Json->HasField(TEXT("api_endpoint")) ? Json->GetStringField(TEXT("api_endpoint")) : TEXT("");
		CopilotSku = Json->HasField(TEXT("sku")) ? Json->GetStringField(TEXT("sku")) : TEXT("");
		if (!GitHubAccessToken.IsEmpty())
		{
			Log(FString::Printf(TEXT("BridgeService: Loaded cache — user=%s, model=%s, endpoint=%s, sku=%s"),
				*GitHubUsername, *ActiveModelId, *CopilotAPIBase, *CopilotSku));
		}
	}
}

// ============================================================================
// Device Code OAuth Flow
// ============================================================================

void FGitHubCopilotUEBridgeService::StartDeviceCodeAuth()
{
	Log(TEXT("BridgeService: Starting GitHub Device Code authentication..."));
	SetAuthState(ECopilotAuthState::WaitingForUserCode);

	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> HttpReq = FHttpModule::Get().CreateRequest();
	HttpReq->SetURL(CopilotEndpoints::DeviceCodeURL);
	HttpReq->SetVerb(TEXT("POST"));
	HttpReq->SetHeader(TEXT("Accept"), TEXT("application/json"));
	HttpReq->SetHeader(TEXT("Content-Type"), TEXT("application/json"));

	TSharedPtr<FJsonObject> Body = MakeShareable(new FJsonObject);
	Body->SetStringField(TEXT("client_id"), CopilotEndpoints::ClientID);
	Body->SetStringField(TEXT("scope"), TEXT("read:user"));

	FString BodyStr;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&BodyStr);
	FJsonSerializer::Serialize(Body.ToSharedRef(), Writer);
	HttpReq->SetContentAsString(BodyStr);

	HttpReq->OnProcessRequestComplete().BindRaw(this, &FGitHubCopilotUEBridgeService::OnDeviceCodeResponse);
	HttpReq->ProcessRequest();
}

void FGitHubCopilotUEBridgeService::OnDeviceCodeResponse(FHttpRequestPtr HttpReq, FHttpResponsePtr HttpResp, bool bSuccess)
{
	if (!bSuccess || !HttpResp.IsValid() || !EHttpResponseCodes::IsOk(HttpResp->GetResponseCode()))
	{
		Log(TEXT("BridgeService: Failed to get device code from GitHub"));
		SetAuthState(ECopilotAuthState::Error);
		return;
	}

	TSharedPtr<FJsonObject> Json;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(HttpResp->GetContentAsString());
	if (!FJsonSerializer::Deserialize(Reader, Json) || !Json.IsValid())
	{
		Log(TEXT("BridgeService: Failed to parse device code response"));
		SetAuthState(ECopilotAuthState::Error);
		return;
	}

	DeviceCode = Json->GetStringField(TEXT("device_code"));
	FString UserCode = Json->GetStringField(TEXT("user_code"));
	FString VerificationURI = Json->GetStringField(TEXT("verification_uri"));
	PollInterval = Json->GetIntegerField(TEXT("interval"));
	if (PollInterval < 5) PollInterval = 5;

	Log(FString::Printf(TEXT("BridgeService: Go to %s and enter code: %s"), *VerificationURI, *UserCode));

	// Open browser automatically
	FPlatformProcess::LaunchURL(*VerificationURI, nullptr, nullptr);

	// Fire delegate so UI can display the code
	OnDeviceCodeReceived.Broadcast(UserCode, VerificationURI);

	// Start polling for the access token
	SetAuthState(ECopilotAuthState::PollingForToken);
	PollForAccessToken();
}

void FGitHubCopilotUEBridgeService::PollForAccessToken()
{
	if (AuthState != ECopilotAuthState::PollingForToken)
	{
		return;
	}

	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> HttpReq = FHttpModule::Get().CreateRequest();
	HttpReq->SetURL(CopilotEndpoints::AccessTokenURL);
	HttpReq->SetVerb(TEXT("POST"));
	HttpReq->SetHeader(TEXT("Accept"), TEXT("application/json"));
	HttpReq->SetHeader(TEXT("Content-Type"), TEXT("application/json"));

	TSharedPtr<FJsonObject> Body = MakeShareable(new FJsonObject);
	Body->SetStringField(TEXT("client_id"), CopilotEndpoints::ClientID);
	Body->SetStringField(TEXT("device_code"), DeviceCode);
	Body->SetStringField(TEXT("grant_type"), TEXT("urn:ietf:params:oauth:grant-type:device_code"));

	FString BodyStr;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&BodyStr);
	FJsonSerializer::Serialize(Body.ToSharedRef(), Writer);
	HttpReq->SetContentAsString(BodyStr);

	HttpReq->OnProcessRequestComplete().BindRaw(this, &FGitHubCopilotUEBridgeService::OnAccessTokenResponse);
	HttpReq->ProcessRequest();
}

void FGitHubCopilotUEBridgeService::OnAccessTokenResponse(FHttpRequestPtr HttpReq, FHttpResponsePtr HttpResp, bool bSuccess)
{
	if (!bSuccess || !HttpResp.IsValid())
	{
		Log(TEXT("BridgeService: Token poll request failed"));
		// Retry after interval
		FPlatformProcess::Sleep(0.1f); // Non-blocking handled by next poll
		return;
	}

	TSharedPtr<FJsonObject> Json;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(HttpResp->GetContentAsString());
	if (!FJsonSerializer::Deserialize(Reader, Json) || !Json.IsValid())
	{
		return;
	}

	if (Json->HasField(TEXT("access_token")))
	{
		GitHubAccessToken = Json->GetStringField(TEXT("access_token"));
		Log(TEXT("BridgeService: GitHub OAuth token obtained!"));

		// Save and proceed to get Copilot token
		FetchGitHubUser();
		ExchangeForCopilotToken();
		return;
	}

	FString Error = Json->GetStringField(TEXT("error"));
	if (Error == TEXT("authorization_pending"))
	{
		// User hasn't entered the code yet — schedule another poll
		// Use async delayed call since we can't use UWorld timers from a non-UObject
		float Delay = (float)PollInterval;
		FTSTicker::GetCoreTicker().AddTicker(
			FTickerDelegate::CreateLambda([this](float) -> bool
			{
				PollForAccessToken();
				return false; // Don't repeat
			}),
			Delay
		);
	}
	else if (Error == TEXT("slow_down"))
	{
		PollInterval += 5;
		float Delay = (float)PollInterval;
		FTSTicker::GetCoreTicker().AddTicker(
			FTickerDelegate::CreateLambda([this](float) -> bool
			{
				PollForAccessToken();
				return false;
			}),
			Delay
		);
	}
	else if (Error == TEXT("expired_token"))
	{
		Log(TEXT("BridgeService: Device code expired. Please try signing in again."));
		SetAuthState(ECopilotAuthState::Error);
	}
	else
	{
		Log(FString::Printf(TEXT("BridgeService: Auth error: %s"), *Error));
		SetAuthState(ECopilotAuthState::Error);
	}
}

void FGitHubCopilotUEBridgeService::FetchGitHubUser()
{
	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> HttpReq = FHttpModule::Get().CreateRequest();
	HttpReq->SetURL(TEXT("https://api.github.com/user"));
	HttpReq->SetVerb(TEXT("GET"));
	HttpReq->SetHeader(TEXT("Authorization"), FString::Printf(TEXT("Bearer %s"), *GitHubAccessToken));
	HttpReq->SetHeader(TEXT("Accept"), TEXT("application/json"));
	HttpReq->SetHeader(TEXT("User-Agent"), TEXT("GitHubCopilotUE-Plugin"));
	HttpReq->OnProcessRequestComplete().BindRaw(this, &FGitHubCopilotUEBridgeService::OnGitHubUserResponse);
	HttpReq->ProcessRequest();
}

void FGitHubCopilotUEBridgeService::OnGitHubUserResponse(FHttpRequestPtr HttpReq, FHttpResponsePtr HttpResp, bool bSuccess)
{
	if (bSuccess && HttpResp.IsValid() && EHttpResponseCodes::IsOk(HttpResp->GetResponseCode()))
	{
		TSharedPtr<FJsonObject> Json;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(HttpResp->GetContentAsString());
		if (FJsonSerializer::Deserialize(Reader, Json) && Json.IsValid())
		{
			GitHubUsername = Json->GetStringField(TEXT("login"));
			Log(FString::Printf(TEXT("BridgeService: Signed in as: %s"), *GitHubUsername));
			UE_LOG(LogGitHubCopilotUE, Display, TEXT("GitHub Copilot: Signed in as %s"), *GitHubUsername);
			SaveTokenCache();
		}
	}
}

void FGitHubCopilotUEBridgeService::ExchangeForCopilotToken()
{
	Log(TEXT("BridgeService: Exchanging GitHub token for Copilot token..."));
	SetConnectionStatus(ECopilotConnectionStatus::Connecting);

	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> HttpReq = FHttpModule::Get().CreateRequest();
	HttpReq->SetURL(CopilotEndpoints::CopilotTokenURL);
	HttpReq->SetVerb(TEXT("GET"));
	HttpReq->SetHeader(TEXT("Authorization"), FString::Printf(TEXT("token %s"), *GitHubAccessToken));
	HttpReq->SetHeader(TEXT("Accept"), TEXT("application/json"));
	HttpReq->SetHeader(TEXT("User-Agent"), TEXT("GitHubCopilotUE-Plugin"));
	HttpReq->OnProcessRequestComplete().BindRaw(this, &FGitHubCopilotUEBridgeService::OnCopilotTokenResponse);
	HttpReq->ProcessRequest();
}

void FGitHubCopilotUEBridgeService::OnCopilotTokenResponse(FHttpRequestPtr HttpReq, FHttpResponsePtr HttpResp, bool bSuccess)
{
	bIsRefreshingToken = false;

	if (!bSuccess || !HttpResp.IsValid())
	{
		Log(TEXT("BridgeService: Failed to reach Copilot token endpoint"));
		SetConnectionStatus(ECopilotConnectionStatus::Error);
		SetAuthState(ECopilotAuthState::Error);
		FlushQueuedRequestsAsFailure(TEXT("Token refresh failed — could not reach Copilot token endpoint."));
		return;
	}

	if (!EHttpResponseCodes::IsOk(HttpResp->GetResponseCode()))
	{
		Log(FString::Printf(TEXT("BridgeService: Copilot token request failed (HTTP %d). Do you have an active Copilot subscription?"),
			HttpResp->GetResponseCode()));
		SetConnectionStatus(ECopilotConnectionStatus::Error);
		SetAuthState(ECopilotAuthState::Error);
		FlushQueuedRequestsAsFailure(FString::Printf(TEXT("Token refresh failed (HTTP %d)."), HttpResp->GetResponseCode()));
		return;
	}

	TSharedPtr<FJsonObject> Json;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(HttpResp->GetContentAsString());
	if (!FJsonSerializer::Deserialize(Reader, Json) || !Json.IsValid())
	{
		Log(TEXT("BridgeService: Failed to parse Copilot token response"));
		SetConnectionStatus(ECopilotConnectionStatus::Error);
		FlushQueuedRequestsAsFailure(TEXT("Token refresh failed — bad response from token endpoint."));
		return;
	}

	CopilotToken = Json->GetStringField(TEXT("token"));
	int64 ExpiresAt = (int64)Json->GetNumberField(TEXT("expires_at"));
	CopilotTokenExpiry = (double)ExpiresAt;
	CopilotTokenIssuedAt = FPlatformTime::Seconds();

	// Parse refresh_in for proactive refresh
	if (Json->HasField(TEXT("refresh_in")))
	{
		CopilotTokenRefreshIn = Json->GetNumberField(TEXT("refresh_in"));
	}
	else
	{
		CopilotTokenRefreshIn = 1200.0; // default 20 min
	}

	// Parse subscription metadata
	if (Json->HasField(TEXT("sku")))
	{
		CopilotSku = Json->GetStringField(TEXT("sku"));
	}
	if (Json->HasField(TEXT("individual")))
	{
		bCopilotIndividual = Json->GetBoolField(TEXT("individual"));
	}
	if (Json->HasField(TEXT("chat_enabled")))
	{
		bChatEnabled = Json->GetBoolField(TEXT("chat_enabled"));
	}

	// Extract API endpoint directly from token exchange response (most reliable source)
	const TSharedPtr<FJsonObject>* EndpointsObj;
	if (Json->TryGetObjectField(TEXT("endpoints"), EndpointsObj))
	{
		FString TokenAPIEndpoint = (*EndpointsObj)->GetStringField(TEXT("api"));
		if (!TokenAPIEndpoint.IsEmpty())
		{
			CopilotAPIBase = TokenAPIEndpoint;
			Log(FString::Printf(TEXT("BridgeService: API endpoint from token exchange: %s"), *CopilotAPIBase));
		}
	}

	Log(FString::Printf(TEXT("BridgeService: Copilot token obtained (sku=%s, chat=%s, refresh_in=%.0fs)"),
		*CopilotSku, bChatEnabled ? TEXT("yes") : TEXT("no"), CopilotTokenRefreshIn));
	SaveTokenCache();

	SetAuthState(ECopilotAuthState::Authenticated);
	SetConnectionStatus(ECopilotConnectionStatus::Connected);

	OnAuthComplete.Broadcast();

	// Flush any requests that were waiting for the token refresh
	FlushQueuedRequests();

	// If we already got the endpoint from token exchange, skip GraphQL and go straight to models
	if (!CopilotAPIBase.IsEmpty())
	{
		FetchAvailableModels();
	}
	else
	{
		// Fallback: discover via GraphQL
		DiscoverAPIEndpoint();
	}
}

bool FGitHubCopilotUEBridgeService::IsCopilotTokenExpired() const
{
	if (CopilotToken.IsEmpty()) return true;
	double Now = FPlatformTime::Seconds();

	// Method 1: Check refresh_in elapsed since issuance
	if (CopilotTokenIssuedAt > 0.0 && CopilotTokenRefreshIn > 0.0)
	{
		double SecondsSinceIssued = Now - CopilotTokenIssuedAt;
		if (SecondsSinceIssued >= CopilotTokenRefreshIn)
		{
			return true;
		}
	}

	// Method 2: Check absolute expires_at (with 60s buffer)
	double NowUnix = FDateTime::UtcNow().ToUnixTimestamp();
	return NowUnix >= (CopilotTokenExpiry - 60.0);
}

void FGitHubCopilotUEBridgeService::RefreshCopilotTokenIfNeeded()
{
	if (IsCopilotTokenExpired() && !GitHubAccessToken.IsEmpty() && !bIsRefreshingToken)
	{
		Log(TEXT("BridgeService: Copilot token expired, refreshing..."));
		bIsRefreshingToken = true;
		ExchangeForCopilotToken();
	}
}

void FGitHubCopilotUEBridgeService::FlushQueuedRequests()
{
	if (QueuedRequestsAwaitingToken.Num() == 0) return;

	Log(FString::Printf(TEXT("BridgeService: Flushing %d queued requests after token refresh"), QueuedRequestsAwaitingToken.Num()));
	TArray<TPair<FCopilotRequest, bool>> Queued = MoveTemp(QueuedRequestsAwaitingToken);
	QueuedRequestsAwaitingToken.Empty();

	for (const auto& Pair : Queued)
	{
		PendingRequestTimestamps.Add(Pair.Key.RequestId, FPlatformTime::Seconds());
		NoResponseRetryCounts.Remove(Pair.Key.RequestId);
		SendChatCompletion(Pair.Key, Pair.Value);
	}
}

void FGitHubCopilotUEBridgeService::FlushQueuedRequestsAsFailure(const FString& ErrorMessage)
{
	if (QueuedRequestsAwaitingToken.Num() == 0) return;

	Log(FString::Printf(TEXT("BridgeService: Failing %d queued requests: %s"), QueuedRequestsAwaitingToken.Num(), *ErrorMessage));
	TArray<TPair<FCopilotRequest, bool>> Queued = MoveTemp(QueuedRequestsAwaitingToken);
	QueuedRequestsAwaitingToken.Empty();

	for (const auto& Pair : Queued)
	{
		FCopilotResponse Resp;
		Resp.RequestId = Pair.Key.RequestId;
		Resp.ResultStatus = ECopilotResultStatus::Failure;
		Resp.ErrorMessage = ErrorMessage;
		OnResponseReceived.Broadcast(Resp);
	}
}

bool FGitHubCopilotUEBridgeService::IsAuthenticated() const
{
	return AuthState == ECopilotAuthState::Authenticated && !CopilotToken.IsEmpty();
}

void FGitHubCopilotUEBridgeService::SignOut()
{
	GitHubAccessToken.Empty();
	CopilotToken.Empty();
	GitHubUsername.Empty();
	CopilotTokenExpiry = 0.0;
	AvailableModels.Empty();
	ActiveModelId.Empty();

	// Delete cached token
	IFileManager::Get().Delete(*GetTokenCachePath());

	SetAuthState(ECopilotAuthState::NotAuthenticated);
	SetConnectionStatus(ECopilotConnectionStatus::Disconnected);
	Log(TEXT("BridgeService: Signed out"));
}

// ============================================================================
// Endpoint Discovery (GraphQL)
// ============================================================================

FString FGitHubCopilotUEBridgeService::GetModelsURL() const
{
	FString Base = CopilotAPIBase.IsEmpty() ? CopilotEndpoints::DefaultAPIBase : CopilotAPIBase;
	return Base + TEXT("/models");
}

FString FGitHubCopilotUEBridgeService::GetChatCompletionsURL() const
{
	FString Base = CopilotAPIBase.IsEmpty() ? CopilotEndpoints::DefaultAPIBase : CopilotAPIBase;
	return Base + TEXT("/chat/completions");
}

FString FGitHubCopilotUEBridgeService::GetEndpointURLForModel(const FString& ModelId) const
{
	FString Base = CopilotAPIBase.IsEmpty() ? CopilotEndpoints::DefaultAPIBase : CopilotAPIBase;

	// Check if this model needs a different endpoint (e.g. Codex → /responses)
	for (const FCopilotModel& M : AvailableModels)
	{
		if (M.Id == ModelId)
		{
			return Base + M.GetEndpointPath();
		}
	}

	// Default to /chat/completions
	return Base + TEXT("/chat/completions");
}

const FCopilotModel* FGitHubCopilotUEBridgeService::GetActiveModelInfo() const
{
	for (const FCopilotModel& M : AvailableModels)
	{
		if (M.Id == ActiveModelId)
		{
			return &M;
		}
	}
	return nullptr;
}

void FGitHubCopilotUEBridgeService::DiscoverAPIEndpoint()
{
	if (GitHubAccessToken.IsEmpty())
	{
		Log(TEXT("BridgeService: No GitHub token for endpoint discovery, using default"));
		FetchAvailableModels();
		return;
	}

	Log(TEXT("BridgeService: Discovering API endpoint via GraphQL..."));

	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> HttpReq = FHttpModule::Get().CreateRequest();
	HttpReq->SetURL(CopilotEndpoints::GraphQLURL);
	HttpReq->SetVerb(TEXT("POST"));
	HttpReq->SetHeader(TEXT("Authorization"), FString::Printf(TEXT("Bearer %s"), *GitHubAccessToken));
	HttpReq->SetHeader(TEXT("Content-Type"), TEXT("application/json"));

	TSharedPtr<FJsonObject> Body = MakeShareable(new FJsonObject);
	Body->SetStringField(TEXT("query"), TEXT("query { viewer { copilotEndpoints { api } } }"));
	FString BodyStr;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&BodyStr);
	FJsonSerializer::Serialize(Body.ToSharedRef(), Writer);
	HttpReq->SetContentAsString(BodyStr);

	HttpReq->OnProcessRequestComplete().BindRaw(this, &FGitHubCopilotUEBridgeService::OnDiscoverEndpointResponse);
	HttpReq->ProcessRequest();
}

void FGitHubCopilotUEBridgeService::OnDiscoverEndpointResponse(FHttpRequestPtr HttpReq, FHttpResponsePtr HttpResp, bool bSuccess)
{
	if (bSuccess && HttpResp.IsValid() && EHttpResponseCodes::IsOk(HttpResp->GetResponseCode()))
	{
		TSharedPtr<FJsonObject> Json;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(HttpResp->GetContentAsString());
		if (FJsonSerializer::Deserialize(Reader, Json) && Json.IsValid())
		{
			const TSharedPtr<FJsonObject>* Data;
			if (Json->TryGetObjectField(TEXT("data"), Data))
			{
				const TSharedPtr<FJsonObject>* Viewer;
				if ((*Data)->TryGetObjectField(TEXT("viewer"), Viewer))
				{
					const TSharedPtr<FJsonObject>* Endpoints;
					if ((*Viewer)->TryGetObjectField(TEXT("copilotEndpoints"), Endpoints))
					{
						CopilotAPIBase = (*Endpoints)->GetStringField(TEXT("api"));
						Log(FString::Printf(TEXT("BridgeService: Discovered API endpoint via GraphQL: %s"), *CopilotAPIBase));
						SaveTokenCache();
					}
				}
			}
		}
	}

	if (CopilotAPIBase.IsEmpty())
	{
		CopilotAPIBase = CopilotEndpoints::DefaultAPIBase;
		Log(FString::Printf(TEXT("BridgeService: Using default API endpoint: %s"), *CopilotAPIBase));
	}

	// Now fetch models from the correct endpoint
	FetchAvailableModels();
}

// ============================================================================
// Models
// ============================================================================

void FGitHubCopilotUEBridgeService::FetchAvailableModels()
{
	if (!IsAuthenticated())
	{
		Log(TEXT("BridgeService: Cannot fetch models - not authenticated"));
		return;
	}

	RefreshCopilotTokenIfNeeded();

	FString URL = GetModelsURL();
	Log(FString::Printf(TEXT("BridgeService: Fetching models from %s"), *URL));

	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> HttpReq = FHttpModule::Get().CreateRequest();
	HttpReq->SetURL(URL);
	HttpReq->SetVerb(TEXT("GET"));
	HttpReq->SetHeader(TEXT("Authorization"), FString::Printf(TEXT("Bearer %s"), *CopilotToken));
	HttpReq->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
	HttpReq->SetHeader(TEXT("Accept"), TEXT("application/json"));
	HttpReq->SetHeader(TEXT("Copilot-Integration-Id"), TEXT("vscode-chat"));
	HttpReq->SetHeader(TEXT("Editor-Version"), TEXT("vscode/1.103.2"));
	HttpReq->SetHeader(TEXT("Editor-Plugin-Version"), TEXT("copilot-chat/0.27.1"));
	HttpReq->SetHeader(TEXT("User-Agent"), TEXT("GitHubCopilotChat/0.27.1"));
	HttpReq->SetHeader(TEXT("X-GitHub-Api-Version"), TEXT("2025-05-01"));
	HttpReq->OnProcessRequestComplete().BindRaw(this, &FGitHubCopilotUEBridgeService::OnModelsResponse);
	HttpReq->ProcessRequest();
}

void FGitHubCopilotUEBridgeService::OnModelsResponse(FHttpRequestPtr HttpReq, FHttpResponsePtr HttpResp, bool bSuccess)
{
	if (!bSuccess || !HttpResp.IsValid() || !EHttpResponseCodes::IsOk(HttpResp->GetResponseCode()))
	{
		int32 Code = HttpResp.IsValid() ? HttpResp->GetResponseCode() : 0;
		Log(FString::Printf(TEXT("BridgeService: Failed to fetch models (HTTP %d)"), Code));
		return;
	}

	FString RawBody = HttpResp->GetContentAsString();

	TSharedPtr<FJsonObject> Json;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(RawBody);
	if (!FJsonSerializer::Deserialize(Reader, Json) || !Json.IsValid())
	{
		Log(TEXT("BridgeService: Failed to parse models response"));
		return;
	}

	AvailableModels.Empty();

	const TArray<TSharedPtr<FJsonValue>>* DataArray;
	if (Json->TryGetArrayField(TEXT("data"), DataArray))
	{
		for (const TSharedPtr<FJsonValue>& Val : *DataArray)
		{
			TSharedPtr<FJsonObject> ModelObj = Val->AsObject();
			if (!ModelObj.IsValid()) continue;

			FCopilotModel Model;
			Model.Id = ModelObj->GetStringField(TEXT("id"));
			Model.DisplayName = ModelObj->HasField(TEXT("name"))
				? ModelObj->GetStringField(TEXT("name")) : Model.Id;
			Model.Vendor = ModelObj->HasField(TEXT("vendor"))
				? ModelObj->GetStringField(TEXT("vendor")) : TEXT("unknown");
			Model.Family = ModelObj->HasField(TEXT("family"))
				? ModelObj->GetStringField(TEXT("family")) : Model.Id;

			// model_picker_enabled (or picker_enabled — handle both)
			if (ModelObj->HasField(TEXT("model_picker_enabled")))
				Model.bIsPickerEnabled = ModelObj->GetBoolField(TEXT("model_picker_enabled"));
			else if (ModelObj->HasField(TEXT("picker_enabled")))
				Model.bIsPickerEnabled = ModelObj->GetBoolField(TEXT("picker_enabled"));

			// Skip models not meant for the picker
			if (!Model.bIsPickerEnabled) continue;

			// Preview flag
			if (ModelObj->HasField(TEXT("preview")))
				Model.bIsPreview = ModelObj->GetBoolField(TEXT("preview"));

			// Picker category
			if (ModelObj->HasField(TEXT("model_picker_category")))
				Model.Category = ModelObj->GetStringField(TEXT("model_picker_category"));

			// Default/fallback flags
			if (ModelObj->HasField(TEXT("is_chat_default")))
				Model.bIsChatDefault = ModelObj->GetBoolField(TEXT("is_chat_default"));
			if (ModelObj->HasField(TEXT("is_chat_fallback")))
				Model.bIsChatFallback = ModelObj->GetBoolField(TEXT("is_chat_fallback"));

			// ── Policy ──
			const TSharedPtr<FJsonObject>* PolicyObj;
			if (ModelObj->TryGetObjectField(TEXT("policy"), PolicyObj))
			{
				FString PolicyState = (*PolicyObj)->HasField(TEXT("state"))
					? (*PolicyObj)->GetStringField(TEXT("state")) : TEXT("enabled");
				Model.bIsEnabled = (PolicyState == TEXT("enabled"));
			}

			// Skip disabled models (org admin hasn't enabled them)
			if (!Model.bIsEnabled) continue;

			// ── Billing ──
			const TSharedPtr<FJsonObject>* BillingObj;
			if (ModelObj->TryGetObjectField(TEXT("billing"), BillingObj))
			{
				if ((*BillingObj)->HasField(TEXT("is_premium")))
					Model.bIsPremium = (*BillingObj)->GetBoolField(TEXT("is_premium"));
				if ((*BillingObj)->HasField(TEXT("multiplier")))
					Model.PremiumMultiplier = (float)(*BillingObj)->GetNumberField(TEXT("multiplier"));
			}
			// Also check metadata.premium_multiplier (alternate schema)
			const TSharedPtr<FJsonObject>* MetadataObj;
			if (ModelObj->TryGetObjectField(TEXT("metadata"), MetadataObj))
			{
				if ((*MetadataObj)->HasField(TEXT("premium_multiplier")))
					Model.PremiumMultiplier = (float)(*MetadataObj)->GetNumberField(TEXT("premium_multiplier"));
			}

			// ── Capabilities (two schema variants) ──
			const TSharedPtr<FJsonObject>* Caps;
			if (ModelObj->TryGetObjectField(TEXT("capabilities"), Caps))
			{
				// Schema variant 1: capabilities.supports.tool_calls (detailed)
				const TSharedPtr<FJsonObject>* Supports;
				if ((*Caps)->TryGetObjectField(TEXT("supports"), Supports))
				{
					if ((*Supports)->HasField(TEXT("tool_calls")))
						Model.bSupportsToolCalls = (*Supports)->GetBoolField(TEXT("tool_calls"));
					if ((*Supports)->HasField(TEXT("parallel_tool_calls")))
						Model.bSupportsParallelToolCalls = (*Supports)->GetBoolField(TEXT("parallel_tool_calls"));
					if ((*Supports)->HasField(TEXT("streaming")))
						Model.bSupportsStreaming = (*Supports)->GetBoolField(TEXT("streaming"));
					if ((*Supports)->HasField(TEXT("vision")))
						Model.bSupportsVision = (*Supports)->GetBoolField(TEXT("vision"));
					if ((*Supports)->HasField(TEXT("structured_outputs")))
						Model.bSupportsStructuredOutput = (*Supports)->GetBoolField(TEXT("structured_outputs"));
				}

				// Schema variant 2: capabilities.tools (flat boolean)
				if ((*Caps)->HasField(TEXT("tools")))
					Model.bSupportsToolCalls = (*Caps)->GetBoolField(TEXT("tools"));
				if ((*Caps)->HasField(TEXT("chat")))
				{
					// If capabilities.chat exists and is false, skip
					if (!(*Caps)->GetBoolField(TEXT("chat"))) continue;
				}

				// capabilities.type — skip non-chat models
				if ((*Caps)->HasField(TEXT("type")))
				{
					FString ModelType = (*Caps)->GetStringField(TEXT("type"));
					if (!ModelType.IsEmpty() && ModelType != TEXT("chat")) continue;
				}

				// ── Token limits (nested under capabilities.limits or top-level limits) ──
				const TSharedPtr<FJsonObject>* Limits;
				if ((*Caps)->TryGetObjectField(TEXT("limits"), Limits))
				{
					if ((*Limits)->HasField(TEXT("max_context_window_tokens")))
						Model.MaxContextWindowTokens = (int32)(*Limits)->GetNumberField(TEXT("max_context_window_tokens"));
					if ((*Limits)->HasField(TEXT("max_prompt_tokens")))
						Model.MaxPromptTokens = (int32)(*Limits)->GetNumberField(TEXT("max_prompt_tokens"));
					if ((*Limits)->HasField(TEXT("max_output_tokens")))
						Model.MaxOutputTokens = (int32)(*Limits)->GetNumberField(TEXT("max_output_tokens"));

					// Vision support from limits
					const TSharedPtr<FJsonObject>* VisionObj;
					if ((*Limits)->TryGetObjectField(TEXT("vision"), VisionObj))
					{
						Model.bSupportsVision = true;
					}
				}
			}

			// ── Top-level limits (alternate schema) ──
			const TSharedPtr<FJsonObject>* TopLimits;
			if (ModelObj->TryGetObjectField(TEXT("limits"), TopLimits))
			{
				if ((*TopLimits)->HasField(TEXT("max_context_window_tokens")) && Model.MaxContextWindowTokens == 0)
					Model.MaxContextWindowTokens = (int32)(*TopLimits)->GetNumberField(TEXT("max_context_window_tokens"));
				if ((*TopLimits)->HasField(TEXT("max_prompt_tokens")) && Model.MaxPromptTokens == 0)
					Model.MaxPromptTokens = (int32)(*TopLimits)->GetNumberField(TEXT("max_prompt_tokens"));
				if ((*TopLimits)->HasField(TEXT("max_output_tokens")) && Model.MaxOutputTokens == 0)
					Model.MaxOutputTokens = (int32)(*TopLimits)->GetNumberField(TEXT("max_output_tokens"));
			}

			// Top-level vision flag
			if (ModelObj->HasField(TEXT("vision")))
				Model.bSupportsVision = ModelObj->GetBoolField(TEXT("vision"));

			// ── Supported endpoints ──
			Model.bSupportsChatCompletions = false; // Reset — will set based on actual data
			const TArray<TSharedPtr<FJsonValue>>* SEPArray;
			if (ModelObj->TryGetArrayField(TEXT("supported_endpoints"), SEPArray))
			{
				for (const TSharedPtr<FJsonValue>& EP : *SEPArray)
				{
					FString EPStr = EP->AsString();
					if (EPStr == TEXT("/chat/completions")) Model.bSupportsChatCompletions = true;
					if (EPStr == TEXT("/responses")) Model.bSupportsResponses = true;
					if (EPStr == TEXT("/v1/messages")) Model.bSupportsMessages = true;
				}
			}
			else
			{
				// No supported_endpoints field — assume /chat/completions
				Model.bSupportsChatCompletions = true;
			}

			// Skip models that can't do chat or responses (embeddings-only)
			if (!Model.bSupportsChatCompletions && !Model.bSupportsResponses) continue;

			// Filter out embeddings-only by name heuristic
			if (Model.Id.Contains(TEXT("embed"))) continue;

			AvailableModels.Add(Model);
		}
	}

	const FString PreviousActiveModel = ActiveModelId;
	auto PickFallbackModelId = [this]() -> FString
	{
		// Prefer Claude over GPT if available
		for (const FCopilotModel& M : AvailableModels)
		{
			if (M.Id.Contains(TEXT("claude")))
			{
				return M.Id;
			}
		}

		// Fallback to model marked as chat default
		for (const FCopilotModel& M : AvailableModels)
		{
			if (M.bIsChatDefault)
			{
				return M.Id;
			}
		}

		return AvailableModels.Num() > 0 ? AvailableModels[0].Id : TEXT("");
	};

	if (AvailableModels.Num() > 0)
	{
		bool bActiveModelFound = false;
		if (!ActiveModelId.IsEmpty())
		{
			for (const FCopilotModel& M : AvailableModels)
			{
				if (M.Id.Equals(ActiveModelId, ESearchCase::IgnoreCase))
				{
					ActiveModelId = M.Id; // normalize casing from server list
					bActiveModelFound = true;
					break;
				}
			}
		}

		if (!bActiveModelFound)
		{
			ActiveModelId = PickFallbackModelId();
			if (!PreviousActiveModel.IsEmpty())
			{
				Log(FString::Printf(
					TEXT("BridgeService: Cached active model '%s' is unavailable; falling back to '%s'"),
					*PreviousActiveModel, *ActiveModelId));
			}
		}
	}
	else
	{
		ActiveModelId.Empty();
	}

	Log(FString::Printf(TEXT("BridgeService: Loaded %d models. Active: %s"), AvailableModels.Num(), *ActiveModelId));
	UE_LOG(LogGitHubCopilotUE, Display, TEXT("GitHub Copilot: %d models loaded, using %s"), AvailableModels.Num(), *ActiveModelId);
	for (const FCopilotModel& M : AvailableModels)
	{
		FString Info = FString::Printf(TEXT("  %s | %s | vendor=%s | tools=%s | ctx=%dk | cost=%.1fx%s"),
			*M.Id, *M.Category, *M.Vendor,
			M.bSupportsToolCalls ? TEXT("yes") : TEXT("no"),
			M.MaxContextWindowTokens / 1000,
			M.PremiumMultiplier,
			M.bSupportsResponses && !M.bSupportsChatCompletions ? TEXT(" [/responses only]") : TEXT(""));
		Log(Info);
	}

	if (ActiveModelId != PreviousActiveModel)
	{
		OnActiveModelChanged.Broadcast(ActiveModelId);
		SaveTokenCache();
	}

	OnModelsLoaded.Broadcast(AvailableModels);
}

void FGitHubCopilotUEBridgeService::SetActiveModel(const FString& ModelId)
{
	// Strip display name if user pasted "model-id (Display Name)"
	FString CleanId = ModelId.TrimStartAndEnd();
	int32 ParenIdx;
	if (CleanId.FindChar(TEXT('('), ParenIdx))
	{
		CleanId = CleanId.Left(ParenIdx).TrimStartAndEnd();
	}
	if (CleanId.IsEmpty())
	{
		return;
	}

	FString ResolvedId = CleanId;
	if (AvailableModels.Num() > 0)
	{
		bool bFound = false;
		for (const FCopilotModel& M : AvailableModels)
		{
			if (M.Id.Equals(CleanId, ESearchCase::IgnoreCase) || M.DisplayName.Equals(CleanId, ESearchCase::IgnoreCase))
			{
				ResolvedId = M.Id;
				bFound = true;
				break;
			}
		}

		if (!bFound)
		{
			Log(FString::Printf(TEXT("BridgeService: Ignoring unknown model selection '%s'"), *CleanId));
			return;
		}
	}

	if (ActiveModelId != ResolvedId)
	{
		ActiveModelId = ResolvedId;
		Log(FString::Printf(TEXT("BridgeService: Active model set to: %s"), *ActiveModelId));
		OnActiveModelChanged.Broadcast(ActiveModelId);
	}
}

// ============================================================================
// Chat Completions
// ============================================================================

void FGitHubCopilotUEBridgeService::SendRequest(const FCopilotRequest& Request)
{
	if (!IsAuthenticated())
	{
		FCopilotResponse ErrorResp;
		ErrorResp.RequestId = Request.RequestId;
		ErrorResp.ResultStatus = ECopilotResultStatus::Failure;
		ErrorResp.ErrorMessage = TEXT("Not authenticated. Click 'Sign in with GitHub' first.");
		OnResponseReceived.Broadcast(ErrorResp);
		return;
	}

	// If token is expired, refresh it and queue this request to send when the token arrives
	if (IsCopilotTokenExpired())
	{
		Log(FString::Printf(TEXT("BridgeService: Token expired — queuing request %s while refreshing"), *Request.RequestId));
		QueuedRequestsAwaitingToken.Add(TPair<FCopilotRequest, bool>(Request, true));
		RefreshCopilotTokenIfNeeded();
		return;
	}

	Log(FString::Printf(TEXT("BridgeService: Sending request %s to Copilot (model: %s)"), *Request.RequestId, *ActiveModelId));
	PendingRequestTimestamps.Add(Request.RequestId, FPlatformTime::Seconds());
	NoResponseRetryCounts.Remove(Request.RequestId);

	SendChatCompletion(Request);
}

FString FGitHubCopilotUEBridgeService::CommandTypeToString(ECopilotCommandType Type) const
{
	switch (Type)
	{
	case ECopilotCommandType::AnalyzeProject: return TEXT("Analyze Project");
	case ECopilotCommandType::AnalyzeSelection: return TEXT("Analyze Selection");
	case ECopilotCommandType::ExplainCode: return TEXT("Explain Code");
	case ECopilotCommandType::SuggestRefactor: return TEXT("Suggest Refactor");
	case ECopilotCommandType::CreateCppClass: return TEXT("Generate C++ Class");
	case ECopilotCommandType::CreateActorComponent: return TEXT("Generate Actor Component");
	case ECopilotCommandType::CreateBlueprintFunctionLibrary: return TEXT("Generate Blueprint Function Library");
	case ECopilotCommandType::GenerateEditorUtilityHelper: return TEXT("Generate Editor Utility Helper");
	case ECopilotCommandType::PatchFile: return TEXT("Patch File");
	default: return TEXT("General Request");
	}
}

FString FGitHubCopilotUEBridgeService::BuildSystemPrompt(const FCopilotRequest& Request) const
{
	FString EngineVer = Request.ProjectContext.EngineVersion.IsEmpty() ? TEXT("5.x") : Request.ProjectContext.EngineVersion;
	FString ProjectName = Request.ProjectContext.ProjectName.IsEmpty() ? TEXT("Unknown") : Request.ProjectContext.ProjectName;
	FString ProjectDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());

	FString Prompt = FString::Printf(
		TEXT("You are inside Unreal Engine %s, project: %s, directory: %s.\n")
		TEXT("This is a persistent multi-turn conversation.\n")
		TEXT("You have tools available. Use them proactively instead of asking the user to check things.\n\n")
		TEXT("STRICT FORMATTING RULES — NEVER BREAK THESE:\n")
		TEXT("- NEVER use markdown. No headers (#), no bold (**), no code fences (```), no bullet points (- or *).\n")
		TEXT("- NEVER use emojis. Not a single one.\n")
		TEXT("- Write in plain conversational English like a coworker talking to you. No lists, no bullet points, no structured formatting.\n")
		TEXT("- Just talk normally. Explain things in sentences and paragraphs."),
		*EngineVer, *ProjectName, *ProjectDir);

	return Prompt;
}

void FGitHubCopilotUEBridgeService::SendChatCompletion(const FCopilotRequest& Request, bool bAllowToolCalls)
{
	// Ensure we have a valid model
	FString ModelToUse = ActiveModelId;
	if (ModelToUse.IsEmpty() && AvailableModels.Num() > 0)
	{
		ModelToUse = AvailableModels[0].Id;
	}
	else if (ModelToUse.IsEmpty())
	{
		ModelToUse = TEXT("claude-sonnet-4");
	}

	Log(FString::Printf(TEXT("BridgeService: Chat completion using model: %s"), *ModelToUse));

	// ConversationId MUST be set. BridgeService owns the canonical conversation.
	// If the request somehow arrives without one, use the one we own — never generate a throwaway key.
	FString ConvoKey = Request.ConversationId;
	if (ConvoKey.IsEmpty())
	{
		ConvoKey = GetOrCreateConversationId();
		Log(FString::Printf(TEXT("BridgeService: [CONVO] WARNING — Request arrived with empty ConversationId! Forced to CurrentConversationId: %s"), *ConvoKey));
	}

	Log(FString::Printf(TEXT("BridgeService: [CONVO] ConversationId: %s"), *ConvoKey));

	// HARD DIAGNOSTIC — always visible in Output Log
	UE_LOG(LogGitHubCopilotUE, Warning, TEXT("[CONVO-DIAG] SendChatCompletion: ConvoKey=%s, RequestId=%s, RequestConvoId=%s, CurrentConvoId=%s, ActiveConversations count=%d"),
		*ConvoKey, *Request.RequestId, *Request.ConversationId, *CurrentConversationId, ActiveConversations.Num());

	// Build or continue the conversation messages
	TArray<TSharedPtr<FJsonValue>>& ConvoMessages = ActiveConversations.FindOrAdd(ConvoKey);

	// HARD DIAGNOSTIC — message count before we do anything
	UE_LOG(LogGitHubCopilotUE, Warning, TEXT("[CONVO-DIAG] ConvoMessages for key %s: %d messages BEFORE append"), *ConvoKey, ConvoMessages.Num());

	if (ConvoMessages.Num() == 0)
	{
		// Brand new conversation — add system prompt + user message
		TSharedPtr<FJsonObject> SystemMsg = MakeShareable(new FJsonObject);
		SystemMsg->SetStringField(TEXT("role"), TEXT("system"));
		SystemMsg->SetStringField(TEXT("content"), BuildSystemPrompt(Request));
		ConvoMessages.Add(MakeShareable(new FJsonValueObject(SystemMsg)));

		Log(FString::Printf(TEXT("BridgeService: [CONVO] NEW conversation %s — system prompt added"), *ConvoKey));
	}
	else
	{
		// Existing conversation — log the message roles for diagnostics
		FString RoleSummary;
		for (const TSharedPtr<FJsonValue>& MsgVal : ConvoMessages)
		{
			if (MsgVal.IsValid() && MsgVal->AsObject().IsValid())
			{
				FString Role = MsgVal->AsObject()->GetStringField(TEXT("role"));
				RoleSummary += Role.Left(1).ToUpper() + TEXT(" ");
			}
		}
		Log(FString::Printf(TEXT("BridgeService: [CONVO] RESUMING conversation %s — %d existing messages [%s]"),
			*ConvoKey, ConvoMessages.Num(), *RoleSummary.TrimEnd()));
	}

	// If UserPrompt is non-empty, append a new user turn (handles both new and continuing conversations)
	if (!Request.UserPrompt.IsEmpty())
	{
		FString UserContent = Request.UserPrompt;

		// Append file context if present
		for (const FCopilotFileTarget& Target : Request.FileTargets)
		{
			if (!Target.SelectedText.IsEmpty())
			{
				UserContent += FString::Printf(TEXT("\n\nFile: %s\n```\n%s\n```"), *Target.FilePath, *Target.SelectedText);
			}
			else if (!Target.FilePath.IsEmpty())
			{
				UserContent += FString::Printf(TEXT("\n\nTarget file: %s"), *Target.FilePath);
			}
		}

		bool bModelSupportsVision = false;
		for (const FCopilotModel& ModelInfo : AvailableModels)
		{
			if (ModelInfo.Id == ModelToUse)
			{
				bModelSupportsVision = ModelInfo.bSupportsVision;
				break;
			}
		}

		bool bUsingMultipartContent = false;
		TArray<TSharedPtr<FJsonValue>> MultipartContent;
		auto AddTextPart = [&MultipartContent](const FString& Text)
		{
			TSharedPtr<FJsonObject> TextPart = MakeShareable(new FJsonObject);
			TextPart->SetStringField(TEXT("type"), TEXT("text"));
			TextPart->SetStringField(TEXT("text"), Text);
			MultipartContent.Add(MakeShareable(new FJsonValueObject(TextPart)));
		};
		auto EnsureMultipartContent = [&]()
		{
			if (!bUsingMultipartContent)
			{
				AddTextPart(UserContent);
				bUsingMultipartContent = true;
			}
		};
		auto AppendAttachmentText = [&](const FString& Text)
		{
			if (bUsingMultipartContent)
			{
				AddTextPart(Text);
			}
			else
			{
				UserContent += FString::Printf(TEXT("\n\n%s"), *Text);
			}
		};

		constexpr int32 MaxInlineAttachmentChars = 24000;
		constexpr int64 MaxInlineImageBytes = 4 * 1024 * 1024;
		for (const FCopilotAttachment& Attachment : Request.Attachments)
		{
			const FString AttachmentPath = Attachment.FilePath.TrimStartAndEnd();
			if (AttachmentPath.IsEmpty())
			{
				continue;
			}

			const FString AttachmentName = FPaths::GetCleanFilename(AttachmentPath);
			if (!FPaths::FileExists(AttachmentPath))
			{
				AppendAttachmentText(FString::Printf(TEXT("[Attachment skipped: %s (file not found)]"), *AttachmentName));
				continue;
			}

			const FString MimeType = ResolveAttachmentMimeType(Attachment);
			if (IsImageMimeType(MimeType))
			{
				if (!bModelSupportsVision)
				{
					AppendAttachmentText(FString::Printf(TEXT("[Image attached but current model is text-only: %s]"), *AttachmentName));
					continue;
				}

				TArray<uint8> ImageBytes;
				if (!FFileHelper::LoadFileToArray(ImageBytes, *AttachmentPath))
				{
					AppendAttachmentText(FString::Printf(TEXT("[Attachment skipped: %s (failed to read image)]"), *AttachmentName));
					continue;
				}

				if (ImageBytes.Num() <= 0)
				{
					AppendAttachmentText(FString::Printf(TEXT("[Attachment skipped: %s (empty image file)]"), *AttachmentName));
					continue;
				}

				if (ImageBytes.Num() > MaxInlineImageBytes)
				{
					AppendAttachmentText(FString::Printf(TEXT("[Attachment skipped: %s (image exceeds %d MB inline limit)]"), *AttachmentName, MaxInlineImageBytes / (1024 * 1024)));
					continue;
				}

				EnsureMultipartContent();

				const FString Base64Data = FBase64::Encode(ImageBytes);
				TSharedPtr<FJsonObject> ImageUrlObj = MakeShareable(new FJsonObject);
				ImageUrlObj->SetStringField(TEXT("url"), FString::Printf(TEXT("data:%s;base64,%s"), *MimeType, *Base64Data));

				TSharedPtr<FJsonObject> ImagePart = MakeShareable(new FJsonObject);
				ImagePart->SetStringField(TEXT("type"), TEXT("image_url"));
				ImagePart->SetObjectField(TEXT("image_url"), ImageUrlObj);
				MultipartContent.Add(MakeShareable(new FJsonValueObject(ImagePart)));
				continue;
			}

			if (!IsLikelyTextAttachment(AttachmentPath))
			{
				AppendAttachmentText(FString::Printf(TEXT("[Attachment noted but not inlined (unsupported binary type): %s]"), *AttachmentName));
				continue;
			}

			FString AttachmentContent;
			if (!FFileHelper::LoadFileToString(AttachmentContent, *AttachmentPath))
			{
				AppendAttachmentText(FString::Printf(TEXT("[Attachment skipped: %s (failed to read text)]"), *AttachmentName));
				continue;
			}

			bool bWasTruncated = false;
			if (AttachmentContent.Len() > MaxInlineAttachmentChars)
			{
				AttachmentContent = AttachmentContent.Left(MaxInlineAttachmentChars);
				bWasTruncated = true;
			}

			AppendAttachmentText(FString::Printf(
				TEXT("Attachment: %s\n```text\n%s\n```%s"),
				*AttachmentName,
				*AttachmentContent,
				bWasTruncated ? TEXT("\n[Truncated for size]") : TEXT("")));
		}

		TSharedPtr<FJsonObject> UserMsg = MakeShareable(new FJsonObject);
		UserMsg->SetStringField(TEXT("role"), TEXT("user"));
		if (bUsingMultipartContent)
		{
			UserMsg->SetArrayField(TEXT("content"), MultipartContent);
		}
		else
		{
			UserMsg->SetStringField(TEXT("content"), UserContent);
		}
		ConvoMessages.Add(MakeShareable(new FJsonValueObject(UserMsg)));

		Log(FString::Printf(TEXT("BridgeService: Appended user turn (%d chars) to conversation %s (now %d messages)"),
			Request.UserPrompt.Len(), *ConvoKey, ConvoMessages.Num()));
	}
	// else: tool call continuation — messages already have assistant + tool results

	// ── Conversation trimming ──
	// If the serialized conversation exceeds a safe payload threshold, trim older
	// messages (preserving system prompt at index 0 and the most recent turns).
	// This prevents 400 errors from exceeding the model's context window.
	{
		constexpr int32 MaxPayloadChars = 800000; // ~800KB safe limit for serialized JSON
		constexpr int32 MinMessagesToKeep = 6;     // system + at least 2 recent user/assistant pairs + current

		auto EstimatePayloadSize = [](const TArray<TSharedPtr<FJsonValue>>& Msgs) -> int32
		{
			int32 Total = 0;
			for (const TSharedPtr<FJsonValue>& MsgVal : Msgs)
			{
				if (MsgVal.IsValid() && MsgVal->AsObject().IsValid())
				{
					const TSharedPtr<FJsonObject>& MsgObj = MsgVal->AsObject();
					if (MsgObj->HasTypedField<EJson::String>(TEXT("content")))
					{
						Total += MsgObj->GetStringField(TEXT("content")).Len();
					}
					else if (MsgObj->HasTypedField<EJson::Array>(TEXT("content")))
					{
						for (const auto& Part : MsgObj->GetArrayField(TEXT("content")))
						{
							if (Part.IsValid() && Part->AsObject().IsValid())
							{
								const TSharedPtr<FJsonObject>& PartObj = Part->AsObject();
								if (PartObj->HasField(TEXT("text")))
									Total += PartObj->GetStringField(TEXT("text")).Len();
								if (PartObj->HasField(TEXT("url")))
									Total += PartObj->GetStringField(TEXT("url")).Len();
							}
						}
					}
					Total += 100; // overhead for role, JSON structure
				}
			}
			return Total;
		};

		int32 PayloadEstimate = EstimatePayloadSize(ConvoMessages);
		if (PayloadEstimate > MaxPayloadChars && ConvoMessages.Num() > MinMessagesToKeep)
		{
			Log(FString::Printf(TEXT("BridgeService: [CONVO] Payload too large (%d chars, %d msgs) — trimming older messages"),
				PayloadEstimate, ConvoMessages.Num()));

			// Keep system prompt (index 0) and the most recent messages
			while (PayloadEstimate > MaxPayloadChars && ConvoMessages.Num() > MinMessagesToKeep)
			{
				// Remove the oldest non-system message (index 1)
				ConvoMessages.RemoveAt(1);
				PayloadEstimate = EstimatePayloadSize(ConvoMessages);
			}

			Log(FString::Printf(TEXT("BridgeService: [CONVO] Trimmed to %d messages (%d chars)"),
				ConvoMessages.Num(), PayloadEstimate));
		}
	}

	// Build request body
	TSharedPtr<FJsonObject> JsonBody = MakeShareable(new FJsonObject);
	JsonBody->SetStringField(TEXT("model"), ModelToUse);

	// Check if this model requires /responses format vs /chat/completions
	bool bUseResponsesFormat = false;
	for (const FCopilotModel& M : AvailableModels)
	{
		if (M.Id == ModelToUse)
		{
			bUseResponsesFormat = M.RequiresResponsesFormat();
			break;
		}
	}

	if (bUseResponsesFormat)
	{
		// /responses endpoint format: { model, input (string or array of items) }
		// Convert messages to a single input string
		FString InputText;
		for (const TSharedPtr<FJsonValue>& MsgVal : ConvoMessages)
		{
			const TSharedPtr<FJsonObject>& Msg = MsgVal->AsObject();
			if (!Msg.IsValid()) continue;

			FString Role = Msg->GetStringField(TEXT("role"));
			FString Content;

			// Content can be string or array (multipart)
			if (Msg->HasTypedField<EJson::String>(TEXT("content")))
			{
				Content = Msg->GetStringField(TEXT("content"));
			}
			else if (Msg->HasTypedField<EJson::Array>(TEXT("content")))
			{
				// Extract text parts from multipart content
				const TArray<TSharedPtr<FJsonValue>>& Parts = Msg->GetArrayField(TEXT("content"));
				for (const auto& Part : Parts)
				{
					const TSharedPtr<FJsonObject>& PartObj = Part->AsObject();
					if (PartObj.IsValid() && PartObj->GetStringField(TEXT("type")) == TEXT("text"))
					{
						Content += PartObj->GetStringField(TEXT("text"));
					}
				}
			}

			if (!Content.IsEmpty())
			{
				if (Role == TEXT("system"))
				{
					InputText += FString::Printf(TEXT("[System]\n%s\n\n"), *Content);
				}
				else if (Role == TEXT("user"))
				{
					InputText += FString::Printf(TEXT("[User]\n%s\n\n"), *Content);
				}
				else if (Role == TEXT("assistant"))
				{
					InputText += FString::Printf(TEXT("[Assistant]\n%s\n\n"), *Content);
				}
				else if (Role == TEXT("tool"))
				{
					FString ToolName = Msg->HasField(TEXT("name")) ? Msg->GetStringField(TEXT("name")) : TEXT("tool");
					InputText += FString::Printf(TEXT("[Tool Result: %s]\n%s\n\n"), *ToolName, *Content);
				}
			}
		}

		JsonBody->SetStringField(TEXT("input"), InputText);

		if (bAllowToolCalls)
		{
			TArray<TSharedPtr<FJsonValue>> Tools = FGitHubCopilotUEToolExecutor::BuildToolDefinitions();
			JsonBody->SetArrayField(TEXT("tools"), Tools);
			Log(FString::Printf(TEXT("BridgeService: [/responses] Sending %d tools, input=%d chars, model=%s"), Tools.Num(), InputText.Len(), *ModelToUse));
		}
		else
		{
			Log(FString::Printf(TEXT("BridgeService: [/responses] Final answer mode, input=%d chars, model=%s"), InputText.Len(), *ModelToUse));
		}
	}
	else
	{
		// Standard /chat/completions format
		JsonBody->SetArrayField(TEXT("messages"), ConvoMessages);
		JsonBody->SetNumberField(TEXT("temperature"), 0.1);

		// Use configurable max output tokens (default: 16384)
		const UGitHubCopilotUESettings* Settings = UGitHubCopilotUESettings::Get();
		const int32 MaxOutputTokens = Settings ? FMath::Max(1024, Settings->MaxOutputTokens) : 16384;
		JsonBody->SetNumberField(TEXT("max_tokens"), MaxOutputTokens);

		if (bAllowToolCalls)
		{
			TArray<TSharedPtr<FJsonValue>> Tools = FGitHubCopilotUEToolExecutor::BuildToolDefinitions();
			JsonBody->SetArrayField(TEXT("tools"), Tools);
			JsonBody->SetStringField(TEXT("tool_choice"), TEXT("auto"));
			Log(FString::Printf(TEXT("BridgeService: Sending %d tools, %d messages, model=%s"), Tools.Num(), ConvoMessages.Num(), *ModelToUse));
		}
		else
		{
			JsonBody->SetStringField(TEXT("tool_choice"), TEXT("none"));
			Log(FString::Printf(TEXT("BridgeService: Forcing final answer with tools disabled, messages=%d, model=%s"), ConvoMessages.Num(), *ModelToUse));
		}
	}

	FString RequestBody;
	TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&RequestBody);
	FJsonSerializer::Serialize(JsonBody.ToSharedRef(), Writer);
	Writer->Close();

	// Log payload size + preview for debugging
	Log(FString::Printf(TEXT("BridgeService: Request payload size: %d chars, endpoint: %s"),
		RequestBody.Len(), *GetEndpointURLForModel(ModelToUse)));
	if (RequestBody.Len() > 0)
	{
		FString Preview = RequestBody.Left(500);
		Log(FString::Printf(TEXT("BridgeService: Body preview: %s"), *Preview));
	}
	if (RequestBody.Len() > 100000)
	{
		Log(TEXT("BridgeService: WARNING - Very large request payload (>100KB). May cause transport issues."));
	}

	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> HttpReq = FHttpModule::Get().CreateRequest();
	HttpReq->SetURL(GetEndpointURLForModel(ModelToUse));
	HttpReq->SetVerb(TEXT("POST"));
	HttpReq->SetHeader(TEXT("Authorization"), FString::Printf(TEXT("Bearer %s"), *CopilotToken));
	HttpReq->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
	HttpReq->SetHeader(TEXT("Accept"), TEXT("application/json"));
	HttpReq->SetHeader(TEXT("Copilot-Integration-Id"), TEXT("vscode-chat"));
	HttpReq->SetHeader(TEXT("Editor-Version"), TEXT("vscode/1.103.2"));
	HttpReq->SetHeader(TEXT("Editor-Plugin-Version"), TEXT("copilot-chat/0.27.1"));
	HttpReq->SetHeader(TEXT("User-Agent"), TEXT("GitHubCopilotChat/0.27.1"));
	HttpReq->SetHeader(TEXT("OpenAI-Intent"), TEXT("conversation-panel"));
	HttpReq->SetHeader(TEXT("X-Initiator"), TEXT("user"));
	HttpReq->SetHeader(TEXT("X-Request-Id"), FGuid::NewGuid().ToString());
	HttpReq->SetContentAsString(RequestBody);
	// Do NOT manually set Content-Length — UE's HTTP module calculates it
	// from the UTF-8 byte array set by SetContentAsString.

	const UGitHubCopilotUESettings* Settings = UGitHubCopilotUESettings::Get();
	if (Settings)
	{
		HttpReq->SetTimeout(Settings->TimeoutSeconds);
	}

	FString RequestId = Request.RequestId;
	FString ConvoId = ConvoKey;
	// HARD DIAGNOSTIC — final state before sending
	UE_LOG(LogGitHubCopilotUE, Warning, TEXT("[CONVO-DIAG] SENDING: ConvoKey=%s, total messages=%d, bound ConvoId=%s"),
		*ConvoKey, ConvoMessages.Num(), *ConvoId);

	HttpReq->OnProcessRequestComplete().BindRaw(this, &FGitHubCopilotUEBridgeService::OnChatCompletionResponse, RequestId, ConvoId);
	HttpReq->ProcessRequest();
}

void FGitHubCopilotUEBridgeService::OnChatCompletionResponse(FHttpRequestPtr HttpReq, FHttpResponsePtr HttpResp, bool bSuccess, FString RequestId, FString ConversationId)
{
	// HARD DIAGNOSTIC
	const TArray<TSharedPtr<FJsonValue>>* DiagConvo = ActiveConversations.Find(ConversationId);
	UE_LOG(LogGitHubCopilotUE, Warning, TEXT("[CONVO-DIAG] OnResponse: ConversationId=%s, found_in_map=%s, msg_count=%d, ActiveConversations_count=%d"),
		*ConversationId, DiagConvo ? TEXT("YES") : TEXT("NO"), DiagConvo ? DiagConvo->Num() : -1, ActiveConversations.Num());

	FCopilotResponse Response;
	Response.RequestId = RequestId;
	Response.Timestamp = FDateTime::Now().ToString();

	if (!bSuccess || !HttpResp.IsValid())
	{
		const int32 RequestStatus = HttpReq.IsValid() ? static_cast<int32>(HttpReq->GetStatus()) : -1;
		const FString RequestUrl = HttpReq.IsValid() ? HttpReq->GetURL() : TEXT("unknown");
		int32& RetryCount = NoResponseRetryCounts.FindOrAdd(RequestId);
		if (RetryCount < MaxNoResponseRetries)
		{
			++RetryCount;
			const bool bAllowToolCalls = !ForcedFinalResponseRequestIds.Contains(RequestId);
			PendingRequestTimestamps.Add(RequestId, FPlatformTime::Seconds());
			Log(FString::Printf(
				TEXT("BridgeService: Request %s got no HTTP response (status=%d, url=%s), retrying once (%d/%d), tools=%s"),
				*RequestId,
				RequestStatus,
				*RequestUrl,
				RetryCount,
				MaxNoResponseRetries,
				bAllowToolCalls ? TEXT("on") : TEXT("off")));

			FCopilotRequest RetryRequest;
			RetryRequest.RequestId = RequestId;
			RetryRequest.ConversationId = ConversationId;
			RetryRequest.CommandType = ECopilotCommandType::Ask;

			// Token may have expired — refresh before retry
			if (IsCopilotTokenExpired())
			{
				Log(TEXT("BridgeService: Token expired before retry — queuing retry after refresh"));
				QueuedRequestsAwaitingToken.Add(TPair<FCopilotRequest, bool>(RetryRequest, bAllowToolCalls));
				RefreshCopilotTokenIfNeeded();
			}
			else
			{
				SendChatCompletion(RetryRequest, bAllowToolCalls);
			}
			return;
		}

		PendingRequestTimestamps.Remove(RequestId);
		// Don't remove ActiveConversations — conversation persists across errors
		ToolCallIterations.Remove(RequestId);
		ForcedFinalResponseRequestIds.Remove(RequestId);
		NoResponseRetryCounts.Remove(RequestId);
		Response.ResultStatus = ECopilotResultStatus::Failure;
		Response.ErrorMessage = FString::Printf(
			TEXT("Request to Copilot failed after retry (no HTTP response, request_status=%d)."),
			RequestStatus);
		Log(FString::Printf(
			TEXT("BridgeService: Request %s failed after retry (no HTTP response, status=%d, url=%s)"),
			*RequestId,
			RequestStatus,
			*RequestUrl));
		OnResponseReceived.Broadcast(Response);
		return;
	}

	int32 StatusCode = HttpResp->GetResponseCode();
	FString Body = HttpResp->GetContentAsString();

	// Log raw response for debugging
	Log(FString::Printf(TEXT("BridgeService: HTTP %d, body length=%d"), StatusCode, Body.Len()));
	if (Body.Len() > 0)
	{
		Log(FString::Printf(TEXT("BridgeService: Response preview: %s"), *Body.Left(300)));
	}

	if (!EHttpResponseCodes::IsOk(StatusCode))
	{
		PendingRequestTimestamps.Remove(RequestId);
		// Don't remove ActiveConversations — conversation persists across errors for retry
		ToolCallIterations.Remove(RequestId);
		ForcedFinalResponseRequestIds.Remove(RequestId);
		NoResponseRetryCounts.Remove(RequestId);
		Response.ResultStatus = ECopilotResultStatus::Failure;
		Response.ErrorMessage = FString::Printf(TEXT("Copilot API error (HTTP %d): %s"), StatusCode, *Body.Left(500));
		Log(FString::Printf(TEXT("BridgeService: Request %s failed - HTTP %d"), *RequestId, StatusCode));
		UE_LOG(LogGitHubCopilotUE, Warning, TEXT("GitHub Copilot: API error (HTTP %d)"), StatusCode);

		if (StatusCode == 401)
		{
			Log(TEXT("BridgeService: Token expired or invalid, refreshing..."));
			ExchangeForCopilotToken();
		}
		else if (StatusCode == 403)
		{
			Log(TEXT("BridgeService: Access denied — check Copilot subscription status or model policy."));
		}
		else if (StatusCode == 429)
		{
			Log(TEXT("BridgeService: Rate limited — slow down requests or quota exhausted for this model."));
		}
		else if (StatusCode == 400)
		{
			// Check for specific error codes like unsupported_api_for_model
			Log(FString::Printf(TEXT("BridgeService: Bad request — check model compatibility: %s"), *Body.Left(300)));
		}

		OnResponseReceived.Broadcast(Response);
		return;
	}

	// Parse response
	TSharedPtr<FJsonObject> Json;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Body);
	if (!FJsonSerializer::Deserialize(Reader, Json) || !Json.IsValid())
	{
		PendingRequestTimestamps.Remove(RequestId);
		ToolCallIterations.Remove(RequestId);
		ForcedFinalResponseRequestIds.Remove(RequestId);
		NoResponseRetryCounts.Remove(RequestId);
		Response.ResultStatus = ECopilotResultStatus::Failure;
		Response.ErrorMessage = TEXT("Failed to parse Copilot response JSON");
		OnResponseReceived.Broadcast(Response);
		return;
	}

	// Extract the assistant's message
	// IMPORTANT: Copilot API may return MULTIPLE choices for Claude models:
	//   choices[0] = content (thinking text), no tool_calls
	//   choices[1] = tool_calls, no content
	// We need to find the choice that has tool_calls, OR merge them.
	const TArray<TSharedPtr<FJsonValue>>* Choices;
	if (!Json->TryGetArrayField(TEXT("choices"), Choices) || Choices->Num() == 0)
	{
		PendingRequestTimestamps.Remove(RequestId);
		ToolCallIterations.Remove(RequestId);
		ForcedFinalResponseRequestIds.Remove(RequestId);
		NoResponseRetryCounts.Remove(RequestId);
		Response.ResultStatus = ECopilotResultStatus::Failure;
		Response.ErrorMessage = TEXT("No choices in Copilot response");
		OnResponseReceived.Broadcast(Response);
		return;
	}

	// Scan all choices to find content and tool_calls (may be in different choices)
	TSharedPtr<FJsonObject> ContentMessage;     // choice with text content
	TSharedPtr<FJsonObject> ToolCallMessage;    // choice with tool_calls
	TSharedPtr<FJsonObject> ToolCallChoice;     // the choice object containing tool_calls
	FString FinishReason;

	for (const TSharedPtr<FJsonValue>& ChoiceVal : *Choices)
	{
		TSharedPtr<FJsonObject> ChoiceObj = ChoiceVal->AsObject();
		if (!ChoiceObj.IsValid()) continue;

		TSharedPtr<FJsonObject> Msg = ChoiceObj->GetObjectField(TEXT("message"));
		if (!Msg.IsValid()) continue;

		FString FR = ChoiceObj->GetStringField(TEXT("finish_reason"));

		if (Msg->HasField(TEXT("tool_calls")))
		{
			ToolCallMessage = Msg;
			ToolCallChoice = ChoiceObj;
			FinishReason = FR;
		}

		FString Content = Msg->HasField(TEXT("content")) ? Msg->GetStringField(TEXT("content")) : TEXT("");
		if (!Content.IsEmpty() && !ContentMessage.IsValid())
		{
			ContentMessage = Msg;
			if (FinishReason.IsEmpty())
			{
				FinishReason = FR;
			}
		}
	}

	// Use the tool_call message if found, otherwise use the content message
	TSharedPtr<FJsonObject> Message = ToolCallMessage.IsValid() ? ToolCallMessage : ContentMessage;
	if (!FinishReason.IsEmpty() && FinishReason == TEXT("tool_calls") && ToolCallMessage.IsValid())
	{
		Message = ToolCallMessage;
	}
	else if (!Message.IsValid() && Choices->Num() > 0)
	{
		// Fallback: just use first choice
		TSharedPtr<FJsonObject> FirstChoice = (*Choices)[0]->AsObject();
		if (FirstChoice.IsValid())
		{
			Message = FirstChoice->GetObjectField(TEXT("message"));
			FinishReason = FirstChoice->GetStringField(TEXT("finish_reason"));
		}
	}

	// Capture content text from ContentMessage even if we're using ToolCallMessage
	FString PrefixContent;
	if (ContentMessage.IsValid() && ContentMessage != Message)
	{
		PrefixContent = ContentMessage->HasField(TEXT("content")) ? ContentMessage->GetStringField(TEXT("content")) : TEXT("");
	}

	// ── Extract and broadcast thinking/reasoning content ──
	// Models may return thinking in several formats:
	//   1. "reasoning_content" field on the message (DeepSeek, some OpenAI)
	//   2. content as array with {type:"thinking", thinking:"..."} parts (Claude)
	//   3. Separate choice with only content (Claude split-choice) — already captured as PrefixContent
	for (const TSharedPtr<FJsonValue>& ChoiceVal : *Choices)
	{
		TSharedPtr<FJsonObject> ChoiceObj = ChoiceVal->AsObject();
		if (!ChoiceObj.IsValid()) continue;
		TSharedPtr<FJsonObject> Msg = ChoiceObj->GetObjectField(TEXT("message"));
		if (!Msg.IsValid()) continue;

		// Format 1: reasoning_content field
		if (Msg->HasField(TEXT("reasoning_content")))
		{
			FString Reasoning = Msg->GetStringField(TEXT("reasoning_content"));
			if (!Reasoning.IsEmpty())
			{
				// Truncate very long reasoning for display
				FString Display = Reasoning.Len() > 300 ? Reasoning.Left(300) + TEXT("...") : Reasoning;
				Display.ReplaceInline(TEXT("\n"), TEXT(" "));
				OnToolActivity.Broadcast(FString::Printf(TEXT("Thinking: %s"), *Display));
			}
		}

		// Format 2: content as array with thinking parts
		if (Msg->HasTypedField<EJson::Array>(TEXT("content")))
		{
			const TArray<TSharedPtr<FJsonValue>>& ContentParts = Msg->GetArrayField(TEXT("content"));
			for (const auto& PartVal : ContentParts)
			{
				TSharedPtr<FJsonObject> Part = PartVal->AsObject();
				if (!Part.IsValid()) continue;
				FString PartType = Part->HasField(TEXT("type")) ? Part->GetStringField(TEXT("type")) : TEXT("");
				if (PartType == TEXT("thinking") && Part->HasField(TEXT("thinking")))
				{
					FString Thinking = Part->GetStringField(TEXT("thinking"));
					if (!Thinking.IsEmpty())
					{
						FString Display = Thinking.Len() > 300 ? Thinking.Left(300) + TEXT("...") : Thinking;
						Display.ReplaceInline(TEXT("\n"), TEXT(" "));
						OnToolActivity.Broadcast(FString::Printf(TEXT("Thinking: %s"), *Display));
					}
				}
			}
		}
	}

	// Format 3: If PrefixContent exists during tool calls, that's the model's reasoning
	if (!PrefixContent.IsEmpty() && ToolCallMessage.IsValid())
	{
		FString Display = PrefixContent.Len() > 300 ? PrefixContent.Left(300) + TEXT("...") : PrefixContent;
		Display.ReplaceInline(TEXT("\n"), TEXT(" "));
		OnToolActivity.Broadcast(FString::Printf(TEXT("Thinking: %s"), *Display));
	}

	// Debug logging
	FString ResponseModel = Json->HasField(TEXT("model")) ? Json->GetStringField(TEXT("model")) : TEXT("unknown");
	bool bHasToolCalls = Message.IsValid() && Message->HasField(TEXT("tool_calls"));
	Log(FString::Printf(TEXT("BridgeService: Response — model=%s, finish_reason=%s, choices=%d, has_tool_calls=%s"),
		*ResponseModel, *FinishReason, Choices->Num(), bHasToolCalls ? TEXT("YES") : TEXT("NO")));

	if (!Message.IsValid())
	{
		PendingRequestTimestamps.Remove(RequestId);
		ToolCallIterations.Remove(RequestId);
		ForcedFinalResponseRequestIds.Remove(RequestId);
		NoResponseRetryCounts.Remove(RequestId);
		Response.ResultStatus = ECopilotResultStatus::Failure;
		Response.ErrorMessage = TEXT("No message in response choices");
		OnResponseReceived.Broadcast(Response);
		return;
	}

	// ════════════════════════════════════════════════════════════════
	// AGENTIC TOOL CALL LOOP
	// If the model wants to call tools, execute them and loop back
	// ════════════════════════════════════════════════════════════════

	if (FinishReason == TEXT("tool_calls") || Message->HasField(TEXT("tool_calls")))
	{
		const TArray<TSharedPtr<FJsonValue>>* ToolCalls;
		if (!Message->TryGetArrayField(TEXT("tool_calls"), ToolCalls) || ToolCalls->Num() == 0)
		{
			Log(TEXT("BridgeService: finish_reason=tool_calls but no tool_calls array found — treating as normal response"));
			goto HandleNormalResponse;
		}

		// Append the assistant's message (with tool_calls) to conversation
		TArray<TSharedPtr<FJsonValue>>& ConvoMessages = ActiveConversations.FindOrAdd(ConversationId);
		ConvoMessages.Add(MakeShareable(new FJsonValueObject(Message)));

		// Execute each tool call and append results
		for (const TSharedPtr<FJsonValue>& ToolCallVal : *ToolCalls)
		{
			TSharedPtr<FJsonObject> ToolCall = ToolCallVal->AsObject();
			if (!ToolCall.IsValid()) continue;

			FString ToolCallId = ToolCall->GetStringField(TEXT("id"));
			TSharedPtr<FJsonObject> Function = ToolCall->GetObjectField(TEXT("function"));
			if (!Function.IsValid()) continue;

			FString ToolName = Function->GetStringField(TEXT("name"));
			FString ArgsStr = Function->GetStringField(TEXT("arguments"));

			// Parse tool arguments
			TSharedPtr<FJsonObject> ToolArgs;
			TSharedRef<TJsonReader<>> ArgReader = TJsonReaderFactory<>::Create(ArgsStr);
			FJsonSerializer::Deserialize(ArgReader, ToolArgs);
			if (!ToolArgs.IsValid())
			{
				ToolArgs = MakeShareable(new FJsonObject);
			}

			// Execute the tool — broadcast activity to UI
			FString ActivitySummary;
			if (ToolName == TEXT("web_search"))
			{
				FString Query = ToolArgs->HasField(TEXT("query")) ? ToolArgs->GetStringField(TEXT("query")) : TEXT("...");
				ActivitySummary = FString::Printf(TEXT("Searching: %s"), *Query);
			}
			else if (ToolName == TEXT("read_file"))
			{
				FString Path = ToolArgs->HasField(TEXT("path")) ? ToolArgs->GetStringField(TEXT("path")) : TEXT("...");
				ActivitySummary = FString::Printf(TEXT("Reading: %s"), *FPaths::GetCleanFilename(Path));
			}
			else if (ToolName == TEXT("write_file") || ToolArgs->HasField(TEXT("path")))
			{
				FString Path = ToolArgs->HasField(TEXT("path")) ? ToolArgs->GetStringField(TEXT("path")) : TEXT("...");
				ActivitySummary = FString::Printf(TEXT("Writing: %s"), *FPaths::GetCleanFilename(Path));
			}
			else if (ToolName == TEXT("execute_console_command"))
			{
				FString Cmd = ToolArgs->HasField(TEXT("command")) ? ToolArgs->GetStringField(TEXT("command")) : TEXT("...");
				ActivitySummary = FString::Printf(TEXT("Running command: %s"), *Cmd.Left(80));
			}
			else if (ToolName == TEXT("create_mesh") || ToolName == TEXT("create_material") || ToolName == TEXT("add_modifier"))
			{
				ActivitySummary = FString::Printf(TEXT("Using tool: %s"), *ToolName);
			}
			else if (ToolName == TEXT("capture_viewport") || ToolName == TEXT("render_preview"))
			{
				ActivitySummary = TEXT("Capturing viewport...");
			}
			else
			{
				ActivitySummary = FString::Printf(TEXT("Using tool: %s"), *ToolName);
			}
			OnToolActivity.Broadcast(ActivitySummary);

			Log(FString::Printf(TEXT("BridgeService: Executing tool: %s"), *ToolName));
			UE_LOG(LogGitHubCopilotUE, Display, TEXT("  [Tool] %s(...)"), *ToolName);

			FString ToolResult;
			if (ToolExecutor.IsValid())
			{
				ToolResult = ToolExecutor->ExecuteTool(ToolName, ToolArgs);
			}
			else
			{
				ToolResult = TEXT("Error: Tool executor not available");
			}

			// Broadcast completion
			FString ResultPreview = ToolResult.Left(120);
			ResultPreview.ReplaceInline(TEXT("\n"), TEXT(" "));
			OnToolActivity.Broadcast(FString::Printf(TEXT("  Done: %s"), *ResultPreview));

			Log(FString::Printf(TEXT("BridgeService: Tool %s returned %d chars"), *ToolName, ToolResult.Len()));

			// Append tool result to conversation
			TSharedPtr<FJsonObject> ToolResultMsg = MakeShareable(new FJsonObject);
			ToolResultMsg->SetStringField(TEXT("role"), TEXT("tool"));
			ToolResultMsg->SetStringField(TEXT("tool_call_id"), ToolCallId);
			ToolResultMsg->SetStringField(TEXT("name"), ToolName);
			ToolResultMsg->SetStringField(TEXT("content"), ToolResult);
			ConvoMessages.Add(MakeShareable(new FJsonValueObject(ToolResultMsg)));

			// If a tool produced a render/screenshot, inject the image as a
			// vision message so the model can see and analyze it
			if (ToolResult.StartsWith(TEXT("__RENDER_IMAGE__:")))
			{
				FString ImageLine = ToolResult;
				int32 NewlineIdx;
				if (ImageLine.FindChar(TEXT('\n'), NewlineIdx))
				{
					ImageLine = ImageLine.Left(NewlineIdx);
				}
				FString ImagePath = ImageLine.Replace(TEXT("__RENDER_IMAGE__:"), TEXT("")).TrimStartAndEnd();

				TArray<uint8> ImageData;
				if (FFileHelper::LoadFileToArray(ImageData, *ImagePath))
				{
					FString Base64 = FBase64::Encode(ImageData);
					FString Ext = FPaths::GetExtension(ImagePath).ToLower();
					FString Mime = (Ext == TEXT("jpg") || Ext == TEXT("jpeg")) ? TEXT("image/jpeg") : TEXT("image/png");

					// Build multipart user message: [{type:text}, {type:image_url}]
					TSharedPtr<FJsonObject> TextPart = MakeShareable(new FJsonObject);
					TextPart->SetStringField(TEXT("type"), TEXT("text"));
					TextPart->SetStringField(TEXT("text"), TEXT("Here is the captured image. Analyze what you see and describe it."));

					TSharedPtr<FJsonObject> ImageUrlInner = MakeShareable(new FJsonObject);
					ImageUrlInner->SetStringField(TEXT("url"), FString::Printf(TEXT("data:%s;base64,%s"), *Mime, *Base64));

					TSharedPtr<FJsonObject> ImagePart = MakeShareable(new FJsonObject);
					ImagePart->SetStringField(TEXT("type"), TEXT("image_url"));
					ImagePart->SetObjectField(TEXT("image_url"), ImageUrlInner);

					TArray<TSharedPtr<FJsonValue>> ContentArray;
					ContentArray.Add(MakeShareable(new FJsonValueObject(TextPart)));
					ContentArray.Add(MakeShareable(new FJsonValueObject(ImagePart)));

					TSharedPtr<FJsonObject> VisionMsg = MakeShareable(new FJsonObject);
					VisionMsg->SetStringField(TEXT("role"), TEXT("user"));
					VisionMsg->SetArrayField(TEXT("content"), ContentArray);
					ConvoMessages.Add(MakeShareable(new FJsonValueObject(VisionMsg)));

					Log(FString::Printf(TEXT("BridgeService: Injected render image for vision analysis (%d bytes, %s)"),
						ImageData.Num(), *Mime));
				}
				else
				{
					Log(FString::Printf(TEXT("BridgeService: Failed to load render image: %s"), *ImagePath));
				}
			}
		}

		// Check iteration guard
		int32& IterCount = ToolCallIterations.FindOrAdd(RequestId);
		IterCount++;
		const UGitHubCopilotUESettings* Settings = UGitHubCopilotUESettings::Get();
		const int32 MaxToolLoopIterationsBeforeFinalization = Settings ? FMath::Max(0, Settings->MaxToolCallIterations) : 0;
		if (MaxToolLoopIterationsBeforeFinalization > 0 && IterCount > MaxToolLoopIterationsBeforeFinalization)
		{
			// Never hard-fail solely on loop count. First, force a no-tool final answer.
			if (!ForcedFinalResponseRequestIds.Contains(RequestId))
			{
				ForcedFinalResponseRequestIds.Add(RequestId);
				Log(FString::Printf(TEXT("BridgeService: Tool loop exceeded %d iterations for %s — forcing final answer (no more tools)"),
					MaxToolLoopIterationsBeforeFinalization, *RequestId));

				TSharedPtr<FJsonObject> FinalizeMsg = MakeShareable(new FJsonObject);
				FinalizeMsg->SetStringField(TEXT("role"), TEXT("system"));
				FinalizeMsg->SetStringField(TEXT("content"),
					TEXT("Stop calling tools now. Provide your best final answer for the user using the existing conversation and tool results."));
				ConvoMessages.Add(MakeShareable(new FJsonValueObject(FinalizeMsg)));

				FCopilotRequest ContinuationRequest;
				ContinuationRequest.RequestId = RequestId;
				ContinuationRequest.ConversationId = ConversationId;
				ContinuationRequest.CommandType = ECopilotCommandType::Ask;
				SendChatCompletion(ContinuationRequest, false);
				return;
			}

			// If the model still keeps looping after forced-final mode, return best effort text.
			PendingRequestTimestamps.Remove(RequestId);
			ToolCallIterations.Remove(RequestId);
			ForcedFinalResponseRequestIds.Remove(RequestId);
			NoResponseRetryCounts.Remove(RequestId);
			Response.ResultStatus = ECopilotResultStatus::Success;
			Response.ResponseText = PrefixContent.IsEmpty()
				? TEXT("I stopped repeated tool-call looping and returned control. Please retry with a narrower prompt or explicit target path.")
				: PrefixContent;
			Response.bSuccess = true;
			OnResponseReceived.Broadcast(Response);
			return;
		}

		// Send the conversation back to the model (it will see tool results and continue)
		Log(FString::Printf(TEXT("BridgeService: Tool loop iteration %d for request %s"), IterCount, *RequestId));

		FCopilotRequest ContinuationRequest;
		ContinuationRequest.RequestId = RequestId;
		ContinuationRequest.ConversationId = ConversationId;
		ContinuationRequest.CommandType = ECopilotCommandType::Ask;
		// Don't rebuild messages — SendChatCompletion will use ActiveConversations
		SendChatCompletion(ContinuationRequest, true);
		return;
	}

HandleNormalResponse:
	// ════════════════════════════════════════════════════════════════
	// FINAL RESPONSE — model is done, return to user
	// ════════════════════════════════════════════════════════════════

	PendingRequestTimestamps.Remove(RequestId);
	ToolCallIterations.Remove(RequestId);
	ForcedFinalResponseRequestIds.Remove(RequestId);
	NoResponseRetryCounts.Remove(RequestId);

	FString Content = Message->HasField(TEXT("content")) ? Message->GetStringField(TEXT("content")) : TEXT("");

	// If content was in a separate choice (Claude split-choice format), use that
	if (Content.IsEmpty() && !PrefixContent.IsEmpty())
	{
		Content = PrefixContent;
	}

	// ALWAYS append assistant message to conversation for multi-turn persistence.
	// Even if content is empty, we need the message in the chain so the model
	// sees proper turn-taking on subsequent requests.
	{
		TArray<TSharedPtr<FJsonValue>>& ConvoMessages = ActiveConversations.FindOrAdd(ConversationId);
		TSharedPtr<FJsonObject> AssistantMsg = MakeShareable(new FJsonObject);
		AssistantMsg->SetStringField(TEXT("role"), TEXT("assistant"));
		AssistantMsg->SetStringField(TEXT("content"), Content.IsEmpty() ? TEXT("(no response)") : Content);
		ConvoMessages.Add(MakeShareable(new FJsonValueObject(AssistantMsg)));
		Log(FString::Printf(TEXT("BridgeService: [CONVO] %s — appended assistant response, total messages: %d"),
			*ConversationId, ConvoMessages.Num()));
	}

	if (!Content.IsEmpty())
	{
		Response.ResponseText = Content;
		Response.ResultStatus = ECopilotResultStatus::Success;
	}
	else
	{
		Response.ResultStatus = ECopilotResultStatus::Failure;
		Response.ErrorMessage = TEXT("No content in Copilot response");
	}

	if (Json->HasField(TEXT("model")))
	{
		Response.ProviderMetadata.Add(TEXT("model"), Json->GetStringField(TEXT("model")));
	}

	Response.bSuccess = (Response.ResultStatus == ECopilotResultStatus::Success);

	Log(FString::Printf(TEXT("BridgeService: Request %s complete (%d chars)"), *RequestId, Response.ResponseText.Len()));
	OnResponseReceived.Broadcast(Response);
}

// ============================================================================
// Conversation management
// ============================================================================

void FGitHubCopilotUEBridgeService::ClearConversation(const FString& InConversationId)
{
	ActiveConversations.Remove(InConversationId);
	Log(FString::Printf(TEXT("BridgeService: Cleared conversation %s"), *InConversationId));
}

FString FGitHubCopilotUEBridgeService::GetOrCreateConversationId()
{
	if (CurrentConversationId.IsEmpty())
	{
		CurrentConversationId = FGuid::NewGuid().ToString(EGuidFormats::Short);
		Log(FString::Printf(TEXT("BridgeService: Created new conversation ID: %s"), *CurrentConversationId));
	}
	else
	{
		Log(FString::Printf(TEXT("BridgeService: Resuming existing conversation ID: %s (messages: %d)"),
			*CurrentConversationId, GetConversationMessageCount(CurrentConversationId)));
	}
	return CurrentConversationId;
}

FString FGitHubCopilotUEBridgeService::ResetConversationId()
{
	if (!CurrentConversationId.IsEmpty())
	{
		ClearConversation(CurrentConversationId);
	}
	CurrentConversationId = FGuid::NewGuid().ToString(EGuidFormats::Short);
	Log(FString::Printf(TEXT("BridgeService: Reset conversation — new ID: %s"), *CurrentConversationId));
	return CurrentConversationId;
}

int32 FGitHubCopilotUEBridgeService::GetConversationMessageCount(const FString& InConversationId) const
{
	const TArray<TSharedPtr<FJsonValue>>* ConvoPtr = ActiveConversations.Find(InConversationId);
	return ConvoPtr ? ConvoPtr->Num() : 0;
}

// ============================================================================
// Conversation persistence (save/load to disk)
// ============================================================================

FString FGitHubCopilotUEBridgeService::GetConversationCachePath() const
{
	return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("CopilotConversation.json"));
}

void FGitHubCopilotUEBridgeService::SetChatTranscript(const FString& Transcript)
{
	CachedChatTranscript = Transcript;
}

FString FGitHubCopilotUEBridgeService::GetChatTranscript() const
{
	return CachedChatTranscript;
}

void FGitHubCopilotUEBridgeService::SaveConversationCache()
{
	if (CurrentConversationId.IsEmpty())
	{
		return;
	}

	const TArray<TSharedPtr<FJsonValue>>* ConvoPtr = ActiveConversations.Find(CurrentConversationId);
	if (!ConvoPtr || ConvoPtr->Num() == 0)
	{
		return;
	}

	TSharedPtr<FJsonObject> Root = MakeShareable(new FJsonObject);
	Root->SetStringField(TEXT("conversation_id"), CurrentConversationId);
	Root->SetStringField(TEXT("chat_transcript"), CachedChatTranscript);
	Root->SetArrayField(TEXT("messages"), *ConvoPtr);

	FString Output;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Output);
	FJsonSerializer::Serialize(Root.ToSharedRef(), Writer);

	if (FFileHelper::SaveStringToFile(Output, *GetConversationCachePath()))
	{
		Log(FString::Printf(TEXT("BridgeService: Saved conversation (%d messages, %d chars transcript) to disk"),
			ConvoPtr->Num(), CachedChatTranscript.Len()));
	}
}

void FGitHubCopilotUEBridgeService::LoadConversationCache()
{
	FString Content;
	if (!FFileHelper::LoadFileToString(Content, *GetConversationCachePath()))
	{
		return;
	}

	TSharedPtr<FJsonObject> Root;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Content);
	if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
	{
		Log(TEXT("BridgeService: Failed to parse conversation cache"));
		return;
	}

	FString SavedConvoId = Root->GetStringField(TEXT("conversation_id"));
	CachedChatTranscript = Root->HasField(TEXT("chat_transcript")) ? Root->GetStringField(TEXT("chat_transcript")) : TEXT("");

	const TArray<TSharedPtr<FJsonValue>>* MessagesArray;
	if (!Root->TryGetArrayField(TEXT("messages"), MessagesArray) || MessagesArray->Num() == 0)
	{
		Log(TEXT("BridgeService: Conversation cache has no messages, skipping"));
		return;
	}

	// Restore the conversation
	CurrentConversationId = SavedConvoId;
	TArray<TSharedPtr<FJsonValue>>& ConvoMessages = ActiveConversations.FindOrAdd(CurrentConversationId);
	ConvoMessages = *MessagesArray;

	Log(FString::Printf(TEXT("BridgeService: Restored conversation %s from disk (%d messages, %d chars transcript)"),
		*CurrentConversationId, ConvoMessages.Num(), CachedChatTranscript.Len()));
}

// ============================================================================
// Connection management
// ============================================================================

ECopilotConnectionStatus FGitHubCopilotUEBridgeService::GetConnectionStatus() const
{
	return ConnectionStatus;
}

void FGitHubCopilotUEBridgeService::Connect()
{
	if (IsAuthenticated())
	{
		RefreshCopilotTokenIfNeeded();
		SetConnectionStatus(ECopilotConnectionStatus::Connected);
	}
	else if (!GitHubAccessToken.IsEmpty())
	{
		ExchangeForCopilotToken();
	}
	else
	{
		Log(TEXT("BridgeService: Not authenticated. Use 'Sign in with GitHub'."));
	}
}

void FGitHubCopilotUEBridgeService::Disconnect()
{
	SetConnectionStatus(ECopilotConnectionStatus::Disconnected);
}

void FGitHubCopilotUEBridgeService::TestConnection()
{
	if (IsAuthenticated())
	{
		FetchAvailableModels();
	}
}

void FGitHubCopilotUEBridgeService::SetConnectionStatus(ECopilotConnectionStatus NewStatus)
{
	if (ConnectionStatus != NewStatus)
	{
		ConnectionStatus = NewStatus;
		OnConnectionStatusChanged.Broadcast(ConnectionStatus);
	}
}

void FGitHubCopilotUEBridgeService::SetAuthState(ECopilotAuthState NewState)
{
	AuthState = NewState;
}

void FGitHubCopilotUEBridgeService::CheckForTimeouts()
{
	const UGitHubCopilotUESettings* Settings = UGitHubCopilotUESettings::Get();
	if (!Settings) return;

	double Now = FPlatformTime::Seconds();
	TArray<FString> TimedOut;

	for (auto& Pair : PendingRequestTimestamps)
	{
		if ((Now - Pair.Value) > Settings->TimeoutSeconds)
		{
			TimedOut.Add(Pair.Key);
		}
	}

	for (const FString& ReqId : TimedOut)
	{
		PendingRequestTimestamps.Remove(ReqId);
		NoResponseRetryCounts.Remove(ReqId);
		FCopilotResponse TimeoutResp;
		TimeoutResp.RequestId = ReqId;
		TimeoutResp.ResultStatus = ECopilotResultStatus::Timeout;
		TimeoutResp.ErrorMessage = TEXT("Request timed out");
		Log(FString::Printf(TEXT("BridgeService: Request %s timed out"), *ReqId));
		OnResponseReceived.Broadcast(TimeoutResp);
	}
}

void FGitHubCopilotUEBridgeService::Log(const FString& Message)
{
	UE_LOG(LogGitHubCopilotUE, Verbose, TEXT("%s"), *Message);
	// Internal log — only goes to full log file, not cluttering the Output Log
}
