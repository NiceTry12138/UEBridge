// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Core/IAssetExporter.h"

/** Exports UMaterial and UMaterialInstance assets. */
class FMaterialExporter : public IAssetExporter
{
public:
	virtual TSharedPtr<FJsonObject> Export(UObject* Asset) override;
};
