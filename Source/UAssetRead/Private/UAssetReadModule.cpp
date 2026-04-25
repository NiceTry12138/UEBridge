// Copyright Epic Games, Inc. All Rights Reserved.

#include "UAssetReadModule.h"
#include "Core/FAssetExportCore.h"

#include "ToolMenus.h"
#include "ContentBrowserMenuContexts.h"
#include "HAL/PlatformApplicationMisc.h"
#include "AssetRegistry/AssetData.h"

#define LOCTEXT_NAMESPACE "UAssetReadModule"

void FUAssetReadModule::StartupModule()
{
	// Defer menu registration until ToolMenus are ready
	UToolMenus::RegisterStartupCallback(
		FSimpleMulticastDelegate::FDelegate::CreateRaw(
			this, &FUAssetReadModule::RegisterMenuExtensions));
}

void FUAssetReadModule::ShutdownModule()
{
	UnregisterMenuExtensions();
}

void FUAssetReadModule::RegisterMenuExtensions()
{
	FToolMenuOwnerScoped OwnerScoped(this);

	// Extend the Content Browser asset right-click context menu
	UToolMenu* Menu = UToolMenus::Get()->ExtendMenu(TEXT("ContentBrowser.AssetContextMenu"));
	if (!Menu)
	{
		return;
	}

	// Add to "CommonAssetActions" section (same area as Reload / Save / etc.)
	FToolMenuSection& Section = Menu->FindOrAddSection(TEXT("CommonAssetActions"));

	Section.AddDynamicEntry(TEXT("ExportToJsonEntry"),
		FNewToolMenuSectionDelegate::CreateLambda([](FToolMenuSection& InSection)
		{
			const UContentBrowserAssetContextMenuContext* Context =
				InSection.FindContext<UContentBrowserAssetContextMenuContext>();
			if (!Context || Context->SelectedAssets.IsEmpty())
			{
				return;
			}

			// Capture selected assets by value so the lambda owns them
			TArray<FAssetData> SelectedAssets = Context->SelectedAssets;

			InSection.AddMenuEntry(
				TEXT("ExportToJson"),
				LOCTEXT("ExportToJson", "Export To Json"),
				LOCTEXT("ExportToJsonTooltip",
					"Export asset data to JSON and copy result to clipboard"),
				FSlateIcon(FAppStyle::GetAppStyleSetName(), TEXT("Icons.Export")),
				FUIAction(FExecuteAction::CreateLambda([SelectedAssets]()
				{
					// Build combined JSON string for all selected assets
					FString Combined;
					for (const FAssetData& AssetData : SelectedAssets)
					{
						UObject* Asset = AssetData.GetAsset();
						if (!Asset)
						{
							continue;
						}

						const FString JsonStr = FAssetExportCore::ExportAssetToString(Asset);
						if (!JsonStr.IsEmpty())
						{
							if (!Combined.IsEmpty())
							{
								Combined += TEXT("\n\n// -----------------------------------------------\n\n");
							}
							Combined += JsonStr;
						}
					}

					if (!Combined.IsEmpty())
					{
						FPlatformApplicationMisc::ClipboardCopy(*Combined);
						UE_LOG(LogTemp, Log,
							TEXT("UAssetRead: Exported %d asset(s) to clipboard."),
							SelectedAssets.Num());
					}
					else
					{
						UE_LOG(LogTemp, Warning,
							TEXT("UAssetRead: Export produced no output."));
					}
				}))
			);
		})
	);
}

void FUAssetReadModule::UnregisterMenuExtensions()
{
	UToolMenus::UnregisterOwner(this);
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FUAssetReadModule, UAssetRead)