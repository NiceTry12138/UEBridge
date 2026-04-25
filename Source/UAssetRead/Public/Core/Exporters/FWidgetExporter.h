// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Core/IAssetExporter.h"

class UWidget;

/** Exports the widget hierarchy of a UWidgetBlueprint asset. */
class FWidgetExporter : public IAssetExporter
{
public:
	virtual TSharedPtr<FJsonObject> Export(UObject* Asset) override;

private:
	static TSharedPtr<FJsonObject> ExportWidget(UWidget* Widget);
};
