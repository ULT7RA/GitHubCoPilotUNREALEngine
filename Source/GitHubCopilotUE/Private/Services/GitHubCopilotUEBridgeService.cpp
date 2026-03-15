// Copyright GitHub, Inc. All Rights Reserved.

#include "Services/GitHubCopilotUEBridgeService.h"
#include "Services/GitHubCopilotUEToolExecutor.h"
#include "GitHubCopilotUESettings.h"
#include "HttpModule.h"
#include "Interfaces/IHttpResponse.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/PlatformProcess.h"
#include "GenericPlatform/GenericPlatformHttp.h"

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
	PendingRequestTimestamps.Empty();
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
	if (!bSuccess || !HttpResp.IsValid())
	{
		Log(TEXT("BridgeService: Failed to reach Copilot token endpoint"));
		SetConnectionStatus(ECopilotConnectionStatus::Error);
		SetAuthState(ECopilotAuthState::Error);
		return;
	}

	if (!EHttpResponseCodes::IsOk(HttpResp->GetResponseCode()))
	{
		Log(FString::Printf(TEXT("BridgeService: Copilot token request failed (HTTP %d). Do you have an active Copilot subscription?"),
			HttpResp->GetResponseCode()));
		SetConnectionStatus(ECopilotConnectionStatus::Error);
		SetAuthState(ECopilotAuthState::Error);
		return;
	}

	TSharedPtr<FJsonObject> Json;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(HttpResp->GetContentAsString());
	if (!FJsonSerializer::Deserialize(Reader, Json) || !Json.IsValid())
	{
		Log(TEXT("BridgeService: Failed to parse Copilot token response"));
		SetConnectionStatus(ECopilotConnectionStatus::Error);
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
	if (IsCopilotTokenExpired() && !GitHubAccessToken.IsEmpty())
	{
		Log(TEXT("BridgeService: Copilot token expired, refreshing..."));
		ExchangeForCopilotToken();
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
	HttpReq->SetHeader(TEXT("Editor-Version"), TEXT("vscode/1.104.3"));
	HttpReq->SetHeader(TEXT("Editor-Plugin-Version"), TEXT("copilot-chat/0.26.7"));
	HttpReq->SetHeader(TEXT("User-Agent"), TEXT("GitHubCopilotChat/0.26.7"));
	HttpReq->SetHeader(TEXT("X-GitHub-Api-Version"), TEXT("2025-04-01"));
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

	// Set default model only if user hasn't chosen one (from cache or /model command)
	if (ActiveModelId.IsEmpty() && AvailableModels.Num() > 0)
	{
		// Prefer Claude over GPT if available
		for (const FCopilotModel& M : AvailableModels)
		{
			if (M.Id.Contains(TEXT("claude")))
			{
				ActiveModelId = M.Id;
				break;
			}
		}
		// Fallback to chat default, then first model
		if (ActiveModelId.IsEmpty())
		{
			for (const FCopilotModel& M : AvailableModels)
			{
				if (M.bIsChatDefault)
				{
					ActiveModelId = M.Id;
					break;
				}
			}
		}
		if (ActiveModelId.IsEmpty())
		{
			ActiveModelId = AvailableModels[0].Id;
		}
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
	ActiveModelId = CleanId;
	Log(FString::Printf(TEXT("BridgeService: Active model set to: %s"), *ActiveModelId));
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

	RefreshCopilotTokenIfNeeded();

	Log(FString::Printf(TEXT("BridgeService: Sending request %s to Copilot (model: %s)"), *Request.RequestId, *ActiveModelId));
	PendingRequestTimestamps.Add(Request.RequestId, FPlatformTime::Seconds());

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
	return TEXT("You have tools: read_file, write_file, edit_file, list_directory, search_files, get_project_structure, create_cpp_class, compile, get_file_info, delete_file. Use them when needed.");
}

void FGitHubCopilotUEBridgeService::SendChatCompletion(const FCopilotRequest& Request)
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

	// Build or continue the conversation messages
	TArray<TSharedPtr<FJsonValue>>& ConvoMessages = ActiveConversations.FindOrAdd(Request.RequestId);

	// If this is a new conversation (no messages yet), add system + user messages
	if (ConvoMessages.Num() == 0)
	{
		// System message
		TSharedPtr<FJsonObject> SystemMsg = MakeShareable(new FJsonObject);
		SystemMsg->SetStringField(TEXT("role"), TEXT("system"));
		SystemMsg->SetStringField(TEXT("content"), BuildSystemPrompt(Request));
		ConvoMessages.Add(MakeShareable(new FJsonValueObject(SystemMsg)));

		// User message
		FString UserContent = Request.UserPrompt;
		if (UserContent.IsEmpty())
		{
			UserContent = CommandTypeToString(Request.CommandType);
		}

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

		TSharedPtr<FJsonObject> UserMsg = MakeShareable(new FJsonObject);
		UserMsg->SetStringField(TEXT("role"), TEXT("user"));
		UserMsg->SetStringField(TEXT("content"), UserContent);
		ConvoMessages.Add(MakeShareable(new FJsonValueObject(UserMsg)));
	}
	// If continuing (tool results already appended), messages are ready

	// Build request body
	TSharedPtr<FJsonObject> JsonBody = MakeShareable(new FJsonObject);
	JsonBody->SetStringField(TEXT("model"), ModelToUse);
	JsonBody->SetArrayField(TEXT("messages"), ConvoMessages);

	// Add tool definitions — this is what makes it agentic
	TArray<TSharedPtr<FJsonValue>> Tools = FGitHubCopilotUEToolExecutor::BuildToolDefinitions();
	JsonBody->SetArrayField(TEXT("tools"), Tools);

	// Explicitly tell the API to allow tool calls
	JsonBody->SetStringField(TEXT("tool_choice"), TEXT("auto"));

	Log(FString::Printf(TEXT("BridgeService: Sending %d tools, %d messages, model=%s"), Tools.Num(), ConvoMessages.Num(), *ModelToUse));

	FString RequestBody;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&RequestBody);
	FJsonSerializer::Serialize(JsonBody.ToSharedRef(), Writer);

	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> HttpReq = FHttpModule::Get().CreateRequest();
	HttpReq->SetURL(GetEndpointURLForModel(ModelToUse));
	HttpReq->SetVerb(TEXT("POST"));
	HttpReq->SetHeader(TEXT("Authorization"), FString::Printf(TEXT("Bearer %s"), *CopilotToken));
	HttpReq->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
	HttpReq->SetHeader(TEXT("Accept"), TEXT("application/json"));
	HttpReq->SetHeader(TEXT("Copilot-Integration-Id"), TEXT("vscode-chat"));
	HttpReq->SetHeader(TEXT("Editor-Version"), TEXT("vscode/1.104.3"));
	HttpReq->SetHeader(TEXT("Editor-Plugin-Version"), TEXT("copilot-chat/0.26.7"));
	HttpReq->SetHeader(TEXT("User-Agent"), TEXT("GitHubCopilotChat/0.26.7"));
	HttpReq->SetHeader(TEXT("OpenAI-Intent"), TEXT("conversation-panel"));
	HttpReq->SetHeader(TEXT("X-GitHub-Api-Version"), TEXT("2025-04-01"));
	HttpReq->SetHeader(TEXT("X-Initiator"), TEXT("user"));
	HttpReq->SetHeader(TEXT("X-Request-Id"), FGuid::NewGuid().ToString());
	HttpReq->SetContentAsString(RequestBody);

	const UGitHubCopilotUESettings* Settings = UGitHubCopilotUESettings::Get();
	if (Settings)
	{
		HttpReq->SetTimeout(Settings->TimeoutSeconds);
	}

	FString RequestId = Request.RequestId;
	HttpReq->OnProcessRequestComplete().BindRaw(this, &FGitHubCopilotUEBridgeService::OnChatCompletionResponse, RequestId);
	HttpReq->ProcessRequest();
}

