// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Core/IAssetExporter.h"

/** Exports UStaticMesh and USkeletalMesh assets. */
class FMeshExporter : public IAssetExporter
{
public:
	virtual TSharedPtr<FJsonObject> Export(UObject* Asset) override;
};
