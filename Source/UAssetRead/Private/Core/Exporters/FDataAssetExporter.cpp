// Copyright Epic Games, Inc. All Rights Reserved.

#include "Core/Exporters/FDataAssetExporter.h"

#include "Engine/DataTable.h"
#include "Engine/DataAsset.h"
#include "UObject/PropertyPortFlags.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

/** Export a single UObject's UPROPERTY fields via reflection. */
static TSharedPtr<FJsonObject> ExportObjectProperties(UObject* Obj)
{
	TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject);
	if (!Obj) return Result;

	UClass* Class = Obj->GetClass();
	for (TFieldIterator<FProperty> It(Class, EFieldIteratorFlags::IncludeSuper); It; ++It)
	{
		FProperty* Prop = *It;
		if (!Prop->HasAnyPropertyFlags(CPF_Edit | CPF_BlueprintVisible))
		{
			continue;
		}

		const void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(Obj);
		FString ExportedValue;
		Prop->ExportTextItem_Direct(ExportedValue, ValuePtr, nullptr, nullptr, PPF_None);
		Result->SetStringField(Prop->GetName(), ExportedValue);
	}
	return Result;
}

/** Export a single DataTable row as a JSON object. */
static TSharedPtr<FJsonObject> ExportDataTableRow(const FName& RowName,
                                                   const uint8* RowData,
                                                   const UScriptStruct* RowStruct)
{
	TSharedPtr<FJsonObject> RowObj = MakeShareable(new FJsonObject);
	RowObj->SetStringField(TEXT("RowName"), RowName.ToString());

	for (TFieldIterator<FProperty> It(RowStruct, EFieldIteratorFlags::IncludeSuper); It; ++It)
	{
		FProperty* Prop = *It;
		const void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(RowData);
		FString ExportedValue;
		Prop->ExportTextItem_Direct(ExportedValue, ValuePtr, nullptr, nullptr, PPF_None);
		RowObj->SetStringField(Prop->GetName(), ExportedValue);
	}
	return RowObj;
}

// ---------------------------------------------------------------------------
// FDataAssetExporter
// ---------------------------------------------------------------------------

TSharedPtr<FJsonObject> FDataAssetExporter::Export(UObject* Asset)
{
	// ---- UDataTable --------------------------------------------------------
	if (UDataTable* DataTable = Cast<UDataTable>(Asset))
	{
		TSharedPtr<FJsonObject> Root = MakeShareable(new FJsonObject);
		Root->SetStringField(TEXT("assetPath"), DataTable->GetPathName());
		Root->SetStringField(TEXT("assetType"), TEXT("DataTable"));

		const UScriptStruct* RowStruct = DataTable->GetRowStruct();
		Root->SetStringField(TEXT("rowStruct"), RowStruct ? RowStruct->GetName() : TEXT(""));

		TArray<TSharedPtr<FJsonValue>> RowsArray;
		const TMap<FName, uint8*>& RowMap = DataTable->GetRowMap();
		for (auto& Pair : RowMap)
		{
			RowsArray.Add(MakeShareable(new FJsonValueObject(
				ExportDataTableRow(Pair.Key, Pair.Value, RowStruct))));
		}
		Root->SetArrayField(TEXT("rows"), RowsArray);
		return Root;
	}

	// ---- UDataAsset --------------------------------------------------------
	if (UDataAsset* DataAsset = Cast<UDataAsset>(Asset))
	{
		TSharedPtr<FJsonObject> Root = MakeShareable(new FJsonObject);
		Root->SetStringField(TEXT("assetPath"), DataAsset->GetPathName());
		Root->SetStringField(TEXT("assetType"), TEXT("DataAsset"));
		Root->SetObjectField(TEXT("properties"), ExportObjectProperties(DataAsset));
		return Root;
	}

	return nullptr;
}
