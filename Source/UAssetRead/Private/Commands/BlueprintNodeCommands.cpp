// Copyright Epic Games, Inc. All Rights Reserved.
// Blueprint Node command routes:
//   POST /add_blueprint_event_node
//   POST /add_blueprint_function_node
//   POST /connect_blueprint_nodes
//   POST /find_blueprint_nodes
//   POST /add_blueprint_var_get_node
//   POST /add_blueprint_var_set_node
//   POST /get_blueprint_node_pins
//   POST /delete_blueprint_node
//   POST /batch_edit_blueprint_nodes

#include "UAssetReadModule.h"

#include "HttpServerRequest.h"
#include "HttpServerResponse.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Async/Async.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UObjectIterator.h"

#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/SCS_Node.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_Event.h"
#include "K2Node_CallFunction.h"
#include "K2Node_Self.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "K2Node_CustomEvent.h"
#include "GameFramework/Actor.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/Character.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Kismet/KismetMathLibrary.h"
#include "Kismet/KismetStringLibrary.h"
#include "Kismet/KismetArrayLibrary.h"
#include "Kismet/GameplayStatics.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "Components/PrimitiveComponent.h"
#include "Components/SceneComponent.h"
#include "Components/SkinnedMeshComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Blueprint/UserWidget.h"
#include "Components/Widget.h"

#include "UAssetReadHelpers.h"

// ---------------------------------------------------------------------------
// File-scoped helper utilities (adapted from ECABlueprintNodeCommands.cpp)
// ---------------------------------------------------------------------------

static UBlueprint* BPNode_LoadBlueprintByPath(const FString& Path)
{
	UBlueprint* BP = LoadObject<UBlueprint>(nullptr, *Path);
	if (!BP)
	{
		FString Trimmed = Path;
		if (Trimmed.EndsWith(TEXT("_C"))) { Trimmed.RemoveFromEnd(TEXT("_C")); }
		BP = LoadObject<UBlueprint>(nullptr, *Trimmed);
	}
	return BP;
}

static UEdGraph* BPNode_FindGraphByName(UBlueprint* Blueprint, const FString& GraphName)
{
	for (UEdGraph* Graph : Blueprint->UbergraphPages)
	{
		if (Graph->GetName() == GraphName ||
			GraphName.Equals(TEXT("EventGraph"), ESearchCase::IgnoreCase))
		{
			return Graph;
		}
	}
	for (UEdGraph* Graph : Blueprint->FunctionGraphs)
	{
		if (Graph->GetName() == GraphName) { return Graph; }
	}
	return nullptr;
}

static UEdGraphNode* BPNode_FindNodeByGuid(UEdGraph* Graph, const FGuid& Guid)
{
	for (UEdGraphNode* Node : Graph->Nodes)
	{
		if (Node && Node->NodeGuid == Guid) { return Node; }
	}
	return nullptr;
}

static void BPNode_EnsureGuid(UEdGraphNode* Node)
{
	if (Node && !Node->NodeGuid.IsValid()) { Node->CreateNewGuid(); }
}

static UEdGraphPin* BPNode_FindPinByFriendlyName(UEdGraphNode* Node, const FString& PinName, EEdGraphPinDirection Dir = EGPD_MAX)
{
	if (!Node) { return nullptr; }
	if (Dir != EGPD_MAX)
	{
		if (UEdGraphPin* P = Node->FindPin(*PinName, Dir)) { return P; }
	}
	if (UEdGraphPin* P = Node->FindPin(*PinName)) { return P; }

	FString Lower = PinName.ToLower();
	if (Lower == TEXT("execute") || Lower == TEXT("exec") || Lower == TEXT("in"))
	{
		for (UEdGraphPin* P : Node->Pins)
		{
			if (P->Direction == EGPD_Input &&
				P->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec &&
				(P->PinName.IsNone() || P->PinName.ToString().IsEmpty()))
			{
				return P;
			}
		}
	}
	else if (Lower == TEXT("then") || Lower == TEXT("out"))
	{
		for (UEdGraphPin* P : Node->Pins)
		{
			if (P->Direction == EGPD_Output &&
				P->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec &&
				(P->PinName.IsNone() || P->PinName.ToString().IsEmpty()))
			{
				return P;
			}
		}
	}
	return nullptr;
}

static TSharedPtr<FJsonObject> BPNode_PinToJson(UEdGraphPin* Pin)
{
	TSharedPtr<FJsonObject> J = MakeShared<FJsonObject>();
	FString ActualName = Pin->PinName.ToString();
	FString DisplayName = ActualName;
	if (DisplayName.IsEmpty() || DisplayName == TEXT("None"))
	{
		if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
			DisplayName = (Pin->Direction == EGPD_Input) ? TEXT("execute") : TEXT("then");
		else
			DisplayName = (Pin->Direction == EGPD_Input) ? TEXT("input") : TEXT("output");
	}
	J->SetStringField(TEXT("name"), DisplayName);
	if (DisplayName != ActualName) { J->SetStringField(TEXT("internal_name"), ActualName); }
	J->SetStringField(TEXT("direction"), Pin->Direction == EGPD_Input ? TEXT("Input") : TEXT("Output"));
	J->SetStringField(TEXT("type"), Pin->PinType.PinCategory.ToString());
	J->SetStringField(TEXT("sub_type"), Pin->PinType.PinSubCategory.ToString());
	if (Pin->PinType.PinSubCategoryObject.IsValid())
		J->SetStringField(TEXT("sub_type_object"), Pin->PinType.PinSubCategoryObject->GetName());
	J->SetBoolField(TEXT("is_connected"), Pin->LinkedTo.Num() > 0);
	J->SetBoolField(TEXT("is_hidden"), Pin->bHidden);
	if (!Pin->DefaultValue.IsEmpty()) { J->SetStringField(TEXT("default_value"), Pin->DefaultValue); }
	if (Pin->DefaultObject)
	{
		J->SetStringField(TEXT("default_object"), Pin->DefaultObject->GetPathName());
	}
	if (Pin->LinkedTo.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> Links;
		for (UEdGraphPin* Linked : Pin->LinkedTo)
		{
			TSharedPtr<FJsonObject> LJ = MakeShared<FJsonObject>();
			LJ->SetStringField(TEXT("node_id"), Linked->GetOwningNode()->NodeGuid.ToString());
			LJ->SetStringField(TEXT("pin_name"), Linked->PinName.ToString());
			Links.Add(MakeShared<FJsonValueObject>(LJ));
		}
		J->SetArrayField(TEXT("connected_to"), Links);
	}
	return J;
}

