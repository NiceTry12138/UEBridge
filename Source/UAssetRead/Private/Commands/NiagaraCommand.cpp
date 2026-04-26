// Copyright Epic Games, Inc. All Rights Reserved.
// Route: POST /dump_niagara_system
// Body: {"path":"/Game/FX/NS_Foo","include_module_inputs":true}

#include "UAssetReadModule.h"

#include "HttpServerRequest.h"
#include "HttpServerResponse.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Async/Async.h"
#include "UObject/UObjectGlobals.h"

// Niagara
#include "NiagaraSystem.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraScript.h"
#include "NiagaraScriptSource.h"
#include "NiagaraGraph.h"
#include "NiagaraNodeFunctionCall.h"
#include "NiagaraNodeOutput.h"
#include "ViewModels/Stack/NiagaraStackGraphUtilities.h"
#include "NiagaraSpriteRendererProperties.h"
#include "NiagaraMeshRendererProperties.h"
#include "NiagaraRibbonRendererProperties.h"
#include "NiagaraLightRendererProperties.h"
#include "EdGraph/EdGraphPin.h"

#include "UAssetReadHelpers.h"

// ---------------------------------------------------------------------------
// Helper
// ---------------------------------------------------------------------------

static FString ScriptUsageToString(ENiagaraScriptUsage Usage)
{
	switch (Usage)
	{
	case ENiagaraScriptUsage::ParticleSpawnScript:       return TEXT("particle_spawn");
	case ENiagaraScriptUsage::ParticleUpdateScript:      return TEXT("particle_update");
	case ENiagaraScriptUsage::EmitterSpawnScript:        return TEXT("emitter_spawn");
	case ENiagaraScriptUsage::EmitterUpdateScript:       return TEXT("emitter_update");
	case ENiagaraScriptUsage::SystemSpawnScript:         return TEXT("system_spawn");
	case ENiagaraScriptUsage::SystemUpdateScript:        return TEXT("system_update");
	default:                                              return TEXT("unknown");
	}
}

// ---------------------------------------------------------------------------
// Route handler
// ---------------------------------------------------------------------------

