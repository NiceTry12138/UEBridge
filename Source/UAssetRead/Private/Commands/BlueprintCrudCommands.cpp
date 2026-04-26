// Copyright Epic Games, Inc. All Rights Reserved.
// Blueprint CRUD command routes:
//   POST /create_blueprint
//   POST /add_blueprint_component
//   POST /compile_blueprint
//   POST /add_blueprint_variable
//   GET  /get_blueprint_info?path=...
//   GET  /list_blueprints?path=/Game/&recursive=true

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
#include "Kismet2/KismetEditorUtilities.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/SCS_Node.h"
#include "Components/StaticMeshComponent.h"
#include "Components/PointLightComponent.h"
#include "Components/SpotLightComponent.h"
#include "Components/DirectionalLightComponent.h"
#include "Camera/CameraComponent.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Components/AudioComponent.h"
#include "Components/BoxComponent.h"
#include "Components/SphereComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/ArrowComponent.h"
#include "Components/BillboardComponent.h"
#include "GameFramework/Actor.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/Character.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "EdGraphSchema_K2.h"
#include "EdGraph/EdGraph.h"

#include "UAssetReadHelpers.h"

// ---------------------------------------------------------------------------
// Shared helpers
// ---------------------------------------------------------------------------

static UBlueprint* LoadBlueprintByPath(const FString& Path)
{
	UBlueprint* BP = LoadObject<UBlueprint>(nullptr, *Path);
	if (!BP)
	{
		// Try with _C suffix stripped
		FString TrimmedPath = Path;
		if (TrimmedPath.EndsWith(TEXT("_C")))
		{
			TrimmedPath.RemoveFromEnd(TEXT("_C"));
			BP = LoadObject<UBlueprint>(nullptr, *TrimmedPath);
		}
	}
	return BP;
}

static UClass* GetParentClassFromString(const FString& ParentClass, FString* OutError = nullptr)
{
	if (ParentClass.Equals(TEXT("Actor"), ESearchCase::IgnoreCase))   { return AActor::StaticClass(); }
	if (ParentClass.Equals(TEXT("Pawn"), ESearchCase::IgnoreCase))    { return APawn::StaticClass(); }
	if (ParentClass.Equals(TEXT("Character"), ESearchCase::IgnoreCase)) { return ACharacter::StaticClass(); }

	TArray<FString> PathsToTry;
	if (ParentClass.Contains(TEXT("/")) || ParentClass.Contains(TEXT(".")))
	{
		PathsToTry.Add(ParentClass);
	}

	FString ClassName = ParentClass;
	FString ClassNameNoPrefix = ParentClass.StartsWith(TEXT("A")) ? ParentClass.RightChop(1) : ParentClass;

	PathsToTry.Add(FString::Printf(TEXT("/Script/Engine.%s"), *ClassName));
	PathsToTry.Add(FString::Printf(TEXT("/Script/Engine.A%s"), *ClassNameNoPrefix));
	PathsToTry.Add(FString::Printf(TEXT("/Script/GameFramework.%s"), *ClassName));

	for (const FString& Path : PathsToTry)
	{
		if (UClass* FoundClass = FindObject<UClass>(nullptr, *Path))
		{
			if (FoundClass->IsChildOf(AActor::StaticClass())) return FoundClass;
		}
	}
	for (const FString& Path : PathsToTry)
	{
		if (UClass* LoadedClass = LoadObject<UClass>(nullptr, *Path))
		{
			if (LoadedClass->IsChildOf(AActor::StaticClass())) return LoadedClass;
		}
	}

	for (TObjectIterator<UClass> It; It; ++It)
	{
		UClass* TestClass = *It;
		if (TestClass->IsChildOf(AActor::StaticClass()))
		{
			FString TestClassName = TestClass->GetName();
			if (TestClassName.Equals(ClassName, ESearchCase::IgnoreCase) ||
				TestClassName.Equals(FString::Printf(TEXT("A%s"), *ClassNameNoPrefix), ESearchCase::IgnoreCase))
			{
				return TestClass;
			}
		}
	}

	if (OutError)
	{
		*OutError = FString::Printf(TEXT("Could not find Actor class '%s'"), *ParentClass);
	}
	return nullptr;
}

