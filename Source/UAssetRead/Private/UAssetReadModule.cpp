// Copyright Epic Games, Inc. All Rights Reserved.

#include "UAssetReadModule.h"
#include "Core/FAssetExportCore.h"

// Menu
#include "ToolMenus.h"
#include "ContentBrowserMenuContexts.h"
#include "HAL/PlatformApplicationMisc.h"
#include "AssetRegistry/AssetData.h"

// HTTP server
#include "HttpServerModule.h"
#include "IHttpRouter.h"
#include "HttpServerRequest.h"
#include "HttpServerResponse.h"
#include "HttpPath.h"

// JSON
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

// Async
#include "Async/Async.h"

// Asset loading
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "UObject/UObjectGlobals.h"

#define LOCTEXT_NAMESPACE "UAssetReadModule"

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

#include "UAssetReadHelpers.h"

// ---------------------------------------------------------------------------
// Module lifecycle
// ---------------------------------------------------------------------------

void FUAssetReadModule::StartupModule()
{
	// Register right-click menu after ToolMenus are ready
	UToolMenus::RegisterStartupCallback(
		FSimpleMulticastDelegate::FDelegate::CreateRaw(
			this, &FUAssetReadModule::RegisterMenuExtensions));

	StartHttpServer();
}

void FUAssetReadModule::ShutdownModule()
{
	UnregisterMenuExtensions();

	// Mark all pending AsyncTask lambdas as stale
	*bAlive = false;

	// Evict cache (RemoveFromRoot on all rooted assets)
	for (auto& Pair : AssetCache)
	{
		EvictCacheEntry(Pair.Value);
	}
	AssetCache.Empty();

	StopHttpServer();
}

// ---------------------------------------------------------------------------
// HTTP server lifecycle
// ---------------------------------------------------------------------------

void FUAssetReadModule::StartHttpServer()
{
	FHttpServerModule& HttpModule =
		FModuleManager::LoadModuleChecked<FHttpServerModule>(TEXT("HTTPServer"));

	HttpRouter = HttpModule.GetHttpRouter(HttpPort);
	if (!HttpRouter.IsValid())
	{
		UE_LOG(LogTemp, Warning,
			TEXT("UAssetRead: Failed to get HTTP router on port %u"), HttpPort);
		return;
	}

	auto BindVerb = [&](const TCHAR* Path, EHttpServerRequestVerbs Verb, auto Handler)
	{
		FUAssetReadModule* Self = this;
		FHttpRequestHandler Callback = FHttpRequestHandler::CreateLambda(
			[Self, Handler](const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete) -> bool
			{
				return (Self->*Handler)(Req, OnComplete);
			});
		RouteHandles.Add(HttpRouter->BindRoute(
			FHttpPath(Path), Verb, Callback));
	};

	BindVerb(TEXT("/dump_asset"),  EHttpServerRequestVerbs::VERB_POST, &FUAssetReadModule::HandleDumpAsset);
	BindVerb(TEXT("/list_assets"), EHttpServerRequestVerbs::VERB_GET,  &FUAssetReadModule::HandleListAssets);
	BindVerb(TEXT("/health"),      EHttpServerRequestVerbs::VERB_GET,  &FUAssetReadModule::HandleHealth);

	// Asset inspection routes
	BindVerb(TEXT("/get_asset_references"),    EHttpServerRequestVerbs::VERB_POST, &FUAssetReadModule::HandleGetAssetReferences);
	BindVerb(TEXT("/dump_niagara_system"),      EHttpServerRequestVerbs::VERB_POST, &FUAssetReadModule::HandleDumpNiagaraSystem);
	BindVerb(TEXT("/dump_level_sequence"),      EHttpServerRequestVerbs::VERB_POST, &FUAssetReadModule::HandleDumpLevelSequence);
	BindVerb(TEXT("/dump_widget_tree"),         EHttpServerRequestVerbs::VERB_POST, &FUAssetReadModule::HandleDumpWidgetTree);
	BindVerb(TEXT("/dump_animation_blueprint"), EHttpServerRequestVerbs::VERB_POST, &FUAssetReadModule::HandleDumpAnimBlueprint);

	// Blueprint CRUD routes
	BindVerb(TEXT("/create_blueprint"),         EHttpServerRequestVerbs::VERB_POST, &FUAssetReadModule::HandleCreateBlueprint);
	BindVerb(TEXT("/add_blueprint_component"),  EHttpServerRequestVerbs::VERB_POST, &FUAssetReadModule::HandleAddBlueprintComponent);
	BindVerb(TEXT("/compile_blueprint"),        EHttpServerRequestVerbs::VERB_POST, &FUAssetReadModule::HandleCompileBlueprint);
	BindVerb(TEXT("/add_blueprint_variable"),   EHttpServerRequestVerbs::VERB_POST, &FUAssetReadModule::HandleAddBlueprintVariable);
	BindVerb(TEXT("/get_blueprint_info"),       EHttpServerRequestVerbs::VERB_GET,  &FUAssetReadModule::HandleGetBlueprintInfo);
	BindVerb(TEXT("/list_blueprints"),          EHttpServerRequestVerbs::VERB_GET,  &FUAssetReadModule::HandleListBlueprints);

	// Blueprint Node routes
	BindVerb(TEXT("/add_blueprint_event_node"),    EHttpServerRequestVerbs::VERB_POST, &FUAssetReadModule::HandleAddBlueprintEventNode);
	BindVerb(TEXT("/add_blueprint_function_node"), EHttpServerRequestVerbs::VERB_POST, &FUAssetReadModule::HandleAddBlueprintFunctionNode);
	BindVerb(TEXT("/connect_blueprint_nodes"),     EHttpServerRequestVerbs::VERB_POST, &FUAssetReadModule::HandleConnectBlueprintNodes);
	BindVerb(TEXT("/find_blueprint_nodes"),        EHttpServerRequestVerbs::VERB_POST, &FUAssetReadModule::HandleFindBlueprintNodes);
	BindVerb(TEXT("/add_blueprint_var_get_node"),  EHttpServerRequestVerbs::VERB_POST, &FUAssetReadModule::HandleAddBlueprintVarGetNode);
	BindVerb(TEXT("/add_blueprint_var_set_node"),  EHttpServerRequestVerbs::VERB_POST, &FUAssetReadModule::HandleAddBlueprintVarSetNode);
	BindVerb(TEXT("/get_blueprint_node_pins"),     EHttpServerRequestVerbs::VERB_POST, &FUAssetReadModule::HandleGetBlueprintNodePins);
	BindVerb(TEXT("/delete_blueprint_node"),       EHttpServerRequestVerbs::VERB_POST, &FUAssetReadModule::HandleDeleteBlueprintNode);
	BindVerb(TEXT("/batch_edit_blueprint_nodes"),  EHttpServerRequestVerbs::VERB_POST, &FUAssetReadModule::HandleBatchEditBlueprintNodes);

	HttpModule.StartAllListeners();

	UE_LOG(LogTemp, Log, TEXT("UAssetRead: HTTP server started on port %u"), HttpPort);
}