static TSharedPtr<FJsonObject> BPNode_NodeToJson(UEdGraphNode* Node)
{
	BPNode_EnsureGuid(Node);
	TSharedPtr<FJsonObject> J = MakeShared<FJsonObject>();
	J->SetStringField(TEXT("node_id"), Node->NodeGuid.ToString());
	J->SetStringField(TEXT("node_class"), Node->GetClass()->GetName());
	J->SetStringField(TEXT("node_title"), Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString());
	J->SetNumberField(TEXT("x"), Node->NodePosX);
	J->SetNumberField(TEXT("y"), Node->NodePosY);
	TArray<TSharedPtr<FJsonValue>> Pins;
	for (UEdGraphPin* P : Node->Pins)
	{
		if (!P->bHidden) { Pins.Add(MakeShared<FJsonValueObject>(BPNode_PinToJson(P))); }
	}
	J->SetArrayField(TEXT("pins"), Pins);
	return J;
}

static FVector2D BPNode_AutoPosition(UEdGraph* Graph, UEdGraphNode* ConnectTo = nullptr)
{
	static const int32 SpX = 400, SpY = 150;
	if (!Graph) { return FVector2D(0, 0); }
	if (ConnectTo) { return FVector2D(ConnectTo->NodePosX + SpX, ConnectTo->NodePosY); }
	int32 MaxX = INT_MIN, MaxY = INT_MIN;
	bool bAny = false;
	for (UEdGraphNode* N : Graph->Nodes)
	{
		if (!N) continue;
		MaxX = FMath::Max(MaxX, N->NodePosX);
		MaxY = FMath::Max(MaxY, N->NodePosY);
		bAny = true;
	}
	if (!bAny) { return FVector2D(0, 0); }
	return FVector2D(MaxX + SpX, MaxY);
}

static FVector2D BPNode_GetPosition(const TSharedPtr<FJsonObject>& Json, UEdGraph* Graph = nullptr, UEdGraphNode* ConnectTo = nullptr)
{
	const TSharedPtr<FJsonObject>* Pos;
	if (Json->TryGetObjectField(TEXT("node_position"), Pos))
	{
		return FVector2D((*Pos)->GetNumberField(TEXT("x")), (*Pos)->GetNumberField(TEXT("y")));
	}
	return BPNode_AutoPosition(Graph, ConnectTo);
}

// Parse request body helper
static bool BPNode_ParseBody(const FHttpServerRequest& Req, TSharedPtr<FJsonObject>& OutJson, FString& OutError)
{
	TArray<uint8> Bytes = Req.Body;
	Bytes.Add(0);
	FString Str = UTF8_TO_TCHAR(reinterpret_cast<const char*>(Bytes.GetData()));
	TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(Str);
	if (!FJsonSerializer::Deserialize(R, OutJson) || !OutJson.IsValid())
	{
		OutError = TEXT("Invalid JSON body");
		return false;
	}
	return true;
}

// ---------------------------------------------------------------------------
// POST /add_blueprint_event_node
// Body: {"blueprint_path":"...","event_name":"BeginPlay","node_position":{"x":0,"y":0}}
// ---------------------------------------------------------------------------

bool FUAssetReadModule::HandleAddBlueprintEventNode(
	const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
	TSharedPtr<FJsonObject> Body; FString Err;
	if (!BPNode_ParseBody(Request, Body, Err)) { OnComplete(MakeJsonError(EHttpServerResponseCodes::BadRequest, Err)); return true; }

	FString BlueprintPath, EventName;
	if (!Body->TryGetStringField(TEXT("blueprint_path"), BlueprintPath) || BlueprintPath.IsEmpty())
	{ OnComplete(MakeJsonError(EHttpServerResponseCodes::BadRequest, TEXT("Missing: blueprint_path"))); return true; }
	if (!Body->TryGetStringField(TEXT("event_name"), EventName) || EventName.IsEmpty())
	{ OnComplete(MakeJsonError(EHttpServerResponseCodes::BadRequest, TEXT("Missing: event_name"))); return true; }

	TSharedRef<bool> AliveRef = bAlive;
	AsyncTask(ENamedThreads::GameThread, [AliveRef, BlueprintPath, EventName, Body, OnComplete]() mutable
	{
		if (!*AliveRef) return;
		UBlueprint* BP = BPNode_LoadBlueprintByPath(BlueprintPath);
		if (!BP) { OnComplete(MakeJsonError(EHttpServerResponseCodes::NoContent, FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintPath))); return; }

		UEdGraph* Graph = BPNode_FindGraphByName(BP, TEXT("EventGraph"));
		if (!Graph) { OnComplete(MakeJsonError(EHttpServerResponseCodes::ServerError, TEXT("EventGraph not found"))); return; }

		FVector2D Pos = BPNode_GetPosition(Body, Graph);

		UFunction* OverrideFunc = BP->ParentClass ? BP->ParentClass->FindFunctionByName(*EventName) : nullptr;
		UK2Node_Event* EventNode = nullptr;

		if (OverrideFunc)
		{
			UK2Node_Event* N = NewObject<UK2Node_Event>(Graph);
			N->EventReference.SetFromField<UFunction>(OverrideFunc, false);
			N->bOverrideFunction = true;
			EventNode = N;
		}
		else
		{
			UK2Node_CustomEvent* N = NewObject<UK2Node_CustomEvent>(Graph);
			N->CustomFunctionName = FName(*EventName);
			EventNode = N;
		}

		EventNode->NodePosX = Pos.X; EventNode->NodePosY = Pos.Y;
		Graph->AddNode(EventNode, false, false);
		EventNode->AllocateDefaultPins();
		BPNode_EnsureGuid(EventNode);
		EventNode->ReconstructNode();
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("node_id"), EventNode->NodeGuid.ToString());
		Result->SetStringField(TEXT("event_name"), EventName);
		Result->SetObjectField(TEXT("node"), BPNode_NodeToJson(EventNode));

		FString Out; TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&Out);
		FJsonSerializer::Serialize(Result.ToSharedRef(), W);
		OnComplete(MakeJsonResponse(Out));
	});
	return true;
}

// ---------------------------------------------------------------------------
// POST /add_blueprint_function_node
// Body: {"blueprint_path":"...","function_name":"PrintString","target_class":"KismetSystemLibrary","graph_name":"EventGraph"}
// ---------------------------------------------------------------------------

