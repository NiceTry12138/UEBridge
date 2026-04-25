// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

/**
 * Main dispatch point for asset-to-JSON export.
 * Selects the right exporter based on UObject type and returns the full JSON object.
 */
class UASSETREAD_API FAssetExportCore
{
public:
	/**
	 * Export a UObject to a JSON object.
	 * Loads the object if needed. Returns nullptr if the type is unsupported.
	 */
	static TSharedPtr<FJsonObject> ExportAsset(UObject* Asset);

	/**
	 * Export a UObject and serialize the JSON to a pretty-printed string.
	 * Returns an empty string on failure.
	 */
	static FString ExportAssetToString(UObject* Asset);
};
