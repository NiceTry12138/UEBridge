// Copyright Epic Games, Inc. All Rights Reserved.
// Route: POST /get_asset_references
// Body: {"path":"/Game/Foo/Bar","direction":"both","depth":1}

#include "UAssetReadModule.h"

#include "HttpServerRequest.h"
#include "HttpServerResponse.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Async/Async.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Misc/PackageName.h"

#include "UAssetReadHelpers.h"

// ---------------------------------------------------------------------------
// Helper: recursively gather dependencies or referencers
// ---------------------------------------------------------------------------

static void GetReferencesRecursive(
	IAssetRegistry& Registry,
	const FName& PackageName,
	bool bDependencies,
	int32 Depth,
	int32 MaxDepth,
	TSet<FName>& Visited,
	TArray<TSharedPtr<FJsonValue>>& OutArray)
{
	if (Depth > MaxDepth || Visited.Contains(PackageName))
	{
		return;
	}
	Visited.Add(PackageName);

	TArray<FName> Results;
	if (bDependencies)
	{
		Registry.GetDependencies(PackageName, Results);
	}
	else
	{
		Registry.GetReferencers(PackageName, Results);
	}

	for (const FName& Ref : Results)
	{
		FString RefStr = Ref.ToString();
		// Only include game content
		if (!RefStr.StartsWith(TEXT("/Game/")))
		{
			continue;
		}

		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("package"), RefStr);
		Entry->SetNumberField(TEXT("depth"), Depth);

		// Try to get asset name and class from registry
		TArray<FAssetData> AssetDataList;
		Registry.GetAssetsByPackageName(Ref, AssetDataList);
		if (AssetDataList.Num() > 0)
		{
			Entry->SetStringField(TEXT("asset_name"), AssetDataList[0].AssetName.ToString());
			Entry->SetStringField(TEXT("class"), AssetDataList[0].AssetClassPath.GetAssetName().ToString());
		}

		OutArray.Add(MakeShared<FJsonValueObject>(Entry));

		// Recurse
		GetReferencesRecursive(Registry, Ref, bDependencies, Depth + 1, MaxDepth, Visited, OutArray);
	}
}

// ---------------------------------------------------------------------------
// Route handler
// ---------------------------------------------------------------------------

bool FUAssetReadModule::HandleGetAssetReferences(
	const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
	TArray<uint8> BodyBytes = Request.Body;
	BodyBytes.Add(0);
	FString BodyStr = UTF8_TO_TCHAR(reinterpret_cast<const char*>(BodyBytes.GetData()));

	TSharedPtr<FJsonObject> BodyJson;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(BodyStr);
	if (!FJsonSerializer::Deserialize(Reader, BodyJson) || !BodyJson.IsValid())
	{
		OnComplete(MakeJsonError(EHttpServerResponseCodes::BadRequest, TEXT("Invalid JSON body")));
		return true;
	}

	FString AssetPath;
	if (!BodyJson->TryGetStringField(TEXT("path"), AssetPath) || AssetPath.IsEmpty())
	{
		OnComplete(MakeJsonError(EHttpServerResponseCodes::BadRequest, TEXT("Missing field: path")));
		return true;
	}

	FString Direction = TEXT("both");
	BodyJson->TryGetStringField(TEXT("direction"), Direction);

	int32 MaxDepth = 1;
	if (const TSharedPtr<FJsonValue>* DepthVal = BodyJson->Values.Find(TEXT("depth")))
	{
		MaxDepth = FMath::Clamp((int32)(*DepthVal)->AsNumber(), 1, 5);
	}

	TSharedRef<bool> AliveRef = bAlive;

	AsyncTask(ENamedThreads::GameThread,
		[AliveRef, AssetPath = MoveTemp(AssetPath), Direction = MoveTemp(Direction), MaxDepth, OnComplete]() mutable
		{
			if (!*AliveRef) { return; }

			IAssetRegistry& Registry =
				FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
			Registry.SearchAllAssets(false);

			FString PackageName = FPackageName::ObjectPathToPackageName(AssetPath);
			FName PackageNameFName(*PackageName);

			TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetStringField(TEXT("asset_path"), AssetPath);

			bool bDeps = Direction.Equals(TEXT("dependencies"), ESearchCase::IgnoreCase)
					  || Direction.Equals(TEXT("both"), ESearchCase::IgnoreCase);
			bool bRefs = Direction.Equals(TEXT("referencers"), ESearchCase::IgnoreCase)
					  || Direction.Equals(TEXT("both"), ESearchCase::IgnoreCase);

			if (bDeps)
			{
				TArray<TSharedPtr<FJsonValue>> DepsArray;
				TSet<FName> Visited;
				Visited.Add(PackageNameFName);
				GetReferencesRecursive(Registry, PackageNameFName, true, 1, MaxDepth, Visited, DepsArray);
				Result->SetArrayField(TEXT("dependencies"), DepsArray);
			}

			if (bRefs)
			{
				TArray<TSharedPtr<FJsonValue>> RefsArray;
				TSet<FName> Visited;
				Visited.Add(PackageNameFName);
				GetReferencesRecursive(Registry, PackageNameFName, false, 1, MaxDepth, Visited, RefsArray);
				Result->SetArrayField(TEXT("referencers"), RefsArray);
			}

			FString OutStr;
			TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutStr);
			FJsonSerializer::Serialize(Result.ToSharedRef(), Writer);

			OnComplete(MakeJsonResponse(OutStr));
		});

	return true;
}