bool FUAssetReadModule::HandleAddBlueprintFunctionNode(
	const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
	TSharedPtr<FJsonObject> Body; FString Err;
	if (!BPNode_ParseBody(Request, Body, Err)) { OnComplete(MakeJsonError(EHttpServerResponseCodes::BadRequest, Err)); return true; }

	FString BlueprintPath, FunctionName;
	if (!Body->TryGetStringField(TEXT("blueprint_path"), BlueprintPath) || BlueprintPath.IsEmpty())
	{ OnComplete(MakeJsonError(EHttpServerResponseCodes::BadRequest, TEXT("Missing: blueprint_path"))); return true; }
	if (!Body->TryGetStringField(TEXT("function_name"), FunctionName) || FunctionName.IsEmpty())
	{ OnComplete(MakeJsonError(EHttpServerResponseCodes::BadRequest, TEXT("Missing: function_name"))); return true; }

	FString TargetClass, GraphName = TEXT("EventGraph");
	Body->TryGetStringField(TEXT("target_class"), TargetClass);
	Body->TryGetStringField(TEXT("graph_name"), GraphName);

	TSharedRef<bool> AliveRef = bAlive;
	AsyncTask(ENamedThreads::GameThread, [AliveRef, BlueprintPath, FunctionName, TargetClass, GraphName, Body, OnComplete]() mutable
	{
		if (!*AliveRef) return;
		UBlueprint* BP = BPNode_LoadBlueprintByPath(BlueprintPath);
		if (!BP) { OnComplete(MakeJsonError(EHttpServerResponseCodes::NoContent, FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintPath))); return; }

		UEdGraph* Graph = BPNode_FindGraphByName(BP, GraphName);
		if (!Graph) { OnComplete(MakeJsonError(EHttpServerResponseCodes::BadRequest, FString::Printf(TEXT("Graph not found: %s"), *GraphName))); return; }

		FVector2D Pos = BPNode_GetPosition(Body, Graph);

		// Check if it's a Blueprint-defined function
		for (UEdGraph* FuncGraph : BP->FunctionGraphs)
		{
			if (FuncGraph && FuncGraph->GetFName() == FName(*FunctionName))
			{
				UK2Node_CallFunction* CallNode = NewObject<UK2Node_CallFunction>(Graph);
				CallNode->FunctionReference.SetSelfMember(FName(*FunctionName));
				CallNode->NodePosX = Pos.X; CallNode->NodePosY = Pos.Y;
				Graph->AddNode(CallNode, false, false);
				CallNode->AllocateDefaultPins();
				BPNode_EnsureGuid(CallNode);
				CallNode->ReconstructNode();
				FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);

				TSharedPtr<FJsonObject> R = MakeShared<FJsonObject>();
				R->SetStringField(TEXT("node_id"), CallNode->NodeGuid.ToString());
				R->SetStringField(TEXT("function_name"), FunctionName);
				R->SetStringField(TEXT("function_class"), TEXT("Self"));
				R->SetObjectField(TEXT("node"), BPNode_NodeToJson(CallNode));
				FString Out; TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&Out);
				FJsonSerializer::Serialize(R.ToSharedRef(), W);
				OnComplete(MakeJsonResponse(Out));
				return;
			}
		}

		// Search for the function in known classes
		TArray<UClass*> SearchClasses;
		if (!TargetClass.IsEmpty())
		{
			UClass* Specified = FindObject<UClass>(nullptr, *TargetClass);
			if (!Specified) { Specified = LoadObject<UClass>(nullptr, *TargetClass); }
			static const TCHAR* Prefixes[] = {
				TEXT("/Script/Engine."), TEXT("/Script/CoreUObject."),
				TEXT("/Script/UMG."), TEXT("/Script/AIModule."), nullptr };
			for (int32 i = 0; !Specified && Prefixes[i]; ++i)
			{
				Specified = FindObject<UClass>(nullptr, *(FString(Prefixes[i]) + TargetClass));
				if (!Specified) Specified = LoadObject<UClass>(nullptr, *(FString(Prefixes[i]) + TargetClass));
			}
			if (Specified) SearchClasses.Add(Specified);
		}
		SearchClasses.Add(UKismetSystemLibrary::StaticClass());
		SearchClasses.Add(UKismetMathLibrary::StaticClass());
		SearchClasses.Add(UKismetStringLibrary::StaticClass());
		SearchClasses.Add(UKismetArrayLibrary::StaticClass());
		SearchClasses.Add(UGameplayStatics::StaticClass());
		SearchClasses.Add(AActor::StaticClass());
		SearchClasses.Add(APawn::StaticClass());
		SearchClasses.Add(ACharacter::StaticClass());
		SearchClasses.Add(USceneComponent::StaticClass());
		SearchClasses.Add(UPrimitiveComponent::StaticClass());
		if (BP->ParentClass) SearchClasses.Add(BP->ParentClass);

		UFunction* Func = nullptr;
		for (UClass* C : SearchClasses)
		{
			if (!C) continue;
			Func = C->FindFunctionByName(*FunctionName);
			if (!Func) Func = C->FindFunctionByName(*FString::Printf(TEXT("K2_%s"), *FunctionName));
			if (Func) break;
		}

		if (!Func)
		{
			for (TObjectIterator<UClass> It; It; ++It)
			{
				if ((*It)->IsChildOf(UBlueprintFunctionLibrary::StaticClass()) &&
					!(*It)->HasAnyClassFlags(CLASS_Abstract))
				{
					Func = (*It)->FindFunctionByName(*FunctionName);
					if (!Func) Func = (*It)->FindFunctionByName(*FString::Printf(TEXT("K2_%s"), *FunctionName));
					if (Func) break;
				}
			}
		}

		if (!Func)
		{
			OnComplete(MakeJsonError(EHttpServerResponseCodes::NotFound,
				FString::Printf(TEXT("Function not found: %s"), *FunctionName)));
			return;
		}

		UK2Node_CallFunction* CallNode = NewObject<UK2Node_CallFunction>(Graph);
		CallNode->SetFromFunction(Func);
		CallNode->NodePosX = Pos.X; CallNode->NodePosY = Pos.Y;
		Graph->AddNode(CallNode, false, false);
		CallNode->AllocateDefaultPins();
		BPNode_EnsureGuid(CallNode);
		CallNode->ReconstructNode();
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);

		TSharedPtr<FJsonObject> R = MakeShared<FJsonObject>();
		R->SetStringField(TEXT("node_id"), CallNode->NodeGuid.ToString());
		R->SetStringField(TEXT("function_name"), FunctionName);
		R->SetStringField(TEXT("function_class"), Func->GetOwnerClass()->GetName());
		R->SetObjectField(TEXT("node"), BPNode_NodeToJson(CallNode));
		FString Out; TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&Out);
		FJsonSerializer::Serialize(R.ToSharedRef(), W);
		OnComplete(MakeJsonResponse(Out));
	});
	return true;
}

