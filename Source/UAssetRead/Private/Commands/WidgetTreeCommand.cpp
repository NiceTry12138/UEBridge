// Copyright Epic Games, Inc. All Rights Reserved.
// Route: POST /dump_widget_tree
// Body: {"path":"/Game/UI/WBP_Foo","include_slot_properties":true}

#include "UAssetReadModule.h"

#include "HttpServerRequest.h"
#include "HttpServerResponse.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Async/Async.h"
#include "UObject/UObjectGlobals.h"

#include "Blueprint/WidgetTree.h"
#include "Components/Widget.h"
#include "Components/PanelWidget.h"
#include "Components/ContentWidget.h"
#include "WidgetBlueprint.h"
#include "Engine/Blueprint.h"
#include "UObject/Class.h"

#include "UAssetReadHelpers.h"

// ---------------------------------------------------------------------------
// Helper: recursive widget dump
// ---------------------------------------------------------------------------

static TSharedPtr<FJsonObject> DumpWidgetRecursive(UWidget* Widget, bool bIncludeSlotProps)
{
	if (!Widget) return nullptr;

	TSharedPtr<FJsonObject> WidgetObj = MakeShared<FJsonObject>();
	WidgetObj->SetStringField(TEXT("name"), Widget->GetName());
	WidgetObj->SetStringField(TEXT("class"), Widget->GetClass()->GetName());
	WidgetObj->SetBoolField(TEXT("is_variable"), Widget->bIsVariable);

	// Visibility
	ESlateVisibility Vis = Widget->GetVisibility();
	switch (Vis)
	{
	case ESlateVisibility::Visible:              WidgetObj->SetStringField(TEXT("visibility"), TEXT("Visible")); break;
	case ESlateVisibility::Collapsed:            WidgetObj->SetStringField(TEXT("visibility"), TEXT("Collapsed")); break;
	case ESlateVisibility::Hidden:               WidgetObj->SetStringField(TEXT("visibility"), TEXT("Hidden")); break;
	case ESlateVisibility::HitTestInvisible:     WidgetObj->SetStringField(TEXT("visibility"), TEXT("HitTestInvisible")); break;
	case ESlateVisibility::SelfHitTestInvisible: WidgetObj->SetStringField(TEXT("visibility"), TEXT("SelfHitTestInvisible")); break;
	default: break;
	}

	// Slot properties via reflection
	if (bIncludeSlotProps && Widget->Slot)
	{
		TSharedPtr<FJsonObject> SlotObj = MakeShared<FJsonObject>();
		SlotObj->SetStringField(TEXT("slot_class"), Widget->Slot->GetClass()->GetName());

		UObject* SlotAsObj = Widget->Slot;
		for (TFieldIterator<FProperty> PropIt(SlotAsObj->GetClass()); PropIt; ++PropIt)
		{
			FProperty* Property = *PropIt;
			if (!Property->HasAnyPropertyFlags(CPF_Edit)) continue;

			FString StringValue;
			void* ValuePtr = Property->ContainerPtrToValuePtr<void>(SlotAsObj);
			Property->ExportTextItem_Direct(StringValue, ValuePtr, nullptr, SlotAsObj, PPF_None);
			if (!StringValue.IsEmpty())
			{
				SlotObj->SetStringField(Property->GetName(), StringValue);
			}
		}
		WidgetObj->SetObjectField(TEXT("slot"), SlotObj);
	}

	// Recurse children
	if (UPanelWidget* Panel = Cast<UPanelWidget>(Widget))
	{
		TArray<TSharedPtr<FJsonValue>> ChildrenArray;
		for (int32 i = 0; i < Panel->GetChildrenCount(); i++)
		{
			TSharedPtr<FJsonObject> ChildObj = DumpWidgetRecursive(Panel->GetChildAt(i), bIncludeSlotProps);
			if (ChildObj)
			{
				ChildrenArray.Add(MakeShared<FJsonValueObject>(ChildObj));
			}
		}
		if (ChildrenArray.Num() > 0)
		{
			WidgetObj->SetArrayField(TEXT("children"), ChildrenArray);
		}
	}
	else if (UContentWidget* Content = Cast<UContentWidget>(Widget))
	{
		UWidget* Child = Content->GetContent();
		if (Child)
		{
			TSharedPtr<FJsonObject> ChildObj = DumpWidgetRecursive(Child, bIncludeSlotProps);
			if (ChildObj)
			{
				TArray<TSharedPtr<FJsonValue>> ChildrenArray;
				ChildrenArray.Add(MakeShared<FJsonValueObject>(ChildObj));
				WidgetObj->SetArrayField(TEXT("children"), ChildrenArray);
			}
		}
	}

	return WidgetObj;
}

