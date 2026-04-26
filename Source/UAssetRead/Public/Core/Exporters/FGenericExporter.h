// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Core/IAssetExporter.h"

/**
 * Generic fallback exporter.
 * Uses UE reflection to dump all obtainable information about any UObject:
 *   - Basic info (path, type, outer)
 *   - Full class hierarchy
 *   - Implemented interfaces
 *   - All UPROPERTY values  (via FJsonObjectConverter — proper JSON types)
 *   - All UPROPERTY metadata (type, declaring class, notable flags)
 *   - All UFUNCTION signatures (params, access, flags)
 */
class FGenericExporter : public IAssetExporter
{
public:
	virtual TSharedPtr<FJsonObject> Export(UObject* Asset) override;
};