// ---------------------------------------------------------------------------
// POST /connect_blueprint_nodes
// Body: {"blueprint_path":"...","source_node_id":"GUID","source_pin":"then","target_node_id":"GUID","target_pin":"execute","graph_name":"EventGraph"}
// ---------------------------------------------------------------------------

bool FUAssetReadModule::HandleConnectBlueprintNodes(
	const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
	TSharedPtr<FJsonObject> Body; FString Err;
	if (!BPNode_ParseBody(Request, Body, Err)) { OnComplete(MakeJsonError(EHttpServerResponseCodes::BadRequest, Err)); return true; }

	FString BlueprintPath, SrcNodeId, SrcPin, TgtNodeId, TgtPin;
	FString GraphName = TEXT("EventGraph");
	if (!Body->TryGetStringField(TEXT("blueprint_path"), BlueprintPath) || BlueprintPath.IsEmpty() ||
		!Body->TryGetStringField(TEXT("source_node_id"), SrcNodeId) ||
		!Body->TryGetStringField(TEXT("source_pin"), SrcPin) ||
		!Body->TryGetStringField(TEXT("target_node_id"), TgtNodeId) ||
		!Body->TryGetStringField(TEXT("target_pin"), TgtPin))
	{
		OnComplete(MakeJsonError(EHttpServerResponseCodes::BadRequest,
			TEXT("Missing required fields: blueprint_path, source_node_id, source_pin, target_node_id, target_pin")));
		return true;
	}
	Body->TryGetStringField(TEXT("graph_name"), GraphName);

	TSharedRef<bool> AliveRef = bAlive;
	AsyncTask(ENamedThreads::GameThread, [AliveRef, BlueprintPath, SrcNodeId, SrcPin, TgtNodeId, TgtPin, GraphName, OnComplete]() mutable
	{
		if (!*AliveRef) return;
		UBlueprint* BP = BPNode_LoadBlueprintByPath(BlueprintPath);
		if (!BP) { OnComplete(MakeJsonError(EHttpServerResponseCodes::NoContent, FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintPath))); return; }

		UEdGraph* Graph = BPNode_FindGraphByName(BP, GraphName);
		if (!Graph) { OnComplete(MakeJsonError(EHttpServerResponseCodes::BadRequest, FString::Printf(TEXT("Graph not found: %s"), *GraphName))); return; }

		FGuid SrcGuid, TgtGuid;
		if (!FGuid::Parse(SrcNodeId, SrcGuid)) { OnComplete(MakeJsonError(EHttpServerResponseCodes::BadRequest, FString::Printf(TEXT("Invalid source GUID: %s"), *SrcNodeId))); return; }
		if (!FGuid::Parse(TgtNodeId, TgtGuid)) { OnComplete(MakeJsonError(EHttpServerResponseCodes::BadRequest, FString::Printf(TEXT("Invalid target GUID: %s"), *TgtNodeId))); return; }

		UEdGraphNode* SrcNode = BPNode_FindNodeByGuid(Graph, SrcGuid);
		UEdGraphNode* TgtNode = BPNode_FindNodeByGuid(Graph, TgtGuid);
		if (!SrcNode) { OnComplete(MakeJsonError(EHttpServerResponseCodes::NotFound, FString::Printf(TEXT("Source node not found: %s"), *SrcNodeId))); return; }
		if (!TgtNode) { OnComplete(MakeJsonError(EHttpServerResponseCodes::NotFound, FString::Printf(TEXT("Target node not found: %s"), *TgtNodeId))); return; }

		UEdGraphPin* SrcPinPtr = BPNode_FindPinByFriendlyName(SrcNode, SrcPin, EGPD_Output);
		UEdGraphPin* TgtPinPtr = BPNode_FindPinByFriendlyName(TgtNode, TgtPin, EGPD_Input);
		if (!SrcPinPtr) { OnComplete(MakeJsonError(EHttpServerResponseCodes::NotFound, FString::Printf(TEXT("Source pin not found: %s"), *SrcPin))); return; }
		if (!TgtPinPtr) { OnComplete(MakeJsonError(EHttpServerResponseCodes::NotFound, FString::Printf(TEXT("Target pin not found: %s"), *TgtPin))); return; }

		SrcPinPtr->MakeLinkTo(TgtPinPtr);
		if (!SrcPinPtr->LinkedTo.Contains(TgtPinPtr))
		{
			OnComplete(MakeJsonError(EHttpServerResponseCodes::ServerError, TEXT("Failed to connect pins — types may be incompatible")));
			return;
		}

		SrcNode->PinConnectionListChanged(SrcPinPtr);
		TgtNode->PinConnectionListChanged(TgtPinPtr);
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);

		TSharedPtr<FJsonObject> R = MakeShared<FJsonObject>();
		R->SetBoolField(TEXT("connected"), true);
		R->SetStringField(TEXT("source_node"), SrcNodeId);
		R->SetStringField(TEXT("source_pin"), SrcPin);
		R->SetStringField(TEXT("target_node"), TgtNodeId);
		R->SetStringField(TEXT("target_pin"), TgtPin);
		FString Out; TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&Out);
		FJsonSerializer::Serialize(R.ToSharedRef(), W);
		OnComplete(MakeJsonResponse(Out));
	});
	return true;
}

// ---------------------------------------------------------------------------
// POST /find_blueprint_nodes
// Body: {"blueprint_path":"...","node_class":"K2Node_Event","node_title":"BeginPlay","graph_name":"EventGraph"}
// ---------------------------------------------------------------------------

