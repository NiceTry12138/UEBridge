// Copyright Epic Games, Inc. All Rights Reserved.

#include "Core/Utils/FBlueprintGraphUtils.h"

#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"

// K2Node types for specialized "extra" data
#include "K2Node_CallFunction.h"
#include "K2Node_Variable.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "K2Node_Event.h"
#include "K2Node_ExecutionSequence.h"
#include "K2Node_MacroInstance.h"
#include "K2Node_IfThenElse.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static FString PinDirectionToString(EEdGraphPinDirection Dir)
{
	return (Dir == EGPD_Input) ? TEXT("input") : TEXT("output");
}

static FString ContainerTypeToString(EPinContainerType Container)
{
	switch (Container)
	{
	case EPinContainerType::Array: return TEXT("array");
	case EPinContainerType::Set:   return TEXT("set");
	case EPinContainerType::Map:   return TEXT("map");
	default:                       return TEXT("none");
	}
}

// ---------------------------------------------------------------------------
// FBlueprintGraphUtils
// ---------------------------------------------------------------------------

TSharedPtr<FJsonObject> FBlueprintGraphUtils::BuildPinType(UEdGraphPin* Pin)
{
	TSharedPtr<FJsonObject> TypeObj = MakeShareable(new FJsonObject);
	const FEdGraphPinType& PT = Pin->PinType;

	TypeObj->SetStringField(TEXT("category"), PT.PinCategory.ToString());
	TypeObj->SetStringField(TEXT("sub_category"), PT.PinSubCategory.ToString());

	FString SubCatObject;
	if (PT.PinSubCategoryObject.IsValid())
	{
		SubCatObject = PT.PinSubCategoryObject->GetPathName();
	}
	TypeObj->SetStringField(TEXT("sub_category_object"), SubCatObject);
	TypeObj->SetStringField(TEXT("container"), ContainerTypeToString(PT.ContainerType));
	TypeObj->SetBoolField(TEXT("is_reference"), PT.bIsReference);
	TypeObj->SetBoolField(TEXT("is_const"), PT.bIsConst);
	return TypeObj;
}

TSharedPtr<FJsonObject> FBlueprintGraphUtils::ExportPin(UEdGraphPin* Pin)
{
	TSharedPtr<FJsonObject> PinObj = MakeShareable(new FJsonObject);

	PinObj->SetStringField(TEXT("pin_id"), Pin->PinId.ToString());
	PinObj->SetStringField(TEXT("name"), Pin->PinName.ToString());
	PinObj->SetStringField(TEXT("display_name"), Pin->PinFriendlyName.ToString());
	PinObj->SetStringField(TEXT("direction"), PinDirectionToString(Pin->Direction));
	PinObj->SetObjectField(TEXT("pin_type"), BuildPinType(Pin));
	PinObj->SetStringField(TEXT("default_value"), Pin->DefaultValue);

	// Links: store the connected pin's ID
	TArray<TSharedPtr<FJsonValue>> LinksArray;
	for (UEdGraphPin* LinkedPin : Pin->LinkedTo)
	{
		if (LinkedPin)
		{
			LinksArray.Add(MakeShareable(new FJsonValueString(LinkedPin->PinId.ToString())));
		}
	}
	PinObj->SetArrayField(TEXT("links"), LinksArray);
	return PinObj;
}

