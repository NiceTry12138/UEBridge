// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Core/IAssetExporter.h"

/**
 * Exports UBlueprint assets: properties, functions, graphs, inheritance chain,
 * interfaces, component hierarchy, and (for WidgetBlueprint) widget tree.
 */
class FBlueprintExporter : public IAssetExporter
{
public:
	virtual TSharedPtr<FJsonObject> Export(UObject* Asset) override;
};