bool FUAssetReadModule::HandleFindBlueprintNodes(
	const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
	TSharedPtr<FJsonObject> Body; FString Err;
	if (!BPNode_ParseBody(Request, Body, Err)) { OnComplete(MakeJsonError(EHttpServerResponseCodes::BadRequest, Err)); return true; }

	FString BlueprintPath, GraphName = TEXT("EventGraph"), NodeClass, NodeTitle;
	if (!Body->TryGetStringField(TEXT("blueprint_path"), BlueprintPath) || BlueprintPath.IsEmpty())
	{ OnComplete(MakeJsonError(EHttpServerResponseCodes::BadRequest, TEXT("Missing: blueprint_path"))); return true; }
	Body->TryGetStringField(TEXT("graph_name"), GraphName);
	Body->TryGetStringField(TEXT("node_class"), NodeClass);
	Body->TryGetStringField(TEXT("node_title"), NodeTitle);

	TSharedRef<bool> AliveRef = bAlive;
	AsyncTask(ENamedThreads::GameThread, [AliveRef, BlueprintPath, GraphName, NodeClass, NodeTitle, OnComplete]() mutable
	{
		if (!*AliveRef) return;
		UBlueprint* BP = BPNode_LoadBlueprintByPath(BlueprintPath);
		if (!BP) { OnComplete(MakeJsonError(EHttpServerResponseCodes::NoContent, FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintPath))); return; }

		UEdGraph* Graph = BPNode_FindGraphByName(BP, GraphName);
		if (!Graph) { OnComplete(MakeJsonError(EHttpServerResponseCodes::BadRequest, FString::Printf(TEXT("Graph not found: %s"), *GraphName))); return; }

		TArray<TSharedPtr<FJsonValue>> NodesArr;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (!Node) continue;
			bool bMatch = true;
			if (!NodeClass.IsEmpty() && !Node->GetClass()->GetName().Contains(NodeClass)) bMatch = false;
			if (!NodeTitle.IsEmpty() && bMatch)
			{
				if (!Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString().Contains(NodeTitle)) bMatch = false;
			}
			if (bMatch) { NodesArr.Add(MakeShared<FJsonValueObject>(BPNode_NodeToJson(Node))); }
		}

		TSharedPtr<FJsonObject> R = MakeShared<FJsonObject>();
		R->SetArrayField(TEXT("nodes"), NodesArr);
		R->SetNumberField(TEXT("count"), NodesArr.Num());
		FString Out; TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&Out);
		FJsonSerializer::Serialize(R.ToSharedRef(), W);
		OnComplete(MakeJsonResponse(Out));
	});
	return true;
}

// ---------------------------------------------------------------------------
// POST /add_blueprint_var_get_node
// Body: {"blueprint_path":"...","variable_name":"MyVar","graph_name":"EventGraph","node_position":{"x":0,"y":0}}
// ---------------------------------------------------------------------------

bool FUAssetReadModule::HandleAddBlueprintVarGetNode(
	const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
	TSharedPtr<FJsonObject> Body; FString Err;
	if (!BPNode_ParseBody(Request, Body, Err)) { OnComplete(MakeJsonError(EHttpServerResponseCodes::BadRequest, Err)); return true; }

	FString BlueprintPath, VariableName, GraphName = TEXT("EventGraph");
	if (!Body->TryGetStringField(TEXT("blueprint_path"), BlueprintPath) || BlueprintPath.IsEmpty())
	{ OnComplete(MakeJsonError(EHttpServerResponseCodes::BadRequest, TEXT("Missing: blueprint_path"))); return true; }
	if (!Body->TryGetStringField(TEXT("variable_name"), VariableName) || VariableName.IsEmpty())
	{ OnComplete(MakeJsonError(EHttpServerResponseCodes::BadRequest, TEXT("Missing: variable_name"))); return true; }
	Body->TryGetStringField(TEXT("graph_name"), GraphName);

	TSharedRef<bool> AliveRef = bAlive;
	AsyncTask(ENamedThreads::GameThread, [AliveRef, BlueprintPath, VariableName, GraphName, Body, OnComplete]() mutable
	{
		if (!*AliveRef) return;
		UBlueprint* BP = BPNode_LoadBlueprintByPath(BlueprintPath);
		if (!BP) { OnComplete(MakeJsonError(EHttpServerResponseCodes::NoContent, FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintPath))); return; }

		UEdGraph* Graph = BPNode_FindGraphByName(BP, GraphName);
		if (!Graph) { OnComplete(MakeJsonError(EHttpServerResponseCodes::BadRequest, FString::Printf(TEXT("Graph not found: %s"), *GraphName))); return; }

		FVector2D Pos = BPNode_GetPosition(Body, Graph);

		UK2Node_VariableGet* GetNode = NewObject<UK2Node_VariableGet>(Graph);
		GetNode->VariableReference.SetSelfMember(FName(*VariableName));
		GetNode->NodePosX = Pos.X; GetNode->NodePosY = Pos.Y;
		Graph->AddNode(GetNode, false, false);
		GetNode->AllocateDefaultPins();
		BPNode_EnsureGuid(GetNode);
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);

		TSharedPtr<FJsonObject> R = MakeShared<FJsonObject>();
		R->SetStringField(TEXT("node_id"), GetNode->NodeGuid.ToString());
		R->SetStringField(TEXT("variable_name"), VariableName);
		R->SetObjectField(TEXT("node"), BPNode_NodeToJson(GetNode));
		FString Out; TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&Out);
		FJsonSerializer::Serialize(R.ToSharedRef(), W);
		OnComplete(MakeJsonResponse(Out));
	});
	return true;
}

// ---------------------------------------------------------------------------
// POST /add_blueprint_var_set_node
// Body: {"blueprint_path":"...","variable_name":"MyVar","graph_name":"EventGraph","node_position":{"x":0,"y":0}}
// ---------------------------------------------------------------------------

bool FUAssetReadModule::HandleAddBlueprintVarSetNode(
	const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
	TSharedPtr<FJsonObject> Body; FString Err;
	if (!BPNode_ParseBody(Request, Body, Err)) { OnComplete(MakeJsonError(EHttpServerResponseCodes::BadRequest, Err)); return true; }

	FString BlueprintPath, VariableName, GraphName = TEXT("EventGraph");
	if (!Body->TryGetStringField(TEXT("blueprint_path"), BlueprintPath) || BlueprintPath.IsEmpty())
	{ OnComplete(MakeJsonError(EHttpServerResponseCodes::BadRequest, TEXT("Missing: blueprint_path"))); return true; }
	if (!Body->TryGetStringField(TEXT("variable_name"), VariableName) || VariableName.IsEmpty())
	{ OnComplete(MakeJsonError(EHttpServerResponseCodes::BadRequest, TEXT("Missing: variable_name"))); return true; }
	Body->TryGetStringField(TEXT("graph_name"), GraphName);

	TSharedRef<bool> AliveRef = bAlive;
	AsyncTask(ENamedThreads::GameThread, [AliveRef, BlueprintPath, VariableName, GraphName, Body, OnComplete]() mutable
	{
		if (!*AliveRef) return;
		UBlueprint* BP = BPNode_LoadBlueprintByPath(BlueprintPath);
		if (!BP) { OnComplete(MakeJsonError(EHttpServerResponseCodes::NoContent, FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintPath))); return; }

		UEdGraph* Graph = BPNode_FindGraphByName(BP, GraphName);
		if (!Graph) { OnComplete(MakeJsonError(EHttpServerResponseCodes::BadRequest, FString::Printf(TEXT("Graph not found: %s"), *GraphName))); return; }

		FVector2D Pos = BPNode_GetPosition(Body, Graph);

		UK2Node_VariableSet* SetNode = NewObject<UK2Node_VariableSet>(Graph);
		SetNode->VariableReference.SetSelfMember(FName(*VariableName));
		SetNode->NodePosX = Pos.X; SetNode->NodePosY = Pos.Y;
		Graph->AddNode(SetNode, false, false);
		SetNode->AllocateDefaultPins();
		BPNode_EnsureGuid(SetNode);
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);

		TSharedPtr<FJsonObject> R = MakeShared<FJsonObject>();
		R->SetStringField(TEXT("node_id"), SetNode->NodeGuid.ToString());
		R->SetStringField(TEXT("variable_name"), VariableName);
		R->SetObjectField(TEXT("node"), BPNode_NodeToJson(SetNode));
		FString Out; TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&Out);
		FJsonSerializer::Serialize(R.ToSharedRef(), W);
		OnComplete(MakeJsonResponse(Out));
	});
	return true;
}