static UClass* GetComponentClassFromString(const FString& ComponentType)
{
	if (ComponentType.Equals(TEXT("StaticMesh"), ESearchCase::IgnoreCase))          return UStaticMeshComponent::StaticClass();
	if (ComponentType.Equals(TEXT("PointLight"), ESearchCase::IgnoreCase))          return UPointLightComponent::StaticClass();
	if (ComponentType.Equals(TEXT("SpotLight"), ESearchCase::IgnoreCase))           return USpotLightComponent::StaticClass();
	if (ComponentType.Equals(TEXT("DirectionalLight"), ESearchCase::IgnoreCase))    return UDirectionalLightComponent::StaticClass();
	if (ComponentType.Equals(TEXT("Camera"), ESearchCase::IgnoreCase))              return UCameraComponent::StaticClass();
	if (ComponentType.Equals(TEXT("SceneCapture2D"), ESearchCase::IgnoreCase))      return USceneCaptureComponent2D::StaticClass();
	if (ComponentType.Equals(TEXT("Audio"), ESearchCase::IgnoreCase))               return UAudioComponent::StaticClass();
	if (ComponentType.Equals(TEXT("Box"), ESearchCase::IgnoreCase) ||
		ComponentType.Equals(TEXT("BoxCollision"), ESearchCase::IgnoreCase))        return UBoxComponent::StaticClass();
	if (ComponentType.Equals(TEXT("Sphere"), ESearchCase::IgnoreCase) ||
		ComponentType.Equals(TEXT("SphereCollision"), ESearchCase::IgnoreCase))     return USphereComponent::StaticClass();
	if (ComponentType.Equals(TEXT("Capsule"), ESearchCase::IgnoreCase) ||
		ComponentType.Equals(TEXT("CapsuleCollision"), ESearchCase::IgnoreCase))    return UCapsuleComponent::StaticClass();
	if (ComponentType.Equals(TEXT("Arrow"), ESearchCase::IgnoreCase))               return UArrowComponent::StaticClass();
	if (ComponentType.Equals(TEXT("Billboard"), ESearchCase::IgnoreCase))           return UBillboardComponent::StaticClass();
	if (ComponentType.Equals(TEXT("Scene"), ESearchCase::IgnoreCase))               return USceneComponent::StaticClass();
	return LoadObject<UClass>(nullptr, *ComponentType);
}

// ---------------------------------------------------------------------------
// POST /create_blueprint
// Body: {"name":"BP_Foo","parent_class":"Actor","path":"/Game/Blueprints/"}
// ---------------------------------------------------------------------------

