// Copyright GitHub, Inc. All Rights Reserved.

#include "Services/GitHubCopilotUETypes.h"

DEFINE_LOG_CATEGORY(LogGitHubCopilotUE);

// ============================================================================
// FCopilotProjectContext
// ============================================================================

TSharedPtr<FJsonObject> FCopilotProjectContext::ToJson() const
{
	TSharedPtr<FJsonObject> Json = MakeShareable(new FJsonObject());
	Json->SetStringField(TEXT("projectName"), ProjectName);
	Json->SetStringField(TEXT("engineVersion"), EngineVersion);
	Json->SetStringField(TEXT("currentMapName"), CurrentMapName);
	Json->SetStringField(TEXT("activePlatform"), ActivePlatform);
	Json->SetStringField(TEXT("questReadinessSummary"), QuestReadinessSummary);

	TArray<TSharedPtr<FJsonValue>> AssetsArray;
	for (const FString& Asset : SelectedAssets)
	{
		AssetsArray.Add(MakeShareable(new FJsonValueString(Asset)));
	}
	Json->SetArrayField(TEXT("selectedAssets"), AssetsArray);

	TArray<TSharedPtr<FJsonValue>> ActorsArray;
	for (const FString& Actor : SelectedActors)
	{
		ActorsArray.Add(MakeShareable(new FJsonValueString(Actor)));
	}
	Json->SetArrayField(TEXT("selectedActors"), ActorsArray);

	TArray<TSharedPtr<FJsonValue>> PluginsArray;
	for (const FString& Plugin : EnabledPlugins)
	{
		PluginsArray.Add(MakeShareable(new FJsonValueString(Plugin)));
	}
	Json->SetArrayField(TEXT("enabledPlugins"), PluginsArray);

	TArray<TSharedPtr<FJsonValue>> XRArray;
	for (const FString& XR : EnabledXRPlugins)
	{
		XRArray.Add(MakeShareable(new FJsonValueString(XR)));
	}
	Json->SetArrayField(TEXT("enabledXRPlugins"), XRArray);

	TArray<TSharedPtr<FJsonValue>> SourceDirsArray;
	for (const FString& Dir : ProjectSourceDirectories)
	{
		SourceDirsArray.Add(MakeShareable(new FJsonValueString(Dir)));
	}
	Json->SetArrayField(TEXT("projectSourceDirectories"), SourceDirsArray);

	TArray<TSharedPtr<FJsonValue>> ModulesArray;
	for (const FString& Module : ModuleNames)
	{
		ModulesArray.Add(MakeShareable(new FJsonValueString(Module)));
	}
	Json->SetArrayField(TEXT("moduleNames"), ModulesArray);

	return Json;
}

FCopilotProjectContext FCopilotProjectContext::FromJson(const TSharedPtr<FJsonObject>& JsonObject)
{
	FCopilotProjectContext Ctx;
	if (!JsonObject.IsValid()) return Ctx;

	Ctx.ProjectName = JsonObject->GetStringField(TEXT("projectName"));
	Ctx.EngineVersion = JsonObject->GetStringField(TEXT("engineVersion"));
	Ctx.CurrentMapName = JsonObject->GetStringField(TEXT("currentMapName"));
	Ctx.ActivePlatform = JsonObject->GetStringField(TEXT("activePlatform"));
	Ctx.QuestReadinessSummary = JsonObject->GetStringField(TEXT("questReadinessSummary"));

	const TArray<TSharedPtr<FJsonValue>>* ArrayPtr;
	if (JsonObject->TryGetArrayField(TEXT("selectedAssets"), ArrayPtr))
	{
		for (const auto& Val : *ArrayPtr)
			Ctx.SelectedAssets.Add(Val->AsString());
	}
	if (JsonObject->TryGetArrayField(TEXT("selectedActors"), ArrayPtr))
	{
		for (const auto& Val : *ArrayPtr)
			Ctx.SelectedActors.Add(Val->AsString());
	}
	if (JsonObject->TryGetArrayField(TEXT("enabledPlugins"), ArrayPtr))
	{
		for (const auto& Val : *ArrayPtr)
			Ctx.EnabledPlugins.Add(Val->AsString());
	}

	return Ctx;
}

// ============================================================================
// FCopilotFileTarget
// ============================================================================

