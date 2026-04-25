// Copyright Epic Games, Inc. All Rights Reserved.

#include "Core/Exporters/FBlueprintExporter.h"
#include "Core/Utils/FBlueprintGraphUtils.h"

#include "Engine/Blueprint.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/SCS_Node.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "Kismet2/BlueprintEditorUtils.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static FString PinCategoryToString(const FEdGraphPinType& PinType)
{
	return PinType.PinCategory.ToString();
}

static TSharedPtr<FJsonObject> ExportPinTypeAsPropertyType(const FEdGraphPinType& PinType)
{
	TSharedPtr<FJsonObject> TypeObj = MakeShareable(new FJsonObject);
	TypeObj->SetStringField(TEXT("category"), PinType.PinCategory.ToString());
	if (PinType.PinSubCategoryObject.IsValid())
	{
		TypeObj->SetStringField(TEXT("object"), PinType.PinSubCategoryObject->GetPathName());
	}
	return TypeObj;
}

// Export one FBPVariableDescription
static TSharedPtr<FJsonObject> ExportVariable(const FBPVariableDescription& Var)
{
	TSharedPtr<FJsonObject> VarObj = MakeShareable(new FJsonObject);
	VarObj->SetStringField(TEXT("name"), Var.VarName.ToString());
	VarObj->SetStringField(TEXT("type"), Var.VarType.PinCategory.ToString());
	if (Var.VarType.PinSubCategoryObject.IsValid())
	{
		VarObj->SetStringField(TEXT("type_object"), Var.VarType.PinSubCategoryObject->GetPathName());
	}
	VarObj->SetStringField(TEXT("category"), Var.Category.ToString());
	VarObj->SetStringField(TEXT("default_value"), Var.DefaultValue);
	return VarObj;
}

// Collect function parameter pins from a graph's entry node
static void CollectFunctionPins(UEdGraph* Graph,
                                TArray<TSharedPtr<FJsonValue>>& OutInputs,
                                TArray<TSharedPtr<FJsonValue>>& OutOutputs)
{
	for (UEdGraphNode* Node : Graph->Nodes)
	{
		if (!Node) continue;
		const FName NodeClass = Node->GetClass()->GetFName();
		// Entry nodes: K2Node_FunctionEntry / K2Node_FunctionResult
		if (NodeClass == TEXT("K2Node_FunctionEntry"))
		{
			for (UEdGraphPin* Pin : Node->Pins)
			{
				if (!Pin || Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec) continue;
				if (Pin->Direction == EGPD_Output)
				{
					TSharedPtr<FJsonObject> PObj = MakeShareable(new FJsonObject);
					PObj->SetStringField(TEXT("name"), Pin->PinName.ToString());
					PObj->SetStringField(TEXT("type"), Pin->PinType.PinCategory.ToString());
					OutInputs.Add(MakeShareable(new FJsonValueObject(PObj)));
				}
			}
		}
		else if (NodeClass == TEXT("K2Node_FunctionResult"))
		{
			for (UEdGraphPin* Pin : Node->Pins)
			{
				if (!Pin || Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec) continue;
				if (Pin->Direction == EGPD_Input)
				{
					TSharedPtr<FJsonObject> PObj = MakeShareable(new FJsonObject);
					PObj->SetStringField(TEXT("name"), Pin->PinName.ToString());
					PObj->SetStringField(TEXT("type"), Pin->PinType.PinCategory.ToString());
					OutOutputs.Add(MakeShareable(new FJsonValueObject(PObj)));
				}
			}
		}
	}
}

// Recursive component tree builder
static TSharedPtr<FJsonObject> ExportSCSNode(USCS_Node* SCSNode)
{
	if (!SCSNode) return nullptr;

	TSharedPtr<FJsonObject> CompObj = MakeShareable(new FJsonObject);
	CompObj->SetStringField(TEXT("name"), SCSNode->GetVariableName().ToString());

	FString CompTypeName;
	if (SCSNode->ComponentClass)
	{
		CompTypeName = SCSNode->ComponentClass->GetName();
	}
	else if (SCSNode->ComponentTemplate)
	{
		CompTypeName = SCSNode->ComponentTemplate->GetClass()->GetName();
	}
	CompObj->SetStringField(TEXT("type"), CompTypeName);

	TArray<TSharedPtr<FJsonValue>> Children;
	for (USCS_Node* Child : SCSNode->GetChildNodes())
	{
		TSharedPtr<FJsonObject> ChildObj = ExportSCSNode(Child);
		if (ChildObj.IsValid())
		{
			Children.Add(MakeShareable(new FJsonValueObject(ChildObj)));
		}
	}
	CompObj->SetArrayField(TEXT("children"), Children);
	return CompObj;
}

// ---------------------------------------------------------------------------
// FBlueprintExporter
// ---------------------------------------------------------------------------

