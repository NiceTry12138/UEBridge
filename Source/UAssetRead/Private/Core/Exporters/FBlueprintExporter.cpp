// Copyright Epic Games, Inc. All Rights Reserved.

#include "Core/Exporters/FBlueprintExporter.h"
#include "Core/Utils/FBlueprintGraphUtils.h"

#include "Engine/Blueprint.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/SCS_Node.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "GameFramework/Actor.h"
#include "Components/SceneComponent.h"
#include "Components/ActorComponent.h"

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

// ---------------------------------------------------------------------------
// Component tree helpers
// ---------------------------------------------------------------------------

// Recursively build a JSON subtree for one SCS node and all its SCS children.
// Also appends a flat entry for every node visited.
static TSharedPtr<FJsonObject> BuildSCSSubtree(
	USCS_Node* Node, TArray<TSharedPtr<FJsonObject>>& OutFlat)
{
	if (!Node) return nullptr;

	FString TypeStr;
	if (Node->ComponentClass)
		TypeStr = Node->ComponentClass->GetName();
	else if (Node->ComponentTemplate)
		TypeStr = Node->ComponentTemplate->GetClass()->GetName();

	// Flat entry
	{
		TSharedPtr<FJsonObject> F = MakeShareable(new FJsonObject);
		F->SetStringField(TEXT("name"),   Node->GetVariableName().ToString());
		F->SetStringField(TEXT("type"),   TypeStr);
		F->SetStringField(TEXT("source"), TEXT("scs"));
		OutFlat.Add(F);
	}

	TArray<TSharedPtr<FJsonValue>> ChildrenJson;
	for (USCS_Node* Child : Node->GetChildNodes())
	{
		TSharedPtr<FJsonObject> ChildObj = BuildSCSSubtree(Child, OutFlat);
		if (ChildObj.IsValid())
			ChildrenJson.Add(MakeShareable(new FJsonValueObject(ChildObj)));
	}

	TSharedPtr<FJsonObject> Obj = MakeShareable(new FJsonObject);
	Obj->SetStringField(TEXT("name"),   Node->GetVariableName().ToString());
	Obj->SetStringField(TEXT("type"),   TypeStr);
	Obj->SetStringField(TEXT("source"), TEXT("scs"));
	Obj->SetArrayField(TEXT("children"), ChildrenJson);
	return Obj;
}