bool FUAssetReadModule::HandleCreateBlueprint(
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

	FString BlueprintName;
	if (!BodyJson->TryGetStringField(TEXT("name"), BlueprintName) || BlueprintName.IsEmpty())
	{
		OnComplete(MakeJsonError(EHttpServerResponseCodes::BadRequest, TEXT("Missing field: name")));
		return true;
	}

	FString ParentClassStr = TEXT("Actor");
	BodyJson->TryGetStringField(TEXT("parent_class"), ParentClassStr);

	FString Path = TEXT("/Game/Blueprints/");
	BodyJson->TryGetStringField(TEXT("path"), Path);
	if (!Path.EndsWith(TEXT("/"))) Path += TEXT("/");

	TSharedRef<bool> AliveRef = bAlive;

	AsyncTask(ENamedThreads::GameThread,
		[AliveRef, BlueprintName = MoveTemp(BlueprintName), ParentClassStr = MoveTemp(ParentClassStr),
		 Path = MoveTemp(Path), OnComplete]() mutable
		{
			if (!*AliveRef) { return; }

			FString ClassError;
			UClass* ParentClass = GetParentClassFromString(ParentClassStr, &ClassError);
			if (!ParentClass)
			{
				OnComplete(MakeJsonError(EHttpServerResponseCodes::BadRequest,
					FString::Printf(TEXT("Invalid parent class '%s'. %s"), *ParentClassStr, *ClassError)));
				return;
			}

			FString PackagePath = Path + BlueprintName;

			if (UObject* Existing = StaticLoadObject(UObject::StaticClass(), nullptr, *PackagePath))
			{
				if (UBlueprint* ExistingBP = Cast<UBlueprint>(Existing))
				{
					TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
					Result->SetStringField(TEXT("blueprint_path"), PackagePath);
					Result->SetStringField(TEXT("blueprint_name"), BlueprintName);
					Result->SetStringField(TEXT("parent_class"), ExistingBP->ParentClass ? ExistingBP->ParentClass->GetName() : TEXT("Unknown"));
					Result->SetBoolField(TEXT("already_exists"), true);
					FString OutStr;
					TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&OutStr);
					FJsonSerializer::Serialize(Result.ToSharedRef(), W);
					OnComplete(MakeJsonResponse(OutStr));
				}
				else
				{
					OnComplete(MakeJsonError(EHttpServerResponseCodes::BadRequest,
						FString::Printf(TEXT("Asset at '%s' is not a Blueprint"), *PackagePath)));
				}
				return;
			}

			UPackage* Package = CreatePackage(*PackagePath);
			if (!Package)
			{
				OnComplete(MakeJsonError(EHttpServerResponseCodes::ServerError, TEXT("Failed to create package")));
				return;
			}

			UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(
				ParentClass, Package, *BlueprintName,
				BPTYPE_Normal, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());

			if (!Blueprint)
			{
				OnComplete(MakeJsonError(EHttpServerResponseCodes::ServerError, TEXT("Failed to create Blueprint")));
				return;
			}

			FAssetRegistryModule::AssetCreated(Blueprint);
			Package->MarkPackageDirty();
			FKismetEditorUtilities::CompileBlueprint(Blueprint);

			TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetStringField(TEXT("blueprint_path"), PackagePath);
			Result->SetStringField(TEXT("blueprint_name"), BlueprintName);
			Result->SetStringField(TEXT("parent_class"), ParentClass->GetName());

			FString OutStr;
			TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutStr);
			FJsonSerializer::Serialize(Result.ToSharedRef(), Writer);
			OnComplete(MakeJsonResponse(OutStr));
		});

	return true;
}

// ---------------------------------------------------------------------------
// POST /add_blueprint_component
// Body: {"blueprint_path":"...","component_type":"StaticMesh","component_name":"MyMesh","attach_to":"RootComponent"}
// ---------------------------------------------------------------------------

bool FUAssetReadModule::HandleAddBlueprintComponent(
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

	FString BlueprintPath, ComponentType;
	if (!BodyJson->TryGetStringField(TEXT("blueprint_path"), BlueprintPath) || BlueprintPath.IsEmpty())
	{
		OnComplete(MakeJsonError(EHttpServerResponseCodes::BadRequest, TEXT("Missing field: blueprint_path")));
		return true;
	}
	if (!BodyJson->TryGetStringField(TEXT("component_type"), ComponentType) || ComponentType.IsEmpty())
	{
		OnComplete(MakeJsonError(EHttpServerResponseCodes::BadRequest, TEXT("Missing field: component_type")));
		return true;
	}

	FString ComponentName = TEXT("NewComponent");
	BodyJson->TryGetStringField(TEXT("component_name"), ComponentName);

	FString AttachTo;
	BodyJson->TryGetStringField(TEXT("attach_to"), AttachTo);

	TSharedRef<bool> AliveRef = bAlive;

	AsyncTask(ENamedThreads::GameThread,
		[AliveRef, BlueprintPath = MoveTemp(BlueprintPath), ComponentType = MoveTemp(ComponentType),
		 ComponentName = MoveTemp(ComponentName), AttachTo = MoveTemp(AttachTo), OnComplete]() mutable
		{
			if (!*AliveRef) { return; }

			UBlueprint* Blueprint = LoadBlueprintByPath(BlueprintPath);
			if (!Blueprint)
			{
				OnComplete(MakeJsonError(EHttpServerResponseCodes::NoContent,
					FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintPath)));
				return;
			}

			UClass* ComponentClass = GetComponentClassFromString(ComponentType);
			if (!ComponentClass || !ComponentClass->IsChildOf(UActorComponent::StaticClass()))
			{
				OnComplete(MakeJsonError(EHttpServerResponseCodes::BadRequest,
					FString::Printf(TEXT("Unknown or invalid component type: %s"), *ComponentType)));
				return;
			}

			USCS_Node* NewNode = Blueprint->SimpleConstructionScript->CreateNode(ComponentClass, *ComponentName);
			if (!NewNode)
			{
				OnComplete(MakeJsonError(EHttpServerResponseCodes::ServerError, TEXT("Failed to create component node")));
				return;
			}

			if (!AttachTo.IsEmpty())
			{
				USCS_Node* ParentNode = nullptr;
				for (USCS_Node* Node : Blueprint->SimpleConstructionScript->GetAllNodes())
				{
					if (Node->GetVariableName().ToString() == AttachTo)
					{
						ParentNode = Node;
						break;
					}
				}
				if (ParentNode)
					ParentNode->AddChildNode(NewNode);
				else
					Blueprint->SimpleConstructionScript->AddNode(NewNode);
			}
			else
			{
				Blueprint->SimpleConstructionScript->AddNode(NewNode);
			}

			FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

			TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetStringField(TEXT("component_name"), ComponentName);
			Result->SetStringField(TEXT("component_class"), ComponentClass->GetName());

			FString OutStr;
			TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutStr);
			FJsonSerializer::Serialize(Result.ToSharedRef(), Writer);
			OnComplete(MakeJsonResponse(OutStr));
		});

	return true;
}

