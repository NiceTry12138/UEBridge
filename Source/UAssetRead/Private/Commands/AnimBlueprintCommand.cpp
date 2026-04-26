// Copyright Epic Games, Inc. All Rights Reserved.
// Route: POST /dump_animation_blueprint
// Body: {"path":"/Game/Characters/ABP_Foo"}

#include "UAssetReadModule.h"

#include "HttpServerRequest.h"
#include "HttpServerResponse.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Async/Async.h"
#include "UObject/UObjectGlobals.h"

#include "Animation/AnimBlueprint.h"
#include "Engine/Blueprint.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"

#include "UAssetReadHelpers.h"

// ---------------------------------------------------------------------------
// Route handler
// ---------------------------------------------------------------------------

bool FUAssetReadModule::HandleDumpAnimBlueprint(
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

	TSharedRef<bool> AliveRef = bAlive;

	AsyncTask(ENamedThreads::GameThread,
		[AliveRef, AssetPath = MoveTemp(AssetPath), OnComplete]() mutable
		{
			if (!*AliveRef) { return; }

			UAnimBlueprint* AnimBP = LoadObject<UAnimBlueprint>(nullptr, *AssetPath);
			if (!AnimBP)
			{
				OnComplete(MakeJsonError(EHttpServerResponseCodes::NoContent,
					FString::Printf(TEXT("Failed to load Animation Blueprint: %s"), *AssetPath)));
				return;
			}

			TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetStringField(TEXT("anim_bp_path"), AssetPath);
			Result->SetStringField(TEXT("anim_bp_name"), AnimBP->GetName());
			Result->SetStringField(TEXT("parent_class"),
				AnimBP->ParentClass ? AnimBP->ParentClass->GetName() : TEXT("None"));

			if (AnimBP->TargetSkeleton)
			{
				Result->SetStringField(TEXT("target_skeleton"), AnimBP->TargetSkeleton->GetPathName());
			}

			// Compilation status
			switch (AnimBP->Status)
			{
			case BS_Dirty:              Result->SetStringField(TEXT("compilation_status"), TEXT("dirty")); break;
			case BS_Error:              Result->SetStringField(TEXT("compilation_status"), TEXT("error")); break;
			case BS_UpToDate:           Result->SetStringField(TEXT("compilation_status"), TEXT("up_to_date")); break;
			case BS_UpToDateWithWarnings: Result->SetStringField(TEXT("compilation_status"), TEXT("warnings")); break;
			default:                    Result->SetStringField(TEXT("compilation_status"), TEXT("unknown")); break;
			}

			// Variables
			TArray<TSharedPtr<FJsonValue>> VarsArray;
			for (const FBPVariableDescription& Var : AnimBP->NewVariables)
			{
				TSharedPtr<FJsonObject> VarObj = MakeShared<FJsonObject>();
				VarObj->SetStringField(TEXT("name"), Var.VarName.ToString());
				VarObj->SetStringField(TEXT("type"), Var.VarType.PinCategory.ToString());
				if (Var.VarType.PinSubCategoryObject.IsValid())
				{
					VarObj->SetStringField(TEXT("sub_type"), Var.VarType.PinSubCategoryObject->GetName());
				}
				if (!Var.DefaultValue.IsEmpty())
				{
					VarObj->SetStringField(TEXT("default_value"), Var.DefaultValue);
				}
				VarsArray.Add(MakeShared<FJsonValueObject>(VarObj));
			}
			Result->SetArrayField(TEXT("variables"), VarsArray);

			// Helper to serialize a single graph with nodes + connections + sub-graphs
			auto SerializeGraph = [](UEdGraph* Graph, const FString& GraphType) -> TSharedPtr<FJsonObject>
			{
				TSharedPtr<FJsonObject> GraphObj = MakeShared<FJsonObject>();
				GraphObj->SetStringField(TEXT("name"), Graph->GetName());
				GraphObj->SetStringField(TEXT("class"), Graph->GetClass()->GetName());
				if (!GraphType.IsEmpty())
				{
					GraphObj->SetStringField(TEXT("graph_type"), GraphType);
				}

				TArray<TSharedPtr<FJsonValue>> NodesArray;
				for (UEdGraphNode* Node : Graph->Nodes)
				{
					if (!Node) continue;
					TSharedPtr<FJsonObject> NodeObj = MakeShared<FJsonObject>();
					NodeObj->SetStringField(TEXT("node_id"), Node->NodeGuid.ToString());
					NodeObj->SetStringField(TEXT("node_class"), Node->GetClass()->GetName());
					NodeObj->SetStringField(TEXT("node_title"), Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString());
					NodeObj->SetNumberField(TEXT("x"), Node->NodePosX);
					NodeObj->SetNumberField(TEXT("y"), Node->NodePosY);
					if (!Node->NodeComment.IsEmpty())
					{
						NodeObj->SetStringField(TEXT("comment"), Node->NodeComment);
					}

					int32 InputCount = 0, OutputCount = 0;
					for (UEdGraphPin* Pin : Node->Pins)
					{
						if (Pin->bHidden) continue;
						if (Pin->Direction == EGPD_Input) InputCount++;
						else OutputCount++;
					}
					NodeObj->SetNumberField(TEXT("input_pins"), InputCount);
					NodeObj->SetNumberField(TEXT("output_pins"), OutputCount);

					if (Node->GetSubGraphs().Num() > 0)
					{
						TArray<TSharedPtr<FJsonValue>> SubGraphNames;
						for (UEdGraph* SubGraph : Node->GetSubGraphs())
						{
							if (SubGraph)
							{
								SubGraphNames.Add(MakeShared<FJsonValueString>(SubGraph->GetName()));
							}
						}
						NodeObj->SetArrayField(TEXT("sub_graphs"), SubGraphNames);
					}

					NodesArray.Add(MakeShared<FJsonValueObject>(NodeObj));
				}
				GraphObj->SetArrayField(TEXT("nodes"), NodesArray);
				GraphObj->SetNumberField(TEXT("node_count"), NodesArray.Num());

				// Connections
				TArray<TSharedPtr<FJsonValue>> ConnectionsArray;
				TSet<FString> SeenConnections;
				for (UEdGraphNode* Node : Graph->Nodes)
				{
					if (!Node) continue;
					for (UEdGraphPin* Pin : Node->Pins)
					{
						if (Pin->Direction != EGPD_Output) continue;
						for (UEdGraphPin* LinkedPin : Pin->LinkedTo)
						{
							if (!LinkedPin || !LinkedPin->GetOwningNode()) continue;
							FString Key = FString::Printf(TEXT("%s->%s"),
								*Node->NodeGuid.ToString(), *LinkedPin->GetOwningNode()->NodeGuid.ToString());
							if (SeenConnections.Contains(Key)) continue;
							SeenConnections.Add(Key);

							TSharedPtr<FJsonObject> ConnObj = MakeShared<FJsonObject>();
							ConnObj->SetStringField(TEXT("source_node"), Node->NodeGuid.ToString());
							ConnObj->SetStringField(TEXT("source_pin"), Pin->PinName.ToString());
							ConnObj->SetStringField(TEXT("target_node"), LinkedPin->GetOwningNode()->NodeGuid.ToString());
							ConnObj->SetStringField(TEXT("target_pin"), LinkedPin->PinName.ToString());
							ConnectionsArray.Add(MakeShared<FJsonValueObject>(ConnObj));
						}
					}
				}
				GraphObj->SetArrayField(TEXT("connections"), ConnectionsArray);

				return GraphObj;
			};

			// All graphs
			TArray<TSharedPtr<FJsonValue>> GraphsArray;

			for (UEdGraph* Graph : AnimBP->FunctionGraphs)
			{
				if (!Graph) continue;
				TSharedPtr<FJsonObject> GraphObj = SerializeGraph(Graph, TEXT(""));

				// Sub-graphs for state machines
				TArray<TSharedPtr<FJsonValue>> SubGraphsArray;
				for (UEdGraphNode* Node : Graph->Nodes)
				{
					if (!Node) continue;
					for (UEdGraph* SubGraph : Node->GetSubGraphs())
					{
						if (!SubGraph) continue;
						TSharedPtr<FJsonObject> SubObj = SerializeGraph(SubGraph, TEXT(""));
						SubObj->SetStringField(TEXT("parent_node"), Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString());
						SubGraphsArray.Add(MakeShared<FJsonValueObject>(SubObj));
					}
				}
				if (SubGraphsArray.Num() > 0)
				{
					GraphObj->SetArrayField(TEXT("state_machines"), SubGraphsArray);
				}

				GraphsArray.Add(MakeShared<FJsonValueObject>(GraphObj));
			}

			for (UEdGraph* Graph : AnimBP->UbergraphPages)
			{
				if (!Graph) continue;
				TSharedPtr<FJsonObject> GraphObj = SerializeGraph(Graph, TEXT("event_graph"));
				GraphsArray.Add(MakeShared<FJsonValueObject>(GraphObj));
			}

			Result->SetArrayField(TEXT("graphs"), GraphsArray);

			FString OutStr;
			TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutStr);
			FJsonSerializer::Serialize(Result.ToSharedRef(), Writer);

			OnComplete(MakeJsonResponse(OutStr));
		});

	return true;
}