TSharedPtr<FJsonObject> FCopilotFileTarget::ToJson() const
{
	TSharedPtr<FJsonObject> Json = MakeShareable(new FJsonObject());
	Json->SetStringField(TEXT("filePath"), FilePath);
	Json->SetNumberField(TEXT("lineStart"), LineStart);
	Json->SetNumberField(TEXT("lineEnd"), LineEnd);
	Json->SetStringField(TEXT("selectedText"), SelectedText);
	return Json;
}

TSharedPtr<FJsonObject> FCopilotAttachment::ToJson() const
{
	TSharedPtr<FJsonObject> Json = MakeShareable(new FJsonObject());
	Json->SetStringField(TEXT("filePath"), FilePath);
	Json->SetStringField(TEXT("mimeType"), MimeType);
	return Json;
}

// ============================================================================
// FCopilotRequest
// ============================================================================

TSharedPtr<FJsonObject> FCopilotRequest::ToJson() const
{
	TSharedPtr<FJsonObject> Json = MakeShareable(new FJsonObject());
	Json->SetStringField(TEXT("requestId"), RequestId);
	Json->SetNumberField(TEXT("commandType"), (int32)CommandType);
	Json->SetNumberField(TEXT("executionMode"), (int32)ExecutionMode);
	Json->SetStringField(TEXT("userPrompt"), UserPrompt);
	Json->SetStringField(TEXT("timestamp"), Timestamp);

	Json->SetObjectField(TEXT("projectContext"), ProjectContext.ToJson());

	TArray<TSharedPtr<FJsonValue>> TargetsArray;
	for (const FCopilotFileTarget& Target : FileTargets)
	{
		TargetsArray.Add(MakeShareable(new FJsonValueObject(Target.ToJson())));
	}
	Json->SetArrayField(TEXT("fileTargets"), TargetsArray);

	TArray<TSharedPtr<FJsonValue>> AttachmentsArray;
	for (const FCopilotAttachment& Attachment : Attachments)
	{
		AttachmentsArray.Add(MakeShareable(new FJsonValueObject(Attachment.ToJson())));
	}
	Json->SetArrayField(TEXT("attachments"), AttachmentsArray);

	// Command arguments
	TSharedPtr<FJsonObject> ArgsJson = MakeShareable(new FJsonObject());
	for (const auto& Pair : CommandArguments)
	{
		ArgsJson->SetStringField(Pair.Key, Pair.Value);
	}
	Json->SetObjectField(TEXT("commandArguments"), ArgsJson);

	return Json;
}

// ============================================================================
// FCopilotResponse
// ============================================================================

FCopilotResponse FCopilotResponse::FromJson(const TSharedPtr<FJsonObject>& JsonObject)
{
	FCopilotResponse Response;
	if (!JsonObject.IsValid()) return Response;

	Response.RequestId = JsonObject->GetStringField(TEXT("requestId"));
	Response.ResponseText = JsonObject->GetStringField(TEXT("responseText"));
	Response.ErrorMessage = JsonObject->GetStringField(TEXT("errorMessage"));
	Response.Timestamp = JsonObject->GetStringField(TEXT("timestamp"));

	int32 StatusInt = JsonObject->GetIntegerField(TEXT("resultStatus"));
	Response.ResultStatus = static_cast<ECopilotResultStatus>(FMath::Clamp(StatusInt, 0, 4));

	// Parse diff preview if present
	const TSharedPtr<FJsonObject>* DiffJson;
	if (JsonObject->TryGetObjectField(TEXT("diffPreview"), DiffJson))
	{
		Response.DiffPreview.OriginalFilePath = (*DiffJson)->GetStringField(TEXT("originalFilePath"));
		Response.DiffPreview.OriginalContent = (*DiffJson)->GetStringField(TEXT("originalContent"));
		Response.DiffPreview.ProposedContent = (*DiffJson)->GetStringField(TEXT("proposedContent"));
		Response.DiffPreview.UnifiedDiff = (*DiffJson)->GetStringField(TEXT("unifiedDiff"));
		Response.DiffPreview.bIsValid = (*DiffJson)->GetBoolField(TEXT("isValid"));
	}

	// Parse provider metadata
	const TSharedPtr<FJsonObject>* MetaJson;
	if (JsonObject->TryGetObjectField(TEXT("providerMetadata"), MetaJson))
	{
		for (const auto& Pair : (*MetaJson)->Values)
		{
			Response.ProviderMetadata.Add(Pair.Key, Pair.Value->AsString());
		}
	}

	return Response;
}
