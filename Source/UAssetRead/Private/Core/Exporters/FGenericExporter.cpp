// Copyright Epic Games, Inc. All Rights Reserved.

#include "Core/Exporters/FGenericExporter.h"

#include "JsonObjectConverter.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "UObject/Class.h"
#include "UObject/UnrealType.h"
#include "UObject/PropertyPortFlags.h"

// ─────────────────────────────────────────────────────────────────────────────
// File-scoped helpers
// ─────────────────────────────────────────────────────────────────────────────

static FString Generic_GetFunctionAccess(const UFunction* Func)
{
	if (Func->FunctionFlags & FUNC_Private)   return TEXT("Private");
	if (Func->FunctionFlags & FUNC_Protected) return TEXT("Protected");
	return TEXT("Public");
}

static TArray<TSharedPtr<FJsonValue>> Generic_GetFunctionFlags(const UFunction* Func)
{
	TArray<TSharedPtr<FJsonValue>> Out;
	auto Add = [&](const TCHAR* Name)
	{
		Out.Add(MakeShareable(new FJsonValueString(Name)));
	};

	if (Func->FunctionFlags & FUNC_Static)            Add(TEXT("Static"));
	if (Func->FunctionFlags & FUNC_Exec)              Add(TEXT("Exec"));
	if (Func->FunctionFlags & FUNC_Const)             Add(TEXT("Const"));
	if (Func->FunctionFlags & FUNC_BlueprintCallable) Add(TEXT("BlueprintCallable"));
	if (Func->FunctionFlags & FUNC_BlueprintPure)     Add(TEXT("BlueprintPure"));
	if (Func->FunctionFlags & FUNC_BlueprintEvent)    Add(TEXT("BlueprintEvent"));
	if (Func->FunctionFlags & FUNC_BlueprintAuthorityOnly) Add(TEXT("AuthorityOnly"));
	if (Func->FunctionFlags & FUNC_Net)               Add(TEXT("Net"));
	if (Func->FunctionFlags & FUNC_NetMulticast)      Add(TEXT("NetMulticast"));
	if (Func->FunctionFlags & FUNC_NetServer)         Add(TEXT("Server"));
	if (Func->FunctionFlags & FUNC_NetClient)         Add(TEXT("Client"));
	if (Func->FunctionFlags & FUNC_NetReliable)       Add(TEXT("NetReliable"));

	return Out;
}

// ─────────────────────────────────────────────────────────────────────────────
// FGenericExporter::Export
// ─────────────────────────────────────────────────────────────────────────────