TSharedPtr<FJsonObject> FBlueprintExporter::Export(UObject* Asset)
{
	UBlueprint* Blueprint = Cast<UBlueprint>(Asset);
	if (!Blueprint)
	{
		return nullptr;
	}

	TSharedPtr<FJsonObject> Root = MakeShareable(new FJsonObject);
	Root->SetStringField(TEXT("assetPath"), Blueprint->GetPathName());
	Root->SetStringField(TEXT("assetType"), Blueprint->GetClass()->GetName());

	// -----------------------------------------------------------------------
	// 1. Properties
	// -----------------------------------------------------------------------
	{
		TArray<TSharedPtr<FJsonValue>> PropsArray;
		for (const FBPVariableDescription& Var : Blueprint->NewVariables)
		{
			PropsArray.Add(MakeShareable(new FJsonValueObject(ExportVariable(Var))));
		}
		Root->SetArrayField(TEXT("properties"), PropsArray);
	}

	// -----------------------------------------------------------------------
	// 2. Functions (signatures only) + graphs
	// -----------------------------------------------------------------------
	{
		TArray<TSharedPtr<FJsonValue>> FuncsArray;
		TArray<TSharedPtr<FJsonValue>> GraphsArray;

		// Function graphs
		for (UEdGraph* Graph : Blueprint->FunctionGraphs)
		{
			if (!Graph) continue;

			TArray<TSharedPtr<FJsonValue>> Inputs, Outputs;
			CollectFunctionPins(Graph, Inputs, Outputs);

			TSharedPtr<FJsonObject> FuncObj = MakeShareable(new FJsonObject);
			FuncObj->SetStringField(TEXT("name"), Graph->GetFName().ToString());
			FuncObj->SetStringField(TEXT("access"), TEXT("Public"));
			FuncObj->SetBoolField(TEXT("isOverride"), false);
			FuncObj->SetBoolField(TEXT("isPure"), false);
			FuncObj->SetArrayField(TEXT("inputs"), Inputs);
			FuncObj->SetArrayField(TEXT("outputs"), Outputs);
			FuncsArray.Add(MakeShareable(new FJsonValueObject(FuncObj)));

			GraphsArray.Add(MakeShareable(new FJsonValueObject(
				FBlueprintGraphUtils::ExportGraph(Graph, TEXT("Function")))));
		}

		// Ubergraph (event graph) pages
		for (UEdGraph* Graph : Blueprint->UbergraphPages)
		{
			if (!Graph) continue;
			GraphsArray.Add(MakeShareable(new FJsonValueObject(
				FBlueprintGraphUtils::ExportGraph(Graph, TEXT("Ubergraph")))));
		}

		// Macro graphs
		for (UEdGraph* Graph : Blueprint->MacroGraphs)
		{
			if (!Graph) continue;
			GraphsArray.Add(MakeShareable(new FJsonValueObject(
				FBlueprintGraphUtils::ExportGraph(Graph, TEXT("Macro")))));
		}

		Root->SetArrayField(TEXT("functions"), FuncsArray);
		Root->SetArrayField(TEXT("graphs"), GraphsArray);
	}

	// -----------------------------------------------------------------------
	// 3. Parent class inheritance chain
	// -----------------------------------------------------------------------
	{
		TArray<TSharedPtr<FJsonValue>> ParentsArray;
		UClass* Current = Blueprint->ParentClass;
		while (Current)
		{
			if (Current->ClassGeneratedBy)
			{
				// Blueprint class – store full asset path
				ParentsArray.Add(MakeShareable(
					new FJsonValueString(Current->ClassGeneratedBy->GetPathName())));
				Current = Current->GetSuperClass();
			}
			else
			{
				// Native C++ class – this is the chain terminus
				ParentsArray.Add(MakeShareable(new FJsonValueString(Current->GetName())));
				break;
			}
		}
		TSharedPtr<FJsonObject> InheritanceObj = MakeShareable(new FJsonObject);
		InheritanceObj->SetArrayField(TEXT("parents"), ParentsArray);
		Root->SetObjectField(TEXT("inheritance"), InheritanceObj);
	}

	// -----------------------------------------------------------------------
	// 4. Implemented interfaces
	// -----------------------------------------------------------------------
	{
		TArray<TSharedPtr<FJsonValue>> InterfacesArray;
		for (const FBPInterfaceDescription& Iface : Blueprint->ImplementedInterfaces)
		{
			if (Iface.Interface)
			{
				InterfacesArray.Add(MakeShareable(
					new FJsonValueString(Iface.Interface->GetPathName())));
			}
		}
		Root->SetArrayField(TEXT("interfaces"), InterfacesArray);
	}

	// -----------------------------------------------------------------------
	// 5. Component hierarchy (Actor subclasses only)
	// -----------------------------------------------------------------------
	{
		bool bIsActor = false;
		UClass* Check = Blueprint->ParentClass;
		while (Check)
		{
			if (Check->GetName() == TEXT("Actor"))
			{
				bIsActor = true;
				break;
			}
			Check = Check->GetSuperClass();
		}

		if (bIsActor && Blueprint->SimpleConstructionScript)
		{
			USCS_Node* RootNode = Blueprint->SimpleConstructionScript->GetDefaultSceneRootNode();
			if (RootNode)
			{
				Root->SetObjectField(TEXT("components"), ExportSCSNode(RootNode));
			}
			else
			{
				// Multiple root nodes (no single default scene root)
				TArray<TSharedPtr<FJsonValue>> RootsArray;
				for (USCS_Node* Node : Blueprint->SimpleConstructionScript->GetRootNodes())
				{
					TSharedPtr<FJsonObject> NodeObj = ExportSCSNode(Node);
					if (NodeObj.IsValid())
					{
						RootsArray.Add(MakeShareable(new FJsonValueObject(NodeObj)));
					}
				}
				Root->SetArrayField(TEXT("components"), RootsArray);
			}
		}
	}

	// -----------------------------------------------------------------------
	// 6. Widget tree – handled by FWidgetExporter, so nothing extra here
	//    (FAssetExportCore will call FWidgetExporter for UWidgetBlueprint)
	// -----------------------------------------------------------------------

	return Root;
}