// ---------------------------------------------------------------------------
// POST /compile_blueprint
// Body: {"blueprint_path":"..."}
// ---------------------------------------------------------------------------

bool FUAssetReadModule::HandleCompileBlueprint(
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

	FString BlueprintPath;
	if (!BodyJson->TryGetStringField(TEXT("blueprint_path"), BlueprintPath) || BlueprintPath.IsEmpty())
	{
		OnComplete(MakeJsonError(EHttpServerResponseCodes::BadRequest, TEXT("Missing field: blueprint_path")));
		return true;
	}

	TSharedRef<bool> AliveRef = bAlive;

	AsyncTask(ENamedThreads::GameThread,
		[AliveRef, BlueprintPath = MoveTemp(BlueprintPath), OnComplete]() mutable
		{
			if (!*AliveRef) { return; }

			UBlueprint* Blueprint = LoadBlueprintByPath(BlueprintPath);
			if (!Blueprint)
			{
				OnComplete(MakeJsonError(EHttpServerResponseCodes::NoContent,
					FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintPath)));
				return;
			}

			FKismetEditorUtilities::CompileBlueprint(Blueprint);

			bool bHasErrors = Blueprint->Status == BS_Error;

			TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetBoolField(TEXT("compiled"), true);
			Result->SetBoolField(TEXT("has_errors"), bHasErrors);
			Result->SetStringField(TEXT("status"), bHasErrors ? TEXT("Error") : TEXT("Success"));

			FString OutStr;
			TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutStr);
			FJsonSerializer::Serialize(Result.ToSharedRef(), Writer);
			OnComplete(MakeJsonResponse(OutStr));
		});

	return true;
}

// ---------------------------------------------------------------------------
// POST /add_blueprint_variable
// Body: {"blueprint_path":"...","variable_name":"MyVar","variable_type":"Float","default_value":"1.0","is_instance_editable":true}
// ---------------------------------------------------------------------------