// ---------------------------------------------------------------------------
// POST /get_blueprint_node_pins
// Body: {"blueprint_path":"...","node_id":"GUID","graph_name":"EventGraph","include_hidden":false}
// ---------------------------------------------------------------------------

bool FUAssetReadModule::HandleGetBlueprintNodePins(
	const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
	TSharedPtr<FJsonObject> Body; FString Err;
	if (!BPNode_ParseBody(Request, Body, Err)) { OnComplete(MakeJsonError(EHttpServerResponseCodes::BadRequest, Err)); return true; }

	FString BlueprintPath, NodeId, GraphName = TEXT("EventGraph");
	bool bIncludeHidden = false;
	if (!Body->TryGetStringField(TEXT("blueprint_path"), BlueprintPath) || BlueprintPath.IsEmpty())
	{ OnComplete(MakeJsonError(EHttpServerResponseCodes::BadRequest, TEXT("Missing: blueprint_path"))); return true; }
	if (!Body->TryGetStringField(TEXT("node_id"), NodeId) || NodeId.IsEmpty())
	{ OnComplete(MakeJsonError(EHttpServerResponseCodes::BadRequest, TEXT("Missing: node_id"))); return true; }
	Body->TryGetStringField(TEXT("graph_name"), GraphName);
	Body->TryGetBoolField(TEXT("include_hidden"), bIncludeHidden);

	TSharedRef<bool> AliveRef = bAlive;
	AsyncTask(ENamedThreads::GameThread, [AliveRef, BlueprintPath, NodeId, GraphName, bIncludeHidden, OnComplete]() mutable
	{
		if (!*AliveRef) return;
		UBlueprint* BP = BPNode_LoadBlueprintByPath(BlueprintPath);
		if (!BP) { OnComplete(MakeJsonError(EHttpServerResponseCodes::NoContent, FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintPath))); return; }

		UEdGraph* Graph = BPNode_FindGraphByName(BP, GraphName);
		if (!Graph) { OnComplete(MakeJsonError(EHttpServerResponseCodes::BadRequest, FString::Printf(TEXT("Graph not found: %s"), *GraphName))); return; }

		FGuid Guid;
		if (!FGuid::Parse(NodeId, Guid)) { OnComplete(MakeJsonError(EHttpServerResponseCodes::BadRequest, FString::Printf(TEXT("Invalid GUID: %s"), *NodeId))); return; }

		UEdGraphNode* Node = BPNode_FindNodeByGuid(Graph, Guid);
		if (!Node) { OnComplete(MakeJsonError(EHttpServerResponseCodes::NotFound, FString::Printf(TEXT("Node not found: %s"), *NodeId))); return; }

		TArray<TSharedPtr<FJsonValue>> InputPins, OutputPins;
		for (UEdGraphPin* P : Node->Pins)
		{
			if (!P || (!bIncludeHidden && P->bHidden)) continue;
			if (P->Direction == EGPD_Input) InputPins.Add(MakeShared<FJsonValueObject>(BPNode_PinToJson(P)));
			else OutputPins.Add(MakeShared<FJsonValueObject>(BPNode_PinToJson(P)));
		}

		TSharedPtr<FJsonObject> R = MakeShared<FJsonObject>();
		R->SetStringField(TEXT("node_id"), NodeId);
		R->SetStringField(TEXT("node_class"), Node->GetClass()->GetName());
		R->SetStringField(TEXT("node_title"), Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString());
		R->SetArrayField(TEXT("input_pins"), InputPins);
		R->SetArrayField(TEXT("output_pins"), OutputPins);
		R->SetNumberField(TEXT("input_count"), InputPins.Num());
		R->SetNumberField(TEXT("output_count"), OutputPins.Num());
		FString Out; TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&Out);
		FJsonSerializer::Serialize(R.ToSharedRef(), W);
		OnComplete(MakeJsonResponse(Out));
	});
	return true;
}

// ---------------------------------------------------------------------------
// POST /delete_blueprint_node
// Body: {"blueprint_path":"...","node_id":"GUID","graph_name":"EventGraph"}
// ---------------------------------------------------------------------------

bool FUAssetReadModule::HandleDeleteBlueprintNode(
	const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
	TSharedPtr<FJsonObject> Body; FString Err;
	if (!BPNode_ParseBody(Request, Body, Err)) { OnComplete(MakeJsonError(EHttpServerResponseCodes::BadRequest, Err)); return true; }

	FString BlueprintPath, NodeId, GraphName = TEXT("EventGraph");
	if (!Body->TryGetStringField(TEXT("blueprint_path"), BlueprintPath) || BlueprintPath.IsEmpty())
	{ OnComplete(MakeJsonError(EHttpServerResponseCodes::BadRequest, TEXT("Missing: blueprint_path"))); return true; }
	if (!Body->TryGetStringField(TEXT("node_id"), NodeId) || NodeId.IsEmpty())
	{ OnComplete(MakeJsonError(EHttpServerResponseCodes::BadRequest, TEXT("Missing: node_id"))); return true; }
	Body->TryGetStringField(TEXT("graph_name"), GraphName);

	TSharedRef<bool> AliveRef = bAlive;
	AsyncTask(ENamedThreads::GameThread, [AliveRef, BlueprintPath, NodeId, GraphName, OnComplete]() mutable
	{
		if (!*AliveRef) return;
		UBlueprint* BP = BPNode_LoadBlueprintByPath(BlueprintPath);
		if (!BP) { OnComplete(MakeJsonError(EHttpServerResponseCodes::NoContent, FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintPath))); return; }

		UEdGraph* Graph = BPNode_FindGraphByName(BP, GraphName);
		if (!Graph) { OnComplete(MakeJsonError(EHttpServerResponseCodes::BadRequest, FString::Printf(TEXT("Graph not found: %s"), *GraphName))); return; }

		FGuid Guid;
		if (!FGuid::Parse(NodeId, Guid)) { OnComplete(MakeJsonError(EHttpServerResponseCodes::BadRequest, FString::Printf(TEXT("Invalid GUID: %s"), *NodeId))); return; }

		UEdGraphNode* Node = BPNode_FindNodeByGuid(Graph, Guid);
		if (!Node) { OnComplete(MakeJsonError(EHttpServerResponseCodes::NotFound, FString::Printf(TEXT("Node not found: %s"), *NodeId))); return; }

		FString Title = Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString();
		FString Class = Node->GetClass()->GetName();
		Graph->RemoveNode(Node);
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);

		TSharedPtr<FJsonObject> R = MakeShared<FJsonObject>();
		R->SetStringField(TEXT("deleted_node_id"), NodeId);
		R->SetStringField(TEXT("deleted_node_title"), Title);
		R->SetStringField(TEXT("deleted_node_class"), Class);
		FString Out; TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&Out);
		FJsonSerializer::Serialize(R.ToSharedRef(), W);
		OnComplete(MakeJsonResponse(Out));
	});
	return true;
}