void FUAssetReadModule::StopHttpServer()
{
	if (FHttpServerModule* HttpModule =
		FModuleManager::GetModulePtr<FHttpServerModule>(TEXT("HTTPServer")))
	{
		HttpModule->StopAllListeners();
	}

	if (HttpRouter.IsValid())
	{
		for (FHttpRouteHandle& Handle : RouteHandles)
		{
			HttpRouter->UnbindRoute(Handle);
		}
		RouteHandles.Empty();
		HttpRouter.Reset();
	}

	UE_LOG(LogTemp, Log, TEXT("UAssetRead: HTTP server stopped"));
}

// ---------------------------------------------------------------------------
// Route: GET /health
// ---------------------------------------------------------------------------

bool FUAssetReadModule::HandleHealth(
	const FHttpServerRequest& /*Request*/, const FHttpResultCallback& OnComplete)
{
	// Trivial – respond immediately on the network thread, no game-thread work needed
	OnComplete(MakeJsonResponse(TEXT("{\"status\":\"ok\"}")));
	return true;
}

// ---------------------------------------------------------------------------
// Route: GET /list_assets?path=/Game/&filter=Blueprint&recursive=true
// ---------------------------------------------------------------------------

bool FUAssetReadModule::HandleListAssets(
	const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
	// Parse query params on network thread (safe – FHttpServerRequest is read-only)
	const FString* PathParam = Request.QueryParams.Find(TEXT("path"));
	if (!PathParam || PathParam->IsEmpty())
	{
		OnComplete(MakeJsonError(EHttpServerResponseCodes::BadRequest,
			TEXT("Missing query parameter: path")));
		return true;
	}

	FString AssetPath    = *PathParam;
	FString FilterStr    = Request.QueryParams.FindRef(TEXT("filter"));
	bool    bRecursive   = Request.QueryParams.FindRef(TEXT("recursive"))
	                           .ToLower() == TEXT("true");

	TArray<FString> TypeFilter;
	if (!FilterStr.IsEmpty())
	{
		FilterStr.ParseIntoArray(TypeFilter, TEXT(","), true);
	}

	TSharedRef<bool> AliveRef = bAlive;

	AsyncTask(ENamedThreads::GameThread,
		[AliveRef, AssetPath = MoveTemp(AssetPath),
		 TypeFilter = MoveTemp(TypeFilter), bRecursive,
		 OnComplete]() mutable
		{
			if (!*AliveRef) { return; }

			IAssetRegistry& AssetRegistry =
				FModuleManager::LoadModuleChecked<FAssetRegistryModule>(
					TEXT("AssetRegistry")).Get();
			AssetRegistry.SearchAllAssets(false);

			FARFilter Filter;
			Filter.bRecursivePaths = bRecursive;
			if (AssetPath.EndsWith(TEXT("/")))
				Filter.PackagePaths.Add(FName(*AssetPath));
			else
				Filter.PackageNames.Add(FName(*AssetPath));

			for (const FString& TypeName : TypeFilter)
			{
				Filter.ClassPaths.Add(
					FTopLevelAssetPath(TEXT("/Script/Engine"), *TypeName));
			}

			TArray<FAssetData> Assets;
			AssetRegistry.GetAssets(Filter, Assets);

			TSharedPtr<FJsonObject> RootObj = MakeShared<FJsonObject>();
			RootObj->SetStringField(TEXT("path"), AssetPath);
			RootObj->SetNumberField(TEXT("count"), Assets.Num());

			TArray<TSharedPtr<FJsonValue>> AssetsJson;
			for (const FAssetData& AD : Assets)
			{
				TSharedPtr<FJsonObject> AObj = MakeShared<FJsonObject>();
				AObj->SetStringField(TEXT("path"), AD.PackageName.ToString());
				AObj->SetStringField(TEXT("type"), AD.AssetClassPath.GetAssetName().ToString());
				AssetsJson.Add(MakeShared<FJsonValueObject>(AObj));
			}
			RootObj->SetArrayField(TEXT("assets"), AssetsJson);

			FString OutStr;
			TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutStr);
			FJsonSerializer::Serialize(RootObj.ToSharedRef(), Writer);

			OnComplete(MakeJsonResponse(OutStr));
		});

	return true;
}