bool FUAssetReadModule::HandleAddBlueprintVariable(
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

	FString BlueprintPath, VariableName, VariableType;
	if (!BodyJson->TryGetStringField(TEXT("blueprint_path"), BlueprintPath) || BlueprintPath.IsEmpty())
	{
		OnComplete(MakeJsonError(EHttpServerResponseCodes::BadRequest, TEXT("Missing field: blueprint_path")));
		return true;
	}
	if (!BodyJson->TryGetStringField(TEXT("variable_name"), VariableName) || VariableName.IsEmpty())
	{
		OnComplete(MakeJsonError(EHttpServerResponseCodes::BadRequest, TEXT("Missing field: variable_name")));
		return true;
	}
	if (!BodyJson->TryGetStringField(TEXT("variable_type"), VariableType) || VariableType.IsEmpty())
	{
		OnComplete(MakeJsonError(EHttpServerResponseCodes::BadRequest, TEXT("Missing field: variable_type")));
		return true;
	}

	bool bInstanceEditable = false;
	bool bBlueprintReadOnly = false;
	FString DefaultValue;
	BodyJson->TryGetBoolField(TEXT("is_instance_editable"), bInstanceEditable);
	BodyJson->TryGetBoolField(TEXT("is_blueprint_read_only"), bBlueprintReadOnly);
	BodyJson->TryGetStringField(TEXT("default_value"), DefaultValue);

	TSharedRef<bool> AliveRef = bAlive;

	AsyncTask(ENamedThreads::GameThread,
		[AliveRef, BlueprintPath = MoveTemp(BlueprintPath), VariableName = MoveTemp(VariableName),
		 VariableType = MoveTemp(VariableType), DefaultValue = MoveTemp(DefaultValue),
		 bInstanceEditable, bBlueprintReadOnly, OnComplete]() mutable
		{
			if (!*AliveRef) { return; }

			UBlueprint* Blueprint = LoadBlueprintByPath(BlueprintPath);
			if (!Blueprint)
			{
				OnComplete(MakeJsonError(EHttpServerResponseCodes::NoContent,
					FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintPath)));
				return;
			}

			FEdGraphPinType PinType;

			if (VariableType.Equals(TEXT("Boolean"), ESearchCase::IgnoreCase) || VariableType.Equals(TEXT("bool"), ESearchCase::IgnoreCase))
				PinType.PinCategory = UEdGraphSchema_K2::PC_Boolean;
			else if (VariableType.Equals(TEXT("Integer"), ESearchCase::IgnoreCase) || VariableType.Equals(TEXT("int"), ESearchCase::IgnoreCase))
				PinType.PinCategory = UEdGraphSchema_K2::PC_Int;
			else if (VariableType.Equals(TEXT("Float"), ESearchCase::IgnoreCase) || VariableType.Equals(TEXT("float"), ESearchCase::IgnoreCase) || VariableType.Equals(TEXT("double"), ESearchCase::IgnoreCase))
			{
				PinType.PinCategory = UEdGraphSchema_K2::PC_Real;
				PinType.PinSubCategory = UEdGraphSchema_K2::PC_Double;
			}
			else if (VariableType.Equals(TEXT("String"), ESearchCase::IgnoreCase))
				PinType.PinCategory = UEdGraphSchema_K2::PC_String;
			else if (VariableType.Equals(TEXT("Name"), ESearchCase::IgnoreCase))
				PinType.PinCategory = UEdGraphSchema_K2::PC_Name;
			else if (VariableType.Equals(TEXT("Text"), ESearchCase::IgnoreCase))
				PinType.PinCategory = UEdGraphSchema_K2::PC_Text;
			else if (VariableType.Equals(TEXT("Vector"), ESearchCase::IgnoreCase))
			{ PinType.PinCategory = UEdGraphSchema_K2::PC_Struct; PinType.PinSubCategoryObject = TBaseStructure<FVector>::Get(); }
			else if (VariableType.Equals(TEXT("Rotator"), ESearchCase::IgnoreCase))
			{ PinType.PinCategory = UEdGraphSchema_K2::PC_Struct; PinType.PinSubCategoryObject = TBaseStructure<FRotator>::Get(); }
			else if (VariableType.Equals(TEXT("Transform"), ESearchCase::IgnoreCase))
			{ PinType.PinCategory = UEdGraphSchema_K2::PC_Struct; PinType.PinSubCategoryObject = TBaseStructure<FTransform>::Get(); }
			else if (VariableType.Equals(TEXT("Color"), ESearchCase::IgnoreCase) || VariableType.Equals(TEXT("LinearColor"), ESearchCase::IgnoreCase))
			{ PinType.PinCategory = UEdGraphSchema_K2::PC_Struct; PinType.PinSubCategoryObject = TBaseStructure<FLinearColor>::Get(); }
			else if (VariableType.Equals(TEXT("Actor"), ESearchCase::IgnoreCase))
			{ PinType.PinCategory = UEdGraphSchema_K2::PC_Object; PinType.PinSubCategoryObject = AActor::StaticClass(); }
			else
			{
				UClass* FoundClass = FindObject<UClass>(nullptr,
					*FString::Printf(TEXT("/Script/Engine.%s"), *VariableType));
				if (!FoundClass) FoundClass = LoadClass<UObject>(nullptr, *VariableType);
				if (FoundClass)
				{ PinType.PinCategory = UEdGraphSchema_K2::PC_Object; PinType.PinSubCategoryObject = FoundClass; }
				else
				{
					OnComplete(MakeJsonError(EHttpServerResponseCodes::BadRequest,
						FString::Printf(TEXT("Unknown variable type: %s"), *VariableType)));
					return;
				}
			}

			if (!FBlueprintEditorUtils::AddMemberVariable(Blueprint, FName(*VariableName), PinType))
			{
				OnComplete(MakeJsonError(EHttpServerResponseCodes::ServerError, TEXT("Failed to add variable")));
				return;
			}

			FBlueprintEditorUtils::SetBlueprintOnlyEditableFlag(Blueprint, FName(*VariableName), !bInstanceEditable);
			FBlueprintEditorUtils::SetBlueprintPropertyReadOnlyFlag(Blueprint, FName(*VariableName), bBlueprintReadOnly);

			if (!DefaultValue.IsEmpty())
			{
				for (FBPVariableDescription& VarDesc : Blueprint->NewVariables)
				{
					if (VarDesc.VarName == FName(*VariableName))
					{
						VarDesc.DefaultValue = DefaultValue;
						FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
						break;
					}
				}
			}

			TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetStringField(TEXT("variable_name"), VariableName);
			Result->SetStringField(TEXT("variable_type"), VariableType);

			FString OutStr;
			TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutStr);
			FJsonSerializer::Serialize(Result.ToSharedRef(), Writer);
			OnComplete(MakeJsonResponse(OutStr));
		});

	return true;
}