void FGitHubCopilotUEBridgeService::OnChatCompletionResponse(FHttpRequestPtr HttpReq, FHttpResponsePtr HttpResp, bool bSuccess, FString RequestId)
{
	FCopilotResponse Response;
	Response.RequestId = RequestId;
	Response.Timestamp = FDateTime::Now().ToString();

	if (!bSuccess || !HttpResp.IsValid())
	{
		PendingRequestTimestamps.Remove(RequestId);
		ActiveConversations.Remove(RequestId);
		Response.ResultStatus = ECopilotResultStatus::Failure;
		Response.ErrorMessage = TEXT("Request to Copilot failed - no response");
		Log(FString::Printf(TEXT("BridgeService: Request %s failed"), *RequestId));
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
		ActiveConversations.Remove(RequestId);
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
		ActiveConversations.Remove(RequestId);
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
		ActiveConversations.Remove(RequestId);
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

	// Debug logging
	FString ResponseModel = Json->HasField(TEXT("model")) ? Json->GetStringField(TEXT("model")) : TEXT("unknown");
	bool bHasToolCalls = Message.IsValid() && Message->HasField(TEXT("tool_calls"));
	Log(FString::Printf(TEXT("BridgeService: Response — model=%s, finish_reason=%s, choices=%d, has_tool_calls=%s"),
		*ResponseModel, *FinishReason, Choices->Num(), bHasToolCalls ? TEXT("YES") : TEXT("NO")));

	if (!Message.IsValid())
	{
		PendingRequestTimestamps.Remove(RequestId);
		ActiveConversations.Remove(RequestId);
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
		TArray<TSharedPtr<FJsonValue>>& ConvoMessages = ActiveConversations.FindOrAdd(RequestId);
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

			// Execute the tool
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

			Log(FString::Printf(TEXT("BridgeService: Tool %s returned %d chars"), *ToolName, ToolResult.Len()));

			// Append tool result to conversation
			TSharedPtr<FJsonObject> ToolResultMsg = MakeShareable(new FJsonObject);
			ToolResultMsg->SetStringField(TEXT("role"), TEXT("tool"));
			ToolResultMsg->SetStringField(TEXT("tool_call_id"), ToolCallId);
			ToolResultMsg->SetStringField(TEXT("name"), ToolName);
			ToolResultMsg->SetStringField(TEXT("content"), ToolResult);
			ConvoMessages.Add(MakeShareable(new FJsonValueObject(ToolResultMsg)));
		}

		// Check iteration guard
		int32& IterCount = ToolCallIterations.FindOrAdd(RequestId);
		IterCount++;
		if (IterCount > 25)
		{
			PendingRequestTimestamps.Remove(RequestId);
			ActiveConversations.Remove(RequestId);
			ToolCallIterations.Remove(RequestId);
			Response.ResultStatus = ECopilotResultStatus::Failure;
			Response.ErrorMessage = TEXT("Tool call loop exceeded 25 iterations — stopping.");
			Response.bSuccess = false;
			OnResponseReceived.Broadcast(Response);
			return;
		}

		// Send the conversation back to the model (it will see tool results and continue)
		Log(FString::Printf(TEXT("BridgeService: Tool loop iteration %d for request %s"), IterCount, *RequestId));

		FCopilotRequest ContinuationRequest;
		ContinuationRequest.RequestId = RequestId;
		ContinuationRequest.CommandType = ECopilotCommandType::Ask;
		// Don't rebuild messages — SendChatCompletion will use ActiveConversations
		SendChatCompletion(ContinuationRequest);
		return;
	}

HandleNormalResponse:
	// ════════════════════════════════════════════════════════════════
	// FINAL RESPONSE — model is done, return to user
	// ════════════════════════════════════════════════════════════════

	PendingRequestTimestamps.Remove(RequestId);
	ActiveConversations.Remove(RequestId);
	ToolCallIterations.Remove(RequestId);

	FString Content = Message->HasField(TEXT("content")) ? Message->GetStringField(TEXT("content")) : TEXT("");

	// If content was in a separate choice (Claude split-choice format), use that
	if (Content.IsEmpty() && !PrefixContent.IsEmpty())
	{
		Content = PrefixContent;
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
