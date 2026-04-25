// Copyright Epic Games, Inc. All Rights Reserved.

#include "Core/FAssetExportCore.h"
#include "Core/Utils/FJsonYamlWriter.h"

// Exporters
#include "Core/Exporters/FBlueprintExporter.h"
#include "Core/Exporters/FDataAssetExporter.h"
#include "Core/Exporters/FMeshExporter.h"
#include "Core/Exporters/FMediaExporter.h"
#include "Core/Exporters/FMaterialExporter.h"
#include "Core/Exporters/FWidgetExporter.h"

// UE asset types
#include "Engine/Blueprint.h"
#include "Engine/DataTable.h"
#include "Engine/DataAsset.h"
#include "Engine/StaticMesh.h"
#include "Engine/SkeletalMesh.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstance.h"
#include "Sound/SoundWave.h"
#include "Engine/Texture2D.h"

// UWidgetBlueprint is checked via class name to avoid mandatory UMGEditor dep here
#include "WidgetBlueprint.h"

TSharedPtr<FJsonObject> FAssetExportCore::ExportAsset(UObject* Asset)
{
	if (!Asset)
	{
		return nullptr;
	}

	// WidgetBlueprint must be checked before UBlueprint
	if (UWidgetBlueprint* WBP = Cast<UWidgetBlueprint>(Asset))
	{
		FWidgetExporter Exporter;
		return Exporter.Export(WBP);
	}

	if (UBlueprint* BP = Cast<UBlueprint>(Asset))
	{
		FBlueprintExporter Exporter;
		return Exporter.Export(BP);
	}

	if (Cast<UDataTable>(Asset) || Cast<UDataAsset>(Asset))
	{
		FDataAssetExporter Exporter;
		return Exporter.Export(Asset);
	}

	if (Cast<UStaticMesh>(Asset) || Cast<USkeletalMesh>(Asset))
	{
		FMeshExporter Exporter;
		return Exporter.Export(Asset);
	}

	if (Cast<USoundWave>(Asset) || Cast<UTexture2D>(Asset))
	{
		FMediaExporter Exporter;
		return Exporter.Export(Asset);
	}

	if (Cast<UMaterial>(Asset) || Cast<UMaterialInstance>(Asset))
	{
		FMaterialExporter Exporter;
		return Exporter.Export(Asset);
	}

	// Unsupported type – return minimal info
	TSharedPtr<FJsonObject> Fallback = MakeShareable(new FJsonObject);
	Fallback->SetStringField(TEXT("assetPath"), Asset->GetPathName());
	Fallback->SetStringField(TEXT("assetType"), Asset->GetClass()->GetName());
	Fallback->SetStringField(TEXT("note"), TEXT("No exporter available for this asset type"));
	return Fallback;
}

FString FAssetExportCore::ExportAssetToString(UObject* Asset)
{
	TSharedPtr<FJsonObject> JsonObject = ExportAsset(Asset);
	if (!JsonObject.IsValid())
	{
		return FString();
	}
	return FJsonYamlWriter::ToJsonString(JsonObject);
}
