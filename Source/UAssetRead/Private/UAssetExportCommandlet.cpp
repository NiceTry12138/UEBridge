// Copyright Epic Games, Inc. All Rights Reserved.

#include "UAssetExportCommandlet.h"
#include "Core/FAssetExportCore.h"
#include "Core/Utils/FJsonYamlWriter.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "AssetRegistry/AssetData.h"
#include "Misc/CommandLine.h"
#include "Misc/Paths.h"
#include "HAL/PlatformFileManager.h"

#define LOCTEXT_NAMESPACE "UAssetExportCommandlet"

UAssetExportCommandlet::UAssetExportCommandlet()
{
	IsClient = false;
	IsEditor = true;
	IsServer = false;
	LogToConsole = true;
}

void UAssetExportCommandlet::CollectAssets(const FString& AssetPath,
                                             bool bRecursive,
                                             const TArray<FString>& TypeFilter,
                                             TArray<FAssetData>& OutAssets) const
{
	FAssetRegistryModule& AssetRegistryModule =
		FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
	IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

	// Wait for scan to complete (commandlet context)
	AssetRegistry.SearchAllAssets(true);

	FARFilter Filter;
	Filter.bRecursivePaths = bRecursive;

	// Distinguish single asset vs directory
	if (AssetPath.EndsWith(TEXT("/")))
	{
		Filter.PackagePaths.Add(FName(*AssetPath));
	}
	else
	{
		// Could be a package path like /Game/Characters/PlayerCharacter
		Filter.PackageNames.Add(FName(*AssetPath));
	}

	if (TypeFilter.Num() > 0)
	{
		for (const FString& TypeName : TypeFilter)
		{
			Filter.ClassPaths.Add(FTopLevelAssetPath(TEXT("/Script/Engine"), *TypeName));
		}
	}

	AssetRegistry.GetAssets(Filter, OutAssets);
}

int32 UAssetExportCommandlet::Main(const FString& Params)
{
	// Parse command line parameters
	FString AssetPath;
	if (!FParse::Value(*Params, TEXT("AssetPath="), AssetPath))
	{
		UE_LOG(LogTemp, Error, TEXT("UAssetExportCommandlet: -AssetPath is required."));
		return 1;
	}

	const bool bStdout = FParse::Param(*Params, TEXT("stdout"));

	FString OutputDir;
	if (!bStdout)
	{
		if (!FParse::Value(*Params, TEXT("OutputDir="), OutputDir))
		{
			OutputDir = FPaths::ProjectSavedDir() / TEXT("AssetExport");
		}
	}

	FString FormatStr = TEXT("json");
	FParse::Value(*Params, TEXT("Format="), FormatStr);
	const FJsonYamlWriter::EOutputFormat Format =
		FormatStr.ToLower() == TEXT("yaml")
		? FJsonYamlWriter::EOutputFormat::Yaml
		: FJsonYamlWriter::EOutputFormat::Json;

	const bool bRecursive = FParse::Param(*Params, TEXT("Recursive"));

	FString FilterStr;
	TArray<FString> TypeFilter;
	if (FParse::Value(*Params, TEXT("Filter="), FilterStr))
	{
		FilterStr.ParseIntoArray(TypeFilter, TEXT(","), true);
	}

	UE_LOG(LogTemp, Log, TEXT("UAssetExportCommandlet: Collecting assets from '%s'..."), *AssetPath);

	TArray<FAssetData> Assets;
	CollectAssets(AssetPath, bRecursive, TypeFilter, Assets);

	UE_LOG(LogTemp, Log, TEXT("UAssetExportCommandlet: Found %d assets."), Assets.Num());

	int32 Succeeded = 0;
	int32 Failed = 0;

	// Collect all JSON lines for stdout mode
	TArray<FString> StdoutLines;

	for (const FAssetData& AssetData : Assets)
	{
		UObject* Asset = AssetData.GetAsset();
		if (!Asset)
		{
			UE_LOG(LogTemp, Warning, TEXT("Failed to load: %s"), *AssetData.PackageName.ToString());
			++Failed;
			continue;
		}

		TSharedPtr<FJsonObject> JsonObj = FAssetExportCore::ExportAsset(Asset);
		if (!JsonObj.IsValid())
		{
			UE_LOG(LogTemp, Warning, TEXT("No exporter for: %s"), *AssetData.PackageName.ToString());
			++Failed;
			continue;
		}

		if (bStdout)
		{
			FString JsonStr = FJsonYamlWriter::ToJsonString(JsonObj);
			// Collapse to single line for JSONL output
			JsonStr.ReplaceInline(TEXT("\n"), TEXT(" "));
			JsonStr.ReplaceInline(TEXT("\r"), TEXT(""));
			StdoutLines.Add(MoveTemp(JsonStr));
			++Succeeded;
		}
		else
		{
			// Mirror the asset path under OutputDir
			// /Game/Characters/PlayerCharacter -> OutputDir/Game/Characters/PlayerCharacter.json
			FString RelativePath = AssetData.PackageName.ToString();
			RelativePath.RemoveFromStart(TEXT("/"));
			const FString OutputFile = OutputDir / RelativePath + TEXT(".json");

			if (FJsonYamlWriter::WriteToFile(JsonObj, OutputFile, Format))
			{
				UE_LOG(LogTemp, Log, TEXT("Exported: %s"), *OutputFile);
				++Succeeded;
			}
			else
			{
				UE_LOG(LogTemp, Error, TEXT("Write failed: %s"), *OutputFile);
				++Failed;
			}
		}
	}

	if (bStdout)
	{
		// Flush log first so it doesn't interleave with our sentinel output
		GLog->Flush();
		FPlatformMisc::LocalPrint(TEXT("\n<<<ASSET_DUMP_BEGIN>>>\n"));
		for (const FString& Line : StdoutLines)
		{
			FPlatformMisc::LocalPrint(*Line);
			FPlatformMisc::LocalPrint(TEXT("\n"));
		}
		FPlatformMisc::LocalPrint(TEXT("<<<ASSET_DUMP_END>>>\n"));
		GLog->Flush();
	}

	UE_LOG(LogTemp, Log,
		TEXT("UAssetExportCommandlet: Done. Succeeded=%d  Failed=%d"), Succeeded, Failed);
	return (Failed == 0) ? 0 : 1;
}

#undef LOCTEXT_NAMESPACE