TSharedPtr<FJsonObject> FBlueprintGraphUtils::BuildNodeExtra(UEdGraphNode* Node)
{
	TSharedPtr<FJsonObject> ExtraObj = MakeShareable(new FJsonObject);

	if (UK2Node_CallFunction* CallFunc = Cast<UK2Node_CallFunction>(Node))
	{
		ExtraObj->SetStringField(TEXT("function_name"), CallFunc->FunctionReference.GetMemberName().ToString());
		UClass* OwnerClass = CallFunc->FunctionReference.GetMemberParentClass();
		ExtraObj->SetStringField(TEXT("function_owner"), OwnerClass ? OwnerClass->GetPathName() : TEXT(""));
		ExtraObj->SetBoolField(TEXT("is_pure"), CallFunc->bIsPureFunc);
		ExtraObj->SetBoolField(TEXT("is_latent"), CallFunc->IsLatentFunction());
	}
	else if (UK2Node_VariableGet* VarGet = Cast<UK2Node_VariableGet>(Node))
	{
		ExtraObj->SetStringField(TEXT("variable_name"), VarGet->VariableReference.GetMemberName().ToString());
		UClass* VarClass = VarGet->VariableReference.GetMemberParentClass();
		ExtraObj->SetStringField(TEXT("variable_class"), VarClass ? VarClass->GetName() : TEXT("Self"));
	}
	else if (UK2Node_VariableSet* VarSet = Cast<UK2Node_VariableSet>(Node))
	{
		ExtraObj->SetStringField(TEXT("variable_name"), VarSet->VariableReference.GetMemberName().ToString());
		UClass* VarClass = VarSet->VariableReference.GetMemberParentClass();
		ExtraObj->SetStringField(TEXT("variable_class"), VarClass ? VarClass->GetName() : TEXT("Self"));
	}
	else if (UK2Node_Event* EventNode = Cast<UK2Node_Event>(Node))
	{
		if (EventNode->bOverrideFunction)
		{
			ExtraObj->SetStringField(TEXT("event_name"), EventNode->EventReference.GetMemberName().ToString());
			ExtraObj->SetBoolField(TEXT("is_custom_event"), false);
		}
		else
		{
			ExtraObj->SetStringField(TEXT("event_name"), EventNode->CustomFunctionName.ToString());
			ExtraObj->SetBoolField(TEXT("is_custom_event"), true);
		}
	}
	else if (UK2Node_ExecutionSequence* SeqNode = Cast<UK2Node_ExecutionSequence>(Node))
	{
		// Count output exec pins
		int32 OutPinCount = 0;
		for (UEdGraphPin* P : Node->Pins)
		{
			if (P->Direction == EGPD_Output && P->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
			{
				++OutPinCount;
			}
		}
		ExtraObj->SetNumberField(TEXT("pin_count"), OutPinCount);
	}
	else if (UK2Node_MacroInstance* MacroNode = Cast<UK2Node_MacroInstance>(Node))
	{
		if (UEdGraph* MacroGraph = MacroNode->GetMacroGraph())
		{
			ExtraObj->SetStringField(TEXT("macro_name"), MacroGraph->GetName());
		}
	}

	return ExtraObj;
}

TSharedPtr<FJsonObject> FBlueprintGraphUtils::ExportNode(UEdGraphNode* Node)
{
	TSharedPtr<FJsonObject> NodeObj = MakeShareable(new FJsonObject);

	NodeObj->SetStringField(TEXT("node_id"), Node->NodeGuid.ToString());
	NodeObj->SetStringField(TEXT("node_class"), Node->GetClass()->GetName());
	NodeObj->SetStringField(TEXT("title"), Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString());
	NodeObj->SetStringField(TEXT("comment"), Node->NodeComment);

	TArray<TSharedPtr<FJsonValue>> PosArray;
	PosArray.Add(MakeShareable(new FJsonValueNumber(Node->NodePosX)));
	PosArray.Add(MakeShareable(new FJsonValueNumber(Node->NodePosY)));
	NodeObj->SetArrayField(TEXT("pos"), PosArray);

	TArray<TSharedPtr<FJsonValue>> PinsArray;
	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (Pin)
		{
			PinsArray.Add(MakeShareable(new FJsonValueObject(ExportPin(Pin))));
		}
	}
	NodeObj->SetArrayField(TEXT("pins"), PinsArray);
	NodeObj->SetObjectField(TEXT("extra"), BuildNodeExtra(Node));
	return NodeObj;
}

TSharedPtr<FJsonObject> FBlueprintGraphUtils::ExportGraph(UEdGraph* Graph, const FString& GraphType)
{
	TSharedPtr<FJsonObject> GraphObj = MakeShareable(new FJsonObject);
	GraphObj->SetStringField(TEXT("graph_name"), Graph->GetFName().ToString());
	GraphObj->SetStringField(TEXT("graph_type"), GraphType);

	TArray<TSharedPtr<FJsonValue>> NodesArray;
	TArray<TSharedPtr<FJsonValue>> EdgesArray;

	for (UEdGraphNode* Node : Graph->Nodes)
	{
		if (!Node) continue;
		NodesArray.Add(MakeShareable(new FJsonValueObject(ExportNode(Node))));

		// Build flat edge list from each output pin's links
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (!Pin || Pin->Direction != EGPD_Output) continue;
			for (UEdGraphPin* LinkedPin : Pin->LinkedTo)
			{
				if (!LinkedPin) continue;

				TSharedPtr<FJsonObject> EdgeObj = MakeShareable(new FJsonObject);
				EdgeObj->SetStringField(TEXT("from_node"), Node->NodeGuid.ToString());
				EdgeObj->SetStringField(TEXT("from_pin"), Pin->PinId.ToString());
				EdgeObj->SetStringField(TEXT("to_node"), LinkedPin->GetOwningNode()->NodeGuid.ToString());
				EdgeObj->SetStringField(TEXT("to_pin"), LinkedPin->PinId.ToString());
				EdgeObj->SetBoolField(TEXT("is_exec"),
					Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec);
				EdgesArray.Add(MakeShareable(new FJsonValueObject(EdgeObj)));
			}
		}
	}

	GraphObj->SetArrayField(TEXT("nodes"), NodesArray);
	GraphObj->SetArrayField(TEXT("edges"), EdgesArray);
	return GraphObj;
}