// Recursively build the native scene-component tree from the CDO's attach
// hierarchy, injecting SCS subtrees under whichever native component they
// declare as their parent (SCSByNativeParent key = native component name).
static TSharedPtr<FJsonObject> BuildNativeSceneTree(
	USceneComponent* Comp,
	const TMap<FName, TArray<TSharedPtr<FJsonObject>>>& SCSByNativeParent,
	TArray<TSharedPtr<FJsonObject>>& OutFlat)
{
	if (!Comp) return nullptr;

	// Flat entry
	{
		TSharedPtr<FJsonObject> F = MakeShareable(new FJsonObject);
		F->SetStringField(TEXT("name"),   Comp->GetName());
		F->SetStringField(TEXT("type"),   Comp->GetClass()->GetName());
		F->SetStringField(TEXT("source"), TEXT("native"));
		OutFlat.Add(F);
	}

	TArray<TSharedPtr<FJsonValue>> ChildrenJson;

	// Native children (CDO attach hierarchy)
	for (USceneComponent* Child : Comp->GetAttachChildren())
	{
		if (!Child) continue;
		TSharedPtr<FJsonObject> ChildObj =
			BuildNativeSceneTree(Child, SCSByNativeParent, OutFlat);
		if (ChildObj.IsValid())
			ChildrenJson.Add(MakeShareable(new FJsonValueObject(ChildObj)));
	}

	// SCS children whose ParentComponentOrVariableName matches this component
	const TArray<TSharedPtr<FJsonObject>>* SCSChildren =
		SCSByNativeParent.Find(FName(*Comp->GetName()));
	if (SCSChildren)
	{
		for (const TSharedPtr<FJsonObject>& SC : *SCSChildren)
			ChildrenJson.Add(MakeShareable(new FJsonValueObject(SC)));
	}

	TSharedPtr<FJsonObject> Obj = MakeShareable(new FJsonObject);
	Obj->SetStringField(TEXT("name"),   Comp->GetName());
	Obj->SetStringField(TEXT("type"),   Comp->GetClass()->GetName());
	Obj->SetStringField(TEXT("source"), TEXT("native"));
	Obj->SetArrayField(TEXT("children"), ChildrenJson);
	return Obj;
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
		TArray<TSharedPtr<FJsonValue>> EventsArray;
		for (UEdGraph* Graph : Blueprint->UbergraphPages)
		{
			if (!Graph) continue;
			GraphsArray.Add(MakeShareable(new FJsonValueObject(
				FBlueprintGraphUtils::ExportGraph(Graph, TEXT("Ubergraph")))));

			// Extract event entry nodes as a structured list
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				if (!Node) continue;
				const FName NodeClassName = Node->GetClass()->GetFName();
				if (NodeClassName == TEXT("K2Node_Event")
					|| NodeClassName == TEXT("K2Node_CustomEvent")
					|| NodeClassName == TEXT("K2Node_EnhancedInputAction")
					|| NodeClassName == TEXT("K2Node_ComponentBoundEvent")
					|| NodeClassName == TEXT("K2Node_ActorBoundEvent"))
				{
					TSharedPtr<FJsonObject> EvtObj = MakeShareable(new FJsonObject);
					EvtObj->SetStringField(TEXT("name"),
						Node->GetNodeTitle(ENodeTitleType::ListView).ToString());
					EvtObj->SetStringField(TEXT("type"), NodeClassName.ToString());
					EvtObj->SetStringField(TEXT("graph"), Graph->GetFName().ToString());
					EvtObj->SetStringField(TEXT("node_id"), Node->NodeGuid.ToString(EGuidFormats::Digits));
					EventsArray.Add(MakeShareable(new FJsonValueObject(EvtObj)));
				}
			}
		}

		// Macro graphs
		for (UEdGraph* Graph : Blueprint->MacroGraphs)
		{
			if (!Graph) continue;
			GraphsArray.Add(MakeShareable(new FJsonValueObject(
				FBlueprintGraphUtils::ExportGraph(Graph, TEXT("Macro")))));
		}

		Root->SetArrayField(TEXT("functions"), FuncsArray);
		Root->SetArrayField(TEXT("events"), EventsArray);
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

		// Build a merged component tree: native components (from CDO) + blueprint-
		// added components (from SCS).  The CDO only carries native components;
		// SCS nodes are instantiated at spawn time and are therefore absent from
		// GetAttachChildren() on the CDO.  We combine both sources here.
		if (bIsActor && Blueprint->GeneratedClass)
		{
			AActor* CDO = Cast<AActor>(Blueprint->GeneratedClass->GetDefaultObject(false));
			USimpleConstructionScript* SCS = Blueprint->SimpleConstructionScript;

			TArray<TSharedPtr<FJsonObject>> FlatList;

			// ── Step 1: Walk SCS root nodes, group by native parent name ─────────
			// Each SCS root node declares which native (or DefaultSceneRoot) comp
			// it attaches to via ParentComponentOrVariableName.
			TMap<FName, TArray<TSharedPtr<FJsonObject>>> SCSByNativeParent;
			if (SCS)
			{
				USCS_Node* DefaultRoot = SCS->GetDefaultSceneRootNode();
				for (USCS_Node* RootNode : SCS->GetRootNodes())
				{
					if (!RootNode) continue;

					if (RootNode == DefaultRoot)
					{
						// Synthetic root – its direct SCS children attach under the
						// native root component (keyed by "ROOT").
						for (USCS_Node* Child : RootNode->GetChildNodes())
						{
							TSharedPtr<FJsonObject> Sub = BuildSCSSubtree(Child, FlatList);
							if (Sub.IsValid())
								SCSByNativeParent.FindOrAdd(FName(TEXT("ROOT"))).Add(Sub);
						}
					}
					else
					{
						TSharedPtr<FJsonObject> Sub = BuildSCSSubtree(RootNode, FlatList);
						if (Sub.IsValid())
						{
							FName ParentName = RootNode->ParentComponentOrVariableName;
							if (ParentName.IsNone())
								ParentName = FName(TEXT("ROOT"));
							SCSByNativeParent.FindOrAdd(ParentName).Add(Sub);
						}
					}
				}
			}

			// ── Step 2: Build native tree from CDO, injecting SCS children ────────
			if (CDO)
			{
				USceneComponent* RootComp = CDO->GetRootComponent();
				if (RootComp)
				{
					// If any SCS nodes were bucketed under "ROOT", move them under
					// the actual native root component's name.
					TArray<TSharedPtr<FJsonObject>>* RootBucket =
						SCSByNativeParent.Find(FName(TEXT("ROOT")));
					if (RootBucket)
					{
						SCSByNativeParent.FindOrAdd(FName(*RootComp->GetName()))
							.Append(*RootBucket);
						SCSByNativeParent.Remove(FName(TEXT("ROOT")));
					}

					Root->SetObjectField(TEXT("components"),
						BuildNativeSceneTree(RootComp, SCSByNativeParent, FlatList));
				}

				// Non-scene components (CharacterMovementComponent, etc.)
				TArray<TSharedPtr<FJsonValue>> NonSceneArray;
				for (UActorComponent* Comp : CDO->GetComponents())
				{
					if (!Comp || Cast<USceneComponent>(Comp)) continue;
					TSharedPtr<FJsonObject> CompObj = MakeShareable(new FJsonObject);
					CompObj->SetStringField(TEXT("name"),   Comp->GetName());
					CompObj->SetStringField(TEXT("type"),   Comp->GetClass()->GetName());
					CompObj->SetStringField(TEXT("source"), TEXT("native"));
					NonSceneArray.Add(MakeShareable(new FJsonValueObject(CompObj)));

					// Also add to flat list
					TSharedPtr<FJsonObject> F = MakeShareable(new FJsonObject);
					F->SetStringField(TEXT("name"),   Comp->GetName());
					F->SetStringField(TEXT("type"),   Comp->GetClass()->GetName());
					F->SetStringField(TEXT("source"), TEXT("native"));
					FlatList.Add(F);
				}
				if (NonSceneArray.Num() > 0)
					Root->SetArrayField(TEXT("non_scene_components"), NonSceneArray);
			}

			// ── Step 3: Output flat list ──────────────────────────────────────────
			TArray<TSharedPtr<FJsonValue>> FlatJsonArray;
			for (const TSharedPtr<FJsonObject>& Entry : FlatList)
				FlatJsonArray.Add(MakeShareable(new FJsonValueObject(Entry)));
			Root->SetArrayField(TEXT("components_flat"), FlatJsonArray);
		}
	}

	// -----------------------------------------------------------------------
	// 6. Widget tree – handled by FWidgetExporter, so nothing extra here
	//    (FAssetExportCore will call FWidgetExporter for UWidgetBlueprint)
	// -----------------------------------------------------------------------

	return Root;
}