// ---------------------------------------------------------------------------
// GET /get_blueprint_info?path=...
// ---------------------------------------------------------------------------

bool FUAssetReadModule::HandleGetBlueprintInfo(
	const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
	const FString* PathParam = Request.QueryParams.Find(TEXT("path"));
	if (!PathParam || PathParam->IsEmpty())
	{
		OnComplete(MakeJsonError(EHttpServerResponseCodes::BadRequest, TEXT("Missing query parameter: path")));
		return true;
	}
	FString AssetPath = *PathParam;

	TSharedRef<bool> AliveRef = bAlive;

	AsyncTask(ENamedThreads::GameThread,
		[AliveRef, AssetPath = MoveTemp(AssetPath), OnComplete]() mutable
		{
			if (!*AliveRef) { return; }

			UBlueprint* Blueprint = LoadBlueprintByPath(AssetPath);
			if (!Blueprint)
			{
				OnComplete(MakeJsonError(EHttpServerResponseCodes::NoContent,
					FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath)));
				return;
			}

			TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetStringField(TEXT("name"), Blueprint->GetName());
			Result->SetStringField(TEXT("path"), Blueprint->GetPathName());
			Result->SetStringField(TEXT("parent_class"), Blueprint->ParentClass ? Blueprint->ParentClass->GetName() : TEXT("None"));
			Result->SetStringField(TEXT("blueprint_type"), UEnum::GetValueAsString(Blueprint->BlueprintType));

			FString StatusStr;
			switch (Blueprint->Status)
			{
			case BS_Unknown:       StatusStr = TEXT("Unknown"); break;
			case BS_Dirty:         StatusStr = TEXT("Dirty"); break;
			case BS_Error:         StatusStr = TEXT("Error"); break;
			case BS_UpToDate:      StatusStr = TEXT("UpToDate"); break;
			case BS_BeingCreated:  StatusStr = TEXT("BeingCreated"); break;
			default:               StatusStr = TEXT("Unknown"); break;
			}
			Result->SetStringField(TEXT("status"), StatusStr);

			TArray<TSharedPtr<FJsonValue>> VariablesArray;
			for (const FBPVariableDescription& Var : Blueprint->NewVariables)
			{
				TSharedPtr<FJsonObject> VarObj = MakeShared<FJsonObject>();
				VarObj->SetStringField(TEXT("name"), Var.VarName.ToString());
				VarObj->SetStringField(TEXT("type"), Var.VarType.PinCategory.ToString());
				VariablesArray.Add(MakeShared<FJsonValueObject>(VarObj));
			}
			Result->SetArrayField(TEXT("variables"), VariablesArray);

			TArray<TSharedPtr<FJsonValue>> ComponentsArray;
			if (Blueprint->SimpleConstructionScript)
			{
				for (USCS_Node* Node : Blueprint->SimpleConstructionScript->GetAllNodes())
				{
					TSharedPtr<FJsonObject> CompObj = MakeShared<FJsonObject>();
					CompObj->SetStringField(TEXT("name"), Node->GetVariableName().ToString());
					CompObj->SetStringField(TEXT("class"), Node->ComponentClass ? Node->ComponentClass->GetName() : TEXT("None"));
					ComponentsArray.Add(MakeShared<FJsonValueObject>(CompObj));
				}
			}
			Result->SetArrayField(TEXT("components"), ComponentsArray);

			TArray<TSharedPtr<FJsonValue>> FunctionsArray;
			for (UEdGraph* Graph : Blueprint->FunctionGraphs)
			{
				TSharedPtr<FJsonObject> FuncObj = MakeShared<FJsonObject>();
				FuncObj->SetStringField(TEXT("name"), Graph->GetName());
				FunctionsArray.Add(MakeShared<FJsonValueObject>(FuncObj));
			}
			Result->SetArrayField(TEXT("functions"), FunctionsArray);

			FString OutStr;
			TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutStr);
			FJsonSerializer::Serialize(Result.ToSharedRef(), Writer);
			OnComplete(MakeJsonResponse(OutStr));
		});

	return true;
}

