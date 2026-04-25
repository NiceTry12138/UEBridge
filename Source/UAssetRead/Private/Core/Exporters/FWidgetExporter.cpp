// Copyright Epic Games, Inc. All Rights Reserved.

#include "Core/Exporters/FWidgetExporter.h"
#include "Core/Exporters/FBlueprintExporter.h"  // To merge BP data

#include "WidgetBlueprint.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Widget.h"
#include "Components/PanelWidget.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/TextBlock.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

// ---------------------------------------------------------------------------
// FWidgetExporter
// ---------------------------------------------------------------------------

TSharedPtr<FJsonObject> FWidgetExporter::ExportWidget(UWidget* Widget)
{
	if (!Widget) return nullptr;

	TSharedPtr<FJsonObject> WidgetObj = MakeShareable(new FJsonObject);
	WidgetObj->SetStringField(TEXT("type"), Widget->GetClass()->GetName());
	WidgetObj->SetStringField(TEXT("name"), Widget->GetName());

	// Slot info (position / size / anchors)
	if (UPanelSlot* Slot = Widget->Slot)
	{
		TSharedPtr<FJsonObject> SlotObj = MakeShareable(new FJsonObject);
		if (UCanvasPanelSlot* CanvasSlot = Cast<UCanvasPanelSlot>(Slot))
		{
			const FAnchors& Anchors = CanvasSlot->GetAnchors();
			const FVector2D Position = CanvasSlot->GetPosition();
			const FVector2D Size = CanvasSlot->GetSize();

			SlotObj->SetNumberField(TEXT("posX"), Position.X);
			SlotObj->SetNumberField(TEXT("posY"), Position.Y);
			SlotObj->SetNumberField(TEXT("sizeX"), Size.X);
			SlotObj->SetNumberField(TEXT("sizeY"), Size.Y);
			SlotObj->SetNumberField(TEXT("anchorMinX"), Anchors.Minimum.X);
			SlotObj->SetNumberField(TEXT("anchorMinY"), Anchors.Minimum.Y);
			SlotObj->SetNumberField(TEXT("anchorMaxX"), Anchors.Maximum.X);
			SlotObj->SetNumberField(TEXT("anchorMaxY"), Anchors.Maximum.Y);
		}
		WidgetObj->SetObjectField(TEXT("slot"), SlotObj);
	}

	// Text content for text blocks
	if (UTextBlock* TextBlock = Cast<UTextBlock>(Widget))
	{
		WidgetObj->SetStringField(TEXT("text"), TextBlock->GetText().ToString());
	}

	// Recurse into children
	TArray<TSharedPtr<FJsonValue>> ChildrenArray;
	if (UPanelWidget* Panel = Cast<UPanelWidget>(Widget))
	{
		for (int32 i = 0; i < Panel->GetChildrenCount(); ++i)
		{
			UWidget* Child = Panel->GetChildAt(i);
			TSharedPtr<FJsonObject> ChildObj = ExportWidget(Child);
			if (ChildObj.IsValid())
			{
				ChildrenArray.Add(MakeShareable(new FJsonValueObject(ChildObj)));
			}
		}
	}
	WidgetObj->SetArrayField(TEXT("children"), ChildrenArray);
	return WidgetObj;
}

TSharedPtr<FJsonObject> FWidgetExporter::Export(UObject* Asset)
{
	UWidgetBlueprint* WBP = Cast<UWidgetBlueprint>(Asset);
	if (!WBP) return nullptr;

	// Start with blueprint data (properties, functions, graphs, inheritance, interfaces)
	FBlueprintExporter BPExporter;
	TSharedPtr<FJsonObject> Root = BPExporter.Export(Asset);
	if (!Root.IsValid())
	{
		Root = MakeShareable(new FJsonObject);
	}

	Root->SetStringField(TEXT("assetType"), TEXT("WidgetBlueprint"));

	// Widget tree
	if (WBP->WidgetTree && WBP->WidgetTree->RootWidget)
	{
		Root->SetObjectField(TEXT("widgetTree"),
			ExportWidget(WBP->WidgetTree->RootWidget));
	}

	return Root;
}