// ---------------------------------------------------------------------------
// Route: POST /dump_asset   body: {"path":"/Game/Foo"}
// ---------------------------------------------------------------------------

bool FUAssetReadModule::HandleDumpAsset(
	const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
	// Parse body on network thread.
	// Request.Body is a raw byte array without a null terminator – add one before
	// passing to UTF8_TO_TCHAR so it does not read beyond the buffer.
	TArray<uint8> BodyBytes = Request.Body;
	BodyBytes.Add(0);
	FString BodyStr = UTF8_TO_TCHAR(reinterpret_cast<const char*>(BodyBytes.GetData()));

	TSharedPtr<FJsonObject> BodyJson;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(BodyStr);
	if (!FJsonSerializer::Deserialize(Reader, BodyJson) || !BodyJson.IsValid())
	{
		OnComplete(MakeJsonError(EHttpServerResponseCodes::BadRequest,
			TEXT("Invalid JSON body")));
		return true;
	}

	FString AssetPath;
	if (!BodyJson->TryGetStringField(TEXT("path"), AssetPath) || AssetPath.IsEmpty())
	{
		OnComplete(MakeJsonError(EHttpServerResponseCodes::BadRequest,
			TEXT("Missing field: path")));
		return true;
	}

	TSharedRef<bool> AliveRef = bAlive;

	AsyncTask(ENamedThreads::GameThread,
		[this, AliveRef, AssetPath = MoveTemp(AssetPath), OnComplete]() mutable
		{
			if (!*AliveRef) { return; }

			UObject* Asset = GetOrLoadAsset(AssetPath);
			if (!Asset)
			{
				OnComplete(MakeJsonError(EHttpServerResponseCodes::NoContent,
					FString::Printf(TEXT("Asset not found: %s"), *AssetPath)));
				return;
			}

			FString JsonStr = FAssetExportCore::ExportAssetToString(Asset);
			if (JsonStr.IsEmpty())
			{
				OnComplete(MakeJsonError(EHttpServerResponseCodes::ServerError,
					TEXT("No exporter for this asset type")));
				return;
			}

			OnComplete(MakeJsonResponse(JsonStr));
		});

	return true;
}

// ---------------------------------------------------------------------------
// Asset cache (game-thread only)
// ---------------------------------------------------------------------------

