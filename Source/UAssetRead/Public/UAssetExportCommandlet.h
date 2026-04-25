// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"
#include "UAssetExportCommandlet.generated.h"

/**
 * Commandlet to export UAsset files to JSON/YAML.
 *
 * Usage:
 *   UnrealEditor-Cmd.exe <ProjectPath> -run=AssetExport
 *       -AssetPath="/Game/Characters/PlayerCharacter"   (or a directory)
 *       -OutputDir="D:/Export"
 *       -Format=json
 *       -Recursive
 *       -Filter="Blueprint,DataTable"
 */
UCLASS()
class UAssetExportCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	UAssetExportCommandlet();
	virtual int32 Main(const FString& Params) override;

private:
	/** Collect FAssetData list from AssetRegistry for the given path. */
	void CollectAssets(const FString& AssetPath, bool bRecursive,
	                   const TArray<FString>& TypeFilter,
	                   TArray<struct FAssetData>& OutAssets) const;
};