bool FUAssetReadModule::HandleDumpNiagaraSystem(
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

	bool bIncludeModuleInputs = true;
	BodyJson->TryGetBoolField(TEXT("include_module_inputs"), bIncludeModuleInputs);

	TSharedRef<bool> AliveRef = bAlive;

	AsyncTask(ENamedThreads::GameThread,
		[AliveRef, AssetPath = MoveTemp(AssetPath), bIncludeModuleInputs, OnComplete]() mutable
		{
			if (!*AliveRef) { return; }

			UNiagaraSystem* NiagaraSystem = LoadObject<UNiagaraSystem>(nullptr, *AssetPath);
			if (!NiagaraSystem)
			{
				OnComplete(MakeJsonError(EHttpServerResponseCodes::NoContent,
					FString::Printf(TEXT("Failed to load Niagara system: %s"), *AssetPath)));
				return;
			}

			TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetStringField(TEXT("system_path"), AssetPath);
			Result->SetStringField(TEXT("system_name"), NiagaraSystem->GetName());

			// System-level user parameters
			{
				const FNiagaraUserRedirectionParameterStore& ExposedParams = NiagaraSystem->GetExposedParameters();
				TArray<TSharedPtr<FJsonValue>> ParamsArray;
				for (const FNiagaraVariableWithOffset& Var : ExposedParams.ReadParameterVariables())
				{
					TSharedPtr<FJsonObject> ParamObj = MakeShared<FJsonObject>();
					ParamObj->SetStringField(TEXT("name"), Var.GetName().ToString());
					ParamObj->SetStringField(TEXT("type"), Var.GetType().GetName());
					ParamsArray.Add(MakeShared<FJsonValueObject>(ParamObj));
				}
				Result->SetArrayField(TEXT("user_parameters"), ParamsArray);
			}

			// Emitters
			TArray<TSharedPtr<FJsonValue>> EmittersArray;
			const TArray<FNiagaraEmitterHandle>& Handles = NiagaraSystem->GetEmitterHandles();

			for (int32 EmitterIdx = 0; EmitterIdx < Handles.Num(); EmitterIdx++)
			{
				const FNiagaraEmitterHandle& Handle = Handles[EmitterIdx];
				TSharedPtr<FJsonObject> EmitterObj = MakeShared<FJsonObject>();

				EmitterObj->SetStringField(TEXT("name"), Handle.GetName().ToString());
				EmitterObj->SetStringField(TEXT("unique_name"), Handle.GetUniqueInstanceName());
				EmitterObj->SetBoolField(TEXT("enabled"), Handle.GetIsEnabled());
				EmitterObj->SetNumberField(TEXT("index"), EmitterIdx);

				FVersionedNiagaraEmitterData* EmitterData = Handle.GetEmitterData();
				if (!EmitterData)
				{
					EmittersArray.Add(MakeShared<FJsonValueObject>(EmitterObj));
					continue;
				}

				// Sim target
				EmitterObj->SetStringField(TEXT("sim_target"),
					EmitterData->SimTarget == ENiagaraSimTarget::CPUSim ? TEXT("cpu") : TEXT("gpu"));

				// Renderers
				TArray<TSharedPtr<FJsonValue>> RenderersArray;
				for (UNiagaraRendererProperties* Renderer : EmitterData->GetRenderers())
				{
					if (!Renderer) continue;
					TSharedPtr<FJsonObject> RendObj = MakeShared<FJsonObject>();
					RendObj->SetStringField(TEXT("class"), Renderer->GetClass()->GetName());
					RendObj->SetBoolField(TEXT("enabled"), Renderer->GetIsEnabled());

					if (UNiagaraSpriteRendererProperties* Sprite = Cast<UNiagaraSpriteRendererProperties>(Renderer))
					{
						RendObj->SetStringField(TEXT("type"), TEXT("sprite"));
						if (Sprite->Material)
						{
							RendObj->SetStringField(TEXT("material"), Sprite->Material->GetPathName());
						}
					}
					else if (UNiagaraMeshRendererProperties* Mesh = Cast<UNiagaraMeshRendererProperties>(Renderer))
					{
						RendObj->SetStringField(TEXT("type"), TEXT("mesh"));
						if (Mesh->Meshes.Num() > 0 && Mesh->Meshes[0].Mesh)
						{
							RendObj->SetStringField(TEXT("mesh"), Mesh->Meshes[0].Mesh->GetPathName());
						}
					}
					else if (UNiagaraRibbonRendererProperties* Ribbon = Cast<UNiagaraRibbonRendererProperties>(Renderer))
					{
						RendObj->SetStringField(TEXT("type"), TEXT("ribbon"));
						if (Ribbon->Material)
						{
							RendObj->SetStringField(TEXT("material"), Ribbon->Material->GetPathName());
						}
					}
					else if (Cast<UNiagaraLightRendererProperties>(Renderer))
					{
						RendObj->SetStringField(TEXT("type"), TEXT("light"));
					}
					else
					{
						RendObj->SetStringField(TEXT("type"), Renderer->GetClass()->GetName());
					}

					RenderersArray.Add(MakeShared<FJsonValueObject>(RendObj));
				}
				EmitterObj->SetArrayField(TEXT("renderers"), RenderersArray);

				// Module stacks
				UNiagaraScriptSource* ScriptSource = Cast<UNiagaraScriptSource>(EmitterData->GraphSource);
				if (ScriptSource && ScriptSource->NodeGraph)
				{
					UNiagaraGraph* Graph = ScriptSource->NodeGraph;
					TArray<UNiagaraNodeFunctionCall*> FunctionNodes;
					Graph->GetNodesOfClass<UNiagaraNodeFunctionCall>(FunctionNodes);

					TMap<FString, TArray<TSharedPtr<FJsonValue>>> ModulesByStage;

					for (UNiagaraNodeFunctionCall* FuncNode : FunctionNodes)
					{
						if (!FuncNode) continue;

						FString Stage = ScriptUsageToString(FNiagaraStackGraphUtilities::GetOutputNodeUsage(*FuncNode));
						TSharedPtr<FJsonObject> ModObj = MakeShared<FJsonObject>();
						ModObj->SetStringField(TEXT("name"), FuncNode->GetNodeTitle(ENodeTitleType::ListView).ToString());

						if (FuncNode->FunctionScript)
						{
							ModObj->SetStringField(TEXT("script_path"), FuncNode->FunctionScript->GetPathName());
						}
						ModObj->SetBoolField(TEXT("enabled"), FuncNode->IsNodeEnabled());

						if (bIncludeModuleInputs)
						{
							TArray<TSharedPtr<FJsonValue>> InputsArray;
							for (UEdGraphPin* Pin : FuncNode->Pins)
							{
								if (!Pin || Pin->bHidden || Pin->Direction != EGPD_Input) continue;
								TSharedPtr<FJsonObject> InputObj = MakeShared<FJsonObject>();
								InputObj->SetStringField(TEXT("name"), Pin->PinName.ToString());
								InputObj->SetStringField(TEXT("type"), Pin->PinType.PinCategory.ToString());
								if (!Pin->DefaultValue.IsEmpty())
								{
									InputObj->SetStringField(TEXT("default_value"), Pin->DefaultValue);
								}
								InputObj->SetBoolField(TEXT("connected"), Pin->LinkedTo.Num() > 0);
								InputsArray.Add(MakeShared<FJsonValueObject>(InputObj));
							}
							ModObj->SetArrayField(TEXT("inputs"), InputsArray);
						}

						ModulesByStage.FindOrAdd(Stage).Add(MakeShared<FJsonValueObject>(ModObj));
					}

					TSharedPtr<FJsonObject> StackObj = MakeShared<FJsonObject>();
					for (auto& Pair : ModulesByStage)
					{
						StackObj->SetArrayField(Pair.Key, Pair.Value);
					}
					EmitterObj->SetObjectField(TEXT("module_stacks"), StackObj);
				}

				EmittersArray.Add(MakeShared<FJsonValueObject>(EmitterObj));
			}

			Result->SetArrayField(TEXT("emitters"), EmittersArray);
			Result->SetNumberField(TEXT("emitter_count"), EmittersArray.Num());

			FString OutStr;
			TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutStr);
			FJsonSerializer::Serialize(Result.ToSharedRef(), Writer);

			OnComplete(MakeJsonResponse(OutStr));
		});

	return true;
}