TSharedPtr<FJsonObject> FGenericExporter::Export(UObject* Asset)
{
	TSharedPtr<FJsonObject> Root = MakeShareable(new FJsonObject);
	UClass* Class = Asset->GetClass();

	// ── Basic info ──────────────────────────────────────────────────────────
	Root->SetStringField(TEXT("assetPath"), Asset->GetPathName());
	Root->SetStringField(TEXT("assetType"), Class->GetName());
	Root->SetStringField(TEXT("outerPath"),
		Asset->GetOuter() ? Asset->GetOuter()->GetPathName() : FString());

	// ── Class hierarchy ─────────────────────────────────────────────────────
	{
		TArray<TSharedPtr<FJsonValue>> Hierarchy;
		for (UClass* C = Class; C; C = C->GetSuperClass())
		{
			Hierarchy.Add(MakeShareable(new FJsonValueString(C->GetName())));
		}
		Root->SetArrayField(TEXT("classHierarchy"), Hierarchy);
	}

	// ── Implemented interfaces ───────────────────────────────────────────────
	{
		TArray<TSharedPtr<FJsonValue>> Ifaces;
		for (const FImplementedInterface& Iface : Class->Interfaces)
		{
			if (Iface.Class)
			{
				Ifaces.Add(MakeShareable(new FJsonValueString(Iface.Class->GetName())));
			}
		}
		Root->SetArrayField(TEXT("interfaces"), Ifaces);
	}

	// ── Property values (proper JSON types via FJsonObjectConverter) ─────────
	// CheckFlags=0 / SkipFlags=0 → include ALL UPROPERTY fields regardless of flags.
	{
		TSharedRef<FJsonObject> PropsObj = MakeShared<FJsonObject>();
		FJsonObjectConverter::UStructToJsonObject(Class, Asset, PropsObj, /*CheckFlags*/0, /*SkipFlags*/0);
		Root->SetObjectField(TEXT("properties"), PropsObj);
	}

	// ── Property metadata (type, declaring class, notable flags) ─────────────
	{
		TArray<TSharedPtr<FJsonValue>> PropMetaArray;
		for (TFieldIterator<FProperty> It(Class, EFieldIteratorFlags::IncludeSuper); It; ++It)
		{
			FProperty* Prop = *It;
			TSharedPtr<FJsonObject> Meta = MakeShareable(new FJsonObject);

			Meta->SetStringField(TEXT("name"), Prop->GetName());
			Meta->SetStringField(TEXT("type"), Prop->GetCPPType());
			Meta->SetStringField(TEXT("definedIn"),
				Prop->GetOwnerUObject() ? Prop->GetOwnerUObject()->GetName() : FString());

			// Notable flags – only set when true to keep JSON lean
			if (Prop->HasAnyPropertyFlags(CPF_Edit))
				Meta->SetBoolField(TEXT("editable"),         true);
			if (Prop->HasAnyPropertyFlags(CPF_BlueprintVisible))
				Meta->SetBoolField(TEXT("blueprintVisible"), true);
			if (Prop->HasAnyPropertyFlags(CPF_BlueprintReadOnly))
				Meta->SetBoolField(TEXT("blueprintReadOnly"),true);
			if (Prop->HasAnyPropertyFlags(CPF_Protected))
				Meta->SetBoolField(TEXT("protected"),        true);
			if (Prop->HasAnyPropertyFlags(CPF_NativeAccessSpecifierPrivate))
				Meta->SetBoolField(TEXT("private"),          true);
			if (Prop->HasAnyPropertyFlags(CPF_Net))
				Meta->SetBoolField(TEXT("replicated"),       true);
			if (Prop->HasAnyPropertyFlags(CPF_SaveGame))
				Meta->SetBoolField(TEXT("saveGame"),         true);
			if (Prop->HasAnyPropertyFlags(CPF_Transient))
				Meta->SetBoolField(TEXT("transient"),        true);
			if (Prop->HasAnyPropertyFlags(CPF_Config))
				Meta->SetBoolField(TEXT("config"),           true);

			PropMetaArray.Add(MakeShareable(new FJsonValueObject(Meta)));
		}
		Root->SetArrayField(TEXT("propertyMeta"), PropMetaArray);
	}

	// ── Functions ────────────────────────────────────────────────────────────
	{
		TArray<TSharedPtr<FJsonValue>> FunctionsArray;
		for (TFieldIterator<UFunction> FuncIt(Class, EFieldIteratorFlags::IncludeSuper); FuncIt; ++FuncIt)
		{
			UFunction* Func = *FuncIt;
			TSharedPtr<FJsonObject> FuncObj = MakeShareable(new FJsonObject);

			FuncObj->SetStringField(TEXT("name"),      Func->GetName());
			FuncObj->SetStringField(TEXT("access"),    Generic_GetFunctionAccess(Func));
			FuncObj->SetStringField(TEXT("definedIn"),
				Func->GetOuterUClass() ? Func->GetOuterUClass()->GetName() : FString());
			FuncObj->SetArrayField(TEXT("flags"), Generic_GetFunctionFlags(Func));

			// Parameters (includes return value)
			TArray<TSharedPtr<FJsonValue>> Params;
			for (TFieldIterator<FProperty> ParamIt(Func);
				 ParamIt && ParamIt->HasAnyPropertyFlags(CPF_Parm); ++ParamIt)
			{
				TSharedPtr<FJsonObject> P = MakeShareable(new FJsonObject);
				P->SetStringField(TEXT("name"), ParamIt->GetName());
				P->SetStringField(TEXT("type"), ParamIt->GetCPPType());
				P->SetBoolField(TEXT("isReturn"),
					ParamIt->HasAnyPropertyFlags(CPF_ReturnParm));
				P->SetBoolField(TEXT("isOut"),
					ParamIt->HasAnyPropertyFlags(CPF_OutParm) &&
					!ParamIt->HasAnyPropertyFlags(CPF_ReturnParm));
				Params.Add(MakeShareable(new FJsonValueObject(P)));
			}
			FuncObj->SetArrayField(TEXT("params"), Params);

			FunctionsArray.Add(MakeShareable(new FJsonValueObject(FuncObj)));
		}
		Root->SetArrayField(TEXT("functions"), FunctionsArray);
	}

	return Root;
}