// ---------------------------------------------------------------------------
// POST /batch_edit_blueprint_nodes
// Body: {
//   "blueprint_path":"...",
//   "graph_name":"EventGraph",
//   "nodes":[
//     {"type":"event","event_name":"BeginPlay","temp_id":"n1"},
//     {"type":"function","function_name":"PrintString","temp_id":"n2"},
//     {"type":"variable_get","variable_name":"MyVar","temp_id":"n3"},
//     {"type":"variable_set","variable_name":"MyVar","temp_id":"n4"},
//     ...
//   ],
//   "connections":[
//     {"source_temp_id":"n1","source_pin":"then","target_temp_id":"n2","target_pin":"execute"}
//   ]
// }
// ---------------------------------------------------------------------------

bool FUAssetReadModule::HandleBatchEditBlueprintNodes(
	const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
	TSharedPtr<FJsonObject> Body; FString Err;
	if (!BPNode_ParseBody(Request, Body, Err)) { OnComplete(MakeJsonError(EHttpServerResponseCodes::BadRequest, Err)); return true; }

	FString BlueprintPath, GraphName = TEXT("EventGraph");
	if (!Body->TryGetStringField(TEXT("blueprint_path"), BlueprintPath) || BlueprintPath.IsEmpty())
	{ OnComplete(MakeJsonError(EHttpServerResponseCodes::BadRequest, TEXT("Missing: blueprint_path"))); return true; }
	Body->TryGetStringField(TEXT("graph_name"), GraphName);

	const TArray<TSharedPtr<FJsonValue>>* NodesJson = nullptr;
	Body->TryGetArrayField(TEXT("nodes"), NodesJson);
	const TArray<TSharedPtr<FJsonValue>>* ConnectionsJson = nullptr;
	Body->TryGetArrayField(TEXT("connections"), ConnectionsJson);

	TSharedRef<bool> AliveRef = bAlive;
	AsyncTask(ENamedThreads::GameThread, [AliveRef, BlueprintPath, GraphName, Body, OnComplete]() mutable
	{
		if (!*AliveRef) return;
		UBlueprint* BP = BPNode_LoadBlueprintByPath(BlueprintPath);
		if (!BP) { OnComplete(MakeJsonError(EHttpServerResponseCodes::NoContent, FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintPath))); return; }

		UEdGraph* Graph = BPNode_FindGraphByName(BP, GraphName);
		if (!Graph) { OnComplete(MakeJsonError(EHttpServerResponseCodes::BadRequest, FString::Printf(TEXT("Graph not found: %s"), *GraphName))); return; }

		const TArray<TSharedPtr<FJsonValue>>* NodesArr = nullptr;
		const TArray<TSharedPtr<FJsonValue>>* ConnsArr = nullptr;
		Body->TryGetArrayField(TEXT("nodes"), NodesArr);
		Body->TryGetArrayField(TEXT("connections"), ConnsArr);

		TMap<FString, UEdGraphNode*> TempIdToNode;
		TArray<TSharedPtr<FJsonValue>> CreatedNodes;
		TArray<FString> Errors;

		// Create nodes
		if (NodesArr)
		{
			for (const TSharedPtr<FJsonValue>& NodeVal : *NodesArr)
			{
				const TSharedPtr<FJsonObject>& NJ = NodeVal->AsObject();
				if (!NJ.IsValid()) continue;

				FString Type, TempId;
				NJ->TryGetStringField(TEXT("type"), Type);
				NJ->TryGetStringField(TEXT("temp_id"), TempId);
				FVector2D Pos = BPNode_GetPosition(NJ, Graph);

				UEdGraphNode* NewNode = nullptr;
				FString CreateErr;

				if (Type.Equals(TEXT("event"), ESearchCase::IgnoreCase) ||
					Type.Equals(TEXT("custom_event"), ESearchCase::IgnoreCase))
				{
					FString EventName;
					NJ->TryGetStringField(TEXT("event_name"), EventName);
					UFunction* OverrideFunc = BP->ParentClass ?
						BP->ParentClass->FindFunctionByName(*EventName) : nullptr;
					if (OverrideFunc)
					{
						UK2Node_Event* N = NewObject<UK2Node_Event>(Graph);
						N->EventReference.SetFromField<UFunction>(OverrideFunc, false);
						N->bOverrideFunction = true;
						NewNode = N;
					}
					else
					{
						UK2Node_CustomEvent* N = NewObject<UK2Node_CustomEvent>(Graph);
						N->CustomFunctionName = FName(*EventName);
						NewNode = N;
					}
				}
				else if (Type.Equals(TEXT("function"), ESearchCase::IgnoreCase))
				{
					FString FuncName, TargetClass;
					NJ->TryGetStringField(TEXT("function_name"), FuncName);
					NJ->TryGetStringField(TEXT("target_class"), TargetClass);

					// Check BP functions first
					bool bBPFunc = false;
					for (UEdGraph* FG : BP->FunctionGraphs)
					{
						if (FG && FG->GetFName() == FName(*FuncName))
						{
							UK2Node_CallFunction* N = NewObject<UK2Node_CallFunction>(Graph);
							N->FunctionReference.SetSelfMember(FName(*FuncName));
							NewNode = N; bBPFunc = true; break;
						}
					}
					if (!bBPFunc)
					{
						TArray<UClass*> SC;
						SC.Add(UKismetSystemLibrary::StaticClass());
						SC.Add(UKismetMathLibrary::StaticClass());
						SC.Add(UGameplayStatics::StaticClass());
						SC.Add(AActor::StaticClass());
						if (BP->ParentClass) SC.Add(BP->ParentClass);
						UFunction* Func = nullptr;
						for (UClass* C : SC)
						{
							if (!C) continue;
							Func = C->FindFunctionByName(*FuncName);
							if (!Func) Func = C->FindFunctionByName(*FString::Printf(TEXT("K2_%s"), *FuncName));
							if (Func) break;
						}
						if (!Func)
						{
							for (TObjectIterator<UClass> It; It; ++It)
							{
								if ((*It)->IsChildOf(UBlueprintFunctionLibrary::StaticClass()) && !(*It)->HasAnyClassFlags(CLASS_Abstract))
								{
									Func = (*It)->FindFunctionByName(*FuncName);
									if (Func) break;
								}
							}
						}
						if (Func)
						{
							UK2Node_CallFunction* N = NewObject<UK2Node_CallFunction>(Graph);
							N->SetFromFunction(Func);
							NewNode = N;
						}
						else { CreateErr = FString::Printf(TEXT("Function not found: %s"), *FuncName); }
					}
				}
				else if (Type.Equals(TEXT("variable_get"), ESearchCase::IgnoreCase))
				{
					FString VarName; NJ->TryGetStringField(TEXT("variable_name"), VarName);
					UK2Node_VariableGet* N = NewObject<UK2Node_VariableGet>(Graph);
					N->VariableReference.SetSelfMember(FName(*VarName));
					NewNode = N;
				}
				else if (Type.Equals(TEXT("variable_set"), ESearchCase::IgnoreCase))
				{
					FString VarName; NJ->TryGetStringField(TEXT("variable_name"), VarName);
					UK2Node_VariableSet* N = NewObject<UK2Node_VariableSet>(Graph);
					N->VariableReference.SetSelfMember(FName(*VarName));
					NewNode = N;
				}
				else if (Type.Equals(TEXT("self"), ESearchCase::IgnoreCase))
				{
					NewNode = NewObject<UK2Node_Self>(Graph);
				}
				else
				{
					CreateErr = FString::Printf(TEXT("Unknown node type: %s"), *Type);
				}

				if (!CreateErr.IsEmpty())
				{
					Errors.Add(FString::Printf(TEXT("Node '%s': %s"), *TempId, *CreateErr));
					continue;
				}

				if (NewNode)
				{
					NewNode->NodePosX = Pos.X; NewNode->NodePosY = Pos.Y;
					Graph->AddNode(NewNode, false, false);
					NewNode->AllocateDefaultPins();
					BPNode_EnsureGuid(NewNode);
					NewNode->ReconstructNode();

					if (!TempId.IsEmpty()) { TempIdToNode.Add(TempId, NewNode); }

					TSharedPtr<FJsonObject> Created = MakeShared<FJsonObject>();
					Created->SetStringField(TEXT("temp_id"), TempId);
					Created->SetStringField(TEXT("node_id"), NewNode->NodeGuid.ToString());
					Created->SetStringField(TEXT("node_class"), NewNode->GetClass()->GetName());
					CreatedNodes.Add(MakeShared<FJsonValueObject>(Created));
				}
			}
		}

		// Make connections using temp_ids or GUIDs
		TArray<TSharedPtr<FJsonValue>> MadeConnections;
		if (ConnsArr)
		{
			for (const TSharedPtr<FJsonValue>& ConnVal : *ConnsArr)
			{
				const TSharedPtr<FJsonObject>& CJ = ConnVal->AsObject();
				if (!CJ.IsValid()) continue;

				FString SrcRef, SrcPin, TgtRef, TgtPin;
				CJ->TryGetStringField(TEXT("source_temp_id"), SrcRef);
				if (SrcRef.IsEmpty()) CJ->TryGetStringField(TEXT("source_node_id"), SrcRef);
				CJ->TryGetStringField(TEXT("source_pin"), SrcPin);
				CJ->TryGetStringField(TEXT("target_temp_id"), TgtRef);
				if (TgtRef.IsEmpty()) CJ->TryGetStringField(TEXT("target_node_id"), TgtRef);
				CJ->TryGetStringField(TEXT("target_pin"), TgtPin);

				auto ResolveNode = [&](const FString& Ref) -> UEdGraphNode*
				{
					if (UEdGraphNode* const* Found = TempIdToNode.Find(Ref)) { return *Found; }
					FGuid G; if (FGuid::Parse(Ref, G)) { return BPNode_FindNodeByGuid(Graph, G); }
					return nullptr;
				};

				UEdGraphNode* SrcNode = ResolveNode(SrcRef);
				UEdGraphNode* TgtNode = ResolveNode(TgtRef);
				if (!SrcNode || !TgtNode)
				{
					Errors.Add(FString::Printf(TEXT("Connection: node not found (src=%s, tgt=%s)"), *SrcRef, *TgtRef));
					continue;
				}

				UEdGraphPin* SP = BPNode_FindPinByFriendlyName(SrcNode, SrcPin, EGPD_Output);
				UEdGraphPin* TP = BPNode_FindPinByFriendlyName(TgtNode, TgtPin, EGPD_Input);
				if (!SP || !TP)
				{
					Errors.Add(FString::Printf(TEXT("Connection: pin not found (src_pin=%s, tgt_pin=%s)"), *SrcPin, *TgtPin));
					continue;
				}

				SP->MakeLinkTo(TP);
				SrcNode->PinConnectionListChanged(SP);
				TgtNode->PinConnectionListChanged(TP);

				TSharedPtr<FJsonObject> Conn = MakeShared<FJsonObject>();
				Conn->SetStringField(TEXT("source_node"), SrcNode->NodeGuid.ToString());
				Conn->SetStringField(TEXT("source_pin"), SrcPin);
				Conn->SetStringField(TEXT("target_node"), TgtNode->NodeGuid.ToString());
				Conn->SetStringField(TEXT("target_pin"), TgtPin);
				MadeConnections.Add(MakeShared<FJsonValueObject>(Conn));
			}
		}

		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);

		TSharedPtr<FJsonObject> R = MakeShared<FJsonObject>();
		R->SetArrayField(TEXT("created_nodes"), CreatedNodes);
		R->SetArrayField(TEXT("connections"), MadeConnections);
		R->SetNumberField(TEXT("node_count"), CreatedNodes.Num());
		R->SetNumberField(TEXT("connection_count"), MadeConnections.Num());
		if (Errors.Num() > 0)
		{
			TArray<TSharedPtr<FJsonValue>> ErrArr;
			for (const FString& E : Errors) { ErrArr.Add(MakeShared<FJsonValueString>(E)); }
			R->SetArrayField(TEXT("errors"), ErrArr);
		}
		FString Out; TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&Out);
		FJsonSerializer::Serialize(R.ToSharedRef(), W);
		OnComplete(MakeJsonResponse(Out));
	});
	return true;
}