// ---------------------------------------------------------------------------
// GET /list_blueprints?path=/Game/&recursive=true
// ---------------------------------------------------------------------------

bool FUAssetReadModule::HandleListBlueprints(
	const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
	FString AssetPath = Request.QueryParams.FindRef(TEXT("path"));
	if (AssetPath.IsEmpty()) AssetPath = TEXT("/Game/");
	bool bRecursive = Request.QueryParams.FindRef(TEXT("recursive")).ToLower() != TEXT("false");

	TSharedRef<bool> AliveRef = bAlive;

	AsyncTask(ENamedThreads::GameThread,
		[AliveRef, AssetPath = MoveTemp(AssetPath), bRecursive, OnComplete]() mutable
		{
			if (!*AliveRef) { return; }

			IAssetRegistry& AssetRegistry =
				FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
			AssetRegistry.SearchAllAssets(false);

			TArray<FAssetData> AssetList;
			AssetRegistry.GetAssetsByPath(*AssetPath, AssetList, bRecursive);

			TArray<TSharedPtr<FJsonValue>> BlueprintsArray;
			for (const FAssetData& Asset : AssetList)
			{
				if (Asset.AssetClassPath == UBlueprint::StaticClass()->GetClassPathName())
				{
					TSharedPtr<FJsonObject> BPObj = MakeShared<FJsonObject>();
					BPObj->SetStringField(TEXT("name"), Asset.AssetName.ToString());
					BPObj->SetStringField(TEXT("path"), Asset.GetObjectPathString());
					BlueprintsArray.Add(MakeShared<FJsonValueObject>(BPObj));
				}
			}

			TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetArrayField(TEXT("blueprints"), BlueprintsArray);
			Result->SetNumberField(TEXT("count"), BlueprintsArray.Num());

			FString OutStr;
			TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutStr);
			FJsonSerializer::Serialize(Result.ToSharedRef(), Writer);
			OnComplete(MakeJsonResponse(OutStr));
		});

	return true;
}
