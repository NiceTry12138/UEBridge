// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

/**
 * Interface for per-type asset exporters.
 * Each concrete exporter handles one asset type and returns a structured FJsonObject.
 */
class IAssetExporter
{
public:
	virtual ~IAssetExporter() {}

	/** Export the given asset to a JSON object. Returns nullptr on failure. */
	virtual TSharedPtr<FJsonObject> Export(UObject* Asset) = 0;
};