// ---------------------------------------------------------------------------
// Route handler
// ---------------------------------------------------------------------------

bool FUAssetReadModule::HandleDumpWidgetTree(
	const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
	TArray<uint8> BodyBytes = Request.Body;
	BodyBytes.Add(0);
	FString BodyStr = UTF8_TO_TCHAR(reinterpret_cast<const char*>(BodyBytes.GetData()));

	TSharedPtr<FJsonObject> BodyJson;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(BodyStr);
	if (!FJsonSerializer::Deserialize(Reader, BodyJson) || !BodyJson.IsValid())
	{
		OnComplete(MakeJsonError(EHttpServerResponseCodes::BadRequest, TEXT("Invalid JSON body")));
		return true;
	}

	FString AssetPath;
	if (!BodyJson->TryGetStringField(TEXT("path"), AssetPath) || AssetPath.IsEmpty())
	{
		OnComplete(MakeJsonError(EHttpServerResponseCodes::BadRequest, TEXT("Missing field: path")));
		return true;
	}

	bool bIncludeSlotProps = true;
	BodyJson->TryGetBoolField(TEXT("include_slot_properties"), bIncludeSlotProps);

	TSharedRef<bool> AliveRef = bAlive;

	AsyncTask(ENamedThreads::GameThread,
		[AliveRef, AssetPath = MoveTemp(AssetPath), bIncludeSlotProps, OnComplete]() mutable
		{
			if (!*AliveRef) { return; }

			UWidgetBlueprint* WidgetBP = LoadObject<UWidgetBlueprint>(nullptr, *AssetPath);
			if (!WidgetBP)
			{
				OnComplete(MakeJsonError(EHttpServerResponseCodes::NoContent,
					FString::Printf(TEXT("Failed to load Widget Blueprint: %s"), *AssetPath)));
				return;
			}

			UWidgetTree* WidgetTree = WidgetBP->WidgetTree;
			if (!WidgetTree)
			{
				OnComplete(MakeJsonError(EHttpServerResponseCodes::ServerError,
					TEXT("Widget Blueprint has no widget tree")));
				return;
			}

			TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetStringField(TEXT("widget_path"), AssetPath);
			Result->SetStringField(TEXT("widget_name"), WidgetBP->GetName());

			if (WidgetTree->RootWidget)
			{
				Result->SetObjectField(TEXT("tree"), DumpWidgetRecursive(WidgetTree->RootWidget, bIncludeSlotProps));
			}

			int32 TotalWidgets = 0;
			WidgetTree->ForEachWidget([&TotalWidgets](UWidget*) { TotalWidgets++; });
			Result->SetNumberField(TEXT("total_widgets"), TotalWidgets);

			// Variables from the Blueprint
			TArray<TSharedPtr<FJsonValue>> VarsArray;
			for (const FBPVariableDescription& Var : WidgetBP->NewVariables)
			{
				TSharedPtr<FJsonObject> VarObj = MakeShared<FJsonObject>();
				VarObj->SetStringField(TEXT("name"), Var.VarName.ToString());
				VarObj->SetStringField(TEXT("type"), Var.VarType.PinCategory.ToString());
				if (Var.VarType.PinSubCategoryObject.IsValid())
				{
					VarObj->SetStringField(TEXT("sub_type"), Var.VarType.PinSubCategoryObject->GetName());
				}
				VarsArray.Add(MakeShared<FJsonValueObject>(VarObj));
			}
			Result->SetArrayField(TEXT("variables"), VarsArray);

			FString OutStr;
			TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutStr);
			FJsonSerializer::Serialize(Result.ToSharedRef(), Writer);

			OnComplete(MakeJsonResponse(OutStr));
		});

	return true;
}