UObject* FUAssetReadModule::GetOrLoadAsset(const FString& AssetPath)
{
	const double Now = FPlatformTime::Seconds();

	// Cache hit?
	if (FCachedAsset* Cached = AssetCache.Find(AssetPath))
	{
		if (Cached->Asset && IsValid(Cached->Asset))
		{
			Cached->LastAccessTime = Now;
			return Cached->Asset;
		}
		// Stale (GC'd or invalid) – remove and reload
		EvictCacheEntry(*Cached);
		AssetCache.Remove(AssetPath);
	}

	// Load from disk
	UObject* Asset = LoadObject<UObject>(nullptr, *AssetPath);
	if (!Asset)
	{
		return nullptr;
	}

	PruneCache();

	bool bWasRooted = Asset->IsRooted();
	if (!bWasRooted)
	{
		Asset->AddToRoot();
	}
	AssetCache.Add(AssetPath, { Asset, Now, !bWasRooted });
	return Asset;
}

void FUAssetReadModule::PruneCache()
{
	// Evict timed-out entries first
	const double Now = FPlatformTime::Seconds();
	TArray<FString> ToRemove;
	for (auto& Pair : AssetCache)
	{
		if ((Now - Pair.Value.LastAccessTime) > CacheTimeoutSecs)
		{
			EvictCacheEntry(Pair.Value);
			ToRemove.Add(Pair.Key);
		}
	}
	for (const FString& Key : ToRemove) AssetCache.Remove(Key);

	// If still over capacity, evict oldest
	while (AssetCache.Num() >= MaxCacheEntries)
	{
		FString OldestKey;
		double  OldestTime = TNumericLimits<double>::Max();
		for (const auto& Pair : AssetCache)
		{
			if (Pair.Value.LastAccessTime < OldestTime)
			{
				OldestTime = Pair.Value.LastAccessTime;
				OldestKey  = Pair.Key;
			}
		}
		if (!OldestKey.IsEmpty())
		{
			EvictCacheEntry(AssetCache[OldestKey]);
			AssetCache.Remove(OldestKey);
		}
		else break;
	}
}

void FUAssetReadModule::EvictCacheEntry(FCachedAsset& Entry)
{
	if (Entry.bWasRooted && Entry.Asset && Entry.Asset->IsRooted())
	{
		Entry.Asset->RemoveFromRoot();
	}
	Entry.Asset = nullptr;
}

// ---------------------------------------------------------------------------
// Menu extensions (unchanged)
// ---------------------------------------------------------------------------

void FUAssetReadModule::RegisterMenuExtensions()
{
	FToolMenuOwnerScoped OwnerScoped(this);

	UToolMenu* Menu = UToolMenus::Get()->ExtendMenu(TEXT("ContentBrowser.AssetContextMenu"));
	if (!Menu)
	{
		return;
	}

	FToolMenuSection& Section = Menu->FindOrAddSection(TEXT("CommonAssetActions"));

	Section.AddDynamicEntry(TEXT("ExportToJsonEntry"),
		FNewToolMenuSectionDelegate::CreateLambda([](FToolMenuSection& InSection)
		{
			const UContentBrowserAssetContextMenuContext* Context =
				InSection.FindContext<UContentBrowserAssetContextMenuContext>();
			if (!Context || Context->SelectedAssets.IsEmpty())
			{
				return;
			}

			TArray<FAssetData> SelectedAssets = Context->SelectedAssets;

			InSection.AddMenuEntry(
				TEXT("ExportToJson"),
				LOCTEXT("ExportToJson", "Export To Json"),
				LOCTEXT("ExportToJsonTooltip",
					"Export asset data to JSON and copy result to clipboard"),
				FSlateIcon(FAppStyle::GetAppStyleSetName(), TEXT("Icons.Export")),
				FUIAction(FExecuteAction::CreateLambda([SelectedAssets]()
				{
					FString Combined;
					for (const FAssetData& AssetData : SelectedAssets)
					{
						UObject* Asset = AssetData.GetAsset();
						if (!Asset) continue;

						const FString JsonStr = FAssetExportCore::ExportAssetToString(Asset);
						if (!JsonStr.IsEmpty())
						{
							if (!Combined.IsEmpty())
								Combined += TEXT("\n\n// -----------------------------------------------\n\n");
							Combined += JsonStr;
						}
					}

					if (!Combined.IsEmpty())
					{
						FPlatformApplicationMisc::ClipboardCopy(*Combined);
						UE_LOG(LogTemp, Log,
							TEXT("UAssetRead: Exported %d asset(s) to clipboard."),
							SelectedAssets.Num());
					}
					else
					{
						UE_LOG(LogTemp, Warning, TEXT("UAssetRead: Export produced no output."));
					}
				}))
			);
		})
	);
}

void FUAssetReadModule::UnregisterMenuExtensions()
{
	UToolMenus::UnregisterOwner(this);
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FUAssetReadModule, UAssetRead)
