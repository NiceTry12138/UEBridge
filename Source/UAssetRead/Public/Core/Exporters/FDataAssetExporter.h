// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Core/IAssetExporter.h"

/** Exports UDataTable and UDataAsset assets. */
class FDataAssetExporter : public IAssetExporter
{
public:
	virtual TSharedPtr<FJsonObject> Export(UObject* Asset) override;
};
