// Copyright Epic Games, Inc. All Rights Reserved.

#include "Core/Exporters/FMaterialExporter.h"

#include "Materials/Material.h"
#include "Materials/MaterialInstance.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialExpressionTextureSample.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionConstant3Vector.h"
#include "Materials/MaterialExpressionConstant4Vector.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionVectorParameter.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

/** Build "extra" data for well-known material expression types. */
static TSharedPtr<FJsonObject> BuildMaterialExpressionExtra(UMaterialExpression* Expr)
{
	TSharedPtr<FJsonObject> Extra = MakeShareable(new FJsonObject);

	if (UMaterialExpressionTextureSample* TexSample = Cast<UMaterialExpressionTextureSample>(Expr))
	{
		Extra->SetStringField(TEXT("texture"),
			TexSample->Texture ? TexSample->Texture->GetPathName() : TEXT(""));
	}
	else if (UMaterialExpressionConstant* Const = Cast<UMaterialExpressionConstant>(Expr))
	{
		Extra->SetNumberField(TEXT("value"), Const->R);
	}
	else if (UMaterialExpressionConstant3Vector* Const3 = Cast<UMaterialExpressionConstant3Vector>(Expr))
	{
		TSharedPtr<FJsonObject> ColorObj = MakeShareable(new FJsonObject);
		ColorObj->SetNumberField(TEXT("r"), Const3->Constant.R);
		ColorObj->SetNumberField(TEXT("g"), Const3->Constant.G);
		ColorObj->SetNumberField(TEXT("b"), Const3->Constant.B);
		Extra->SetObjectField(TEXT("value"), ColorObj);
	}
	else if (UMaterialExpressionConstant4Vector* Const4 = Cast<UMaterialExpressionConstant4Vector>(Expr))
	{
		TSharedPtr<FJsonObject> ColorObj = MakeShareable(new FJsonObject);
		ColorObj->SetNumberField(TEXT("r"), Const4->Constant.R);
		ColorObj->SetNumberField(TEXT("g"), Const4->Constant.G);
		ColorObj->SetNumberField(TEXT("b"), Const4->Constant.B);
		ColorObj->SetNumberField(TEXT("a"), Const4->Constant.A);
		Extra->SetObjectField(TEXT("value"), ColorObj);
	}
	else if (UMaterialExpressionScalarParameter* ScalarParam = Cast<UMaterialExpressionScalarParameter>(Expr))
	{
		Extra->SetStringField(TEXT("parameter_name"), ScalarParam->ParameterName.ToString());
		Extra->SetNumberField(TEXT("default_value"), ScalarParam->DefaultValue);
		Extra->SetStringField(TEXT("group"), ScalarParam->Group.ToString());
	}
	else if (UMaterialExpressionVectorParameter* VecParam = Cast<UMaterialExpressionVectorParameter>(Expr))
	{
		Extra->SetStringField(TEXT("parameter_name"), VecParam->ParameterName.ToString());
		TSharedPtr<FJsonObject> DefObj = MakeShareable(new FJsonObject);
		DefObj->SetNumberField(TEXT("r"), VecParam->DefaultValue.R);
		DefObj->SetNumberField(TEXT("g"), VecParam->DefaultValue.G);
		DefObj->SetNumberField(TEXT("b"), VecParam->DefaultValue.B);
		DefObj->SetNumberField(TEXT("a"), VecParam->DefaultValue.A);
		Extra->SetObjectField(TEXT("default_value"), DefObj);
		Extra->SetStringField(TEXT("group"), VecParam->Group.ToString());
	}

	return Extra;
}

// ---------------------------------------------------------------------------
// UMaterial graph builder
// ---------------------------------------------------------------------------

static TSharedPtr<FJsonObject> BuildMaterialGraph(UMaterial* Material)
{
	TSharedPtr<FJsonObject> GraphObj = MakeShareable(new FJsonObject);
	GraphObj->SetStringField(TEXT("graph_name"), TEXT("MaterialGraph"));
	GraphObj->SetStringField(TEXT("graph_type"), TEXT("Material"));

	const TConstArrayView<TObjectPtr<UMaterialExpression>>& Expressions = Material->GetExpressions();

	// Map expression pointer -> array index (used as node id)
	TMap<UMaterialExpression*, int32> ExprIndexMap;
	for (int32 i = 0; i < Expressions.Num(); ++i)
	{
		if (Expressions[i])
		{
			ExprIndexMap.Add(Expressions[i].Get(), i);
		}
	}

	TArray<TSharedPtr<FJsonValue>> NodesArray;
	TArray<TSharedPtr<FJsonValue>> EdgesArray;

	// ---- Build expression nodes ----
	for (int32 ExprIdx = 0; ExprIdx < Expressions.Num(); ++ExprIdx)
	{
		UMaterialExpression* Expr = Expressions[ExprIdx].Get();
		if (!Expr) continue;

		const FString NodeId = FString::Printf(TEXT("EXPR_%d"), ExprIdx);

		TSharedPtr<FJsonObject> NodeObj = MakeShareable(new FJsonObject);
		NodeObj->SetStringField(TEXT("node_id"), NodeId);
		NodeObj->SetStringField(TEXT("node_class"), Expr->GetClass()->GetName());
		NodeObj->SetStringField(TEXT("title"), Expr->Desc.IsEmpty()
			? Expr->GetClass()->GetName() : Expr->Desc);

		TArray<TSharedPtr<FJsonValue>> PosArray;
		PosArray.Add(MakeShareable(new FJsonValueNumber(Expr->MaterialExpressionEditorX)));
		PosArray.Add(MakeShareable(new FJsonValueNumber(Expr->MaterialExpressionEditorY)));
		NodeObj->SetArrayField(TEXT("pos"), PosArray);

		TArray<TSharedPtr<FJsonValue>> PinsArray;

		// Input pins
		const TArrayView<FExpressionInput*> InputsView = Expr->GetInputsView();
		const int32 NumInputs = InputsView.Num();
		for (int32 InputIdx = 0; InputIdx < NumInputs; ++InputIdx)
		{
			FExpressionInput* Input = InputsView[InputIdx];
			const FName InputName = Expr->GetInputName(InputIdx);
			const FString PinId = FString::Printf(TEXT("EXPR_%d_IN_%d"), ExprIdx, InputIdx);

			TSharedPtr<FJsonObject> PinObj = MakeShareable(new FJsonObject);
			PinObj->SetStringField(TEXT("pin_id"), PinId);
			PinObj->SetStringField(TEXT("name"), InputName.ToString());
			PinObj->SetStringField(TEXT("direction"), TEXT("input"));

			TArray<TSharedPtr<FJsonValue>> Links;
			if (Input && Input->Expression)
			{
				const int32* ConnectedIdx = ExprIndexMap.Find(Input->Expression);
				if (ConnectedIdx)
				{
					const FString ConnectedPinId = FString::Printf(TEXT("EXPR_%d_OUT_%d"),
						*ConnectedIdx, Input->OutputIndex);
					Links.Add(MakeShareable(new FJsonValueString(ConnectedPinId)));

					// Flat edge
					TSharedPtr<FJsonObject> EdgeObj = MakeShareable(new FJsonObject);
					EdgeObj->SetStringField(TEXT("from_node"),
						FString::Printf(TEXT("EXPR_%d"), *ConnectedIdx));
					EdgeObj->SetStringField(TEXT("from_pin"), ConnectedPinId);
					EdgeObj->SetStringField(TEXT("to_node"), NodeId);
					EdgeObj->SetStringField(TEXT("to_pin"), PinId);
					EdgeObj->SetBoolField(TEXT("is_exec"), false);
					EdgesArray.Add(MakeShareable(new FJsonValueObject(EdgeObj)));
				}
			}
			PinObj->SetArrayField(TEXT("links"), Links);
			PinsArray.Add(MakeShareable(new FJsonValueObject(PinObj)));
		}

		// Output pins
		for (int32 OutputIdx = 0; OutputIdx < Expr->Outputs.Num(); ++OutputIdx)
		{
			const FExpressionOutput& Output = Expr->Outputs[OutputIdx];
			const FString PinId = FString::Printf(TEXT("EXPR_%d_OUT_%d"), ExprIdx, OutputIdx);

			TSharedPtr<FJsonObject> PinObj = MakeShareable(new FJsonObject);
			PinObj->SetStringField(TEXT("pin_id"), PinId);
			PinObj->SetStringField(TEXT("name"), Output.OutputName.ToString());
			PinObj->SetStringField(TEXT("direction"), TEXT("output"));
			PinObj->SetArrayField(TEXT("links"), TArray<TSharedPtr<FJsonValue>>());
			PinsArray.Add(MakeShareable(new FJsonValueObject(PinObj)));
		}

		NodeObj->SetArrayField(TEXT("pins"), PinsArray);
		NodeObj->SetObjectField(TEXT("extra"), BuildMaterialExpressionExtra(Expr));
		NodesArray.Add(MakeShareable(new FJsonValueObject(NodeObj)));
	}

	// ---- Material Result Node (the UMaterial itself) ----
	{
		const FString ResultNodeId = TEXT("MATERIAL_RESULT");

		struct FResultPin
		{
			const TCHAR* Name;
			FExpressionInput* Input;
		};

		UMaterialEditorOnlyData* EditorData = Material->GetEditorOnlyData();
		TArray<FResultPin> ResultPins;
		if (EditorData)
		{
			ResultPins =
			{
				{ TEXT("BaseColor"),           (FExpressionInput*)&EditorData->BaseColor },
				{ TEXT("Metallic"),            (FExpressionInput*)&EditorData->Metallic },
				{ TEXT("Specular"),            (FExpressionInput*)&EditorData->Specular },
				{ TEXT("Roughness"),           (FExpressionInput*)&EditorData->Roughness },
				{ TEXT("EmissiveColor"),       (FExpressionInput*)&EditorData->EmissiveColor },
				{ TEXT("Opacity"),             (FExpressionInput*)&EditorData->Opacity },
				{ TEXT("OpacityMask"),         (FExpressionInput*)&EditorData->OpacityMask },
				{ TEXT("Normal"),              (FExpressionInput*)&EditorData->Normal },
				{ TEXT("WorldPositionOffset"), (FExpressionInput*)&EditorData->WorldPositionOffset },
				{ TEXT("AmbientOcclusion"),    (FExpressionInput*)&EditorData->AmbientOcclusion },
			};
		}

		TArray<TSharedPtr<FJsonValue>> ResultPinsArray;
		for (int32 PinIdx = 0; PinIdx < ResultPins.Num(); ++PinIdx)
		{
			const FResultPin& RP = ResultPins[PinIdx];
			const FString PinId = FString::Printf(TEXT("RESULT_IN_%d"), PinIdx);

			TSharedPtr<FJsonObject> PinObj = MakeShareable(new FJsonObject);
			PinObj->SetStringField(TEXT("pin_id"), PinId);
			PinObj->SetStringField(TEXT("name"), RP.Name);
			PinObj->SetStringField(TEXT("direction"), TEXT("input"));

			TArray<TSharedPtr<FJsonValue>> Links;
			if (RP.Input && RP.Input->Expression)
			{
				const int32* ConnectedIdx = ExprIndexMap.Find(RP.Input->Expression);
				if (ConnectedIdx)
				{
					const FString ConnectedPinId = FString::Printf(TEXT("EXPR_%d_OUT_%d"),
						*ConnectedIdx, RP.Input->OutputIndex);
					Links.Add(MakeShareable(new FJsonValueString(ConnectedPinId)));

					TSharedPtr<FJsonObject> EdgeObj = MakeShareable(new FJsonObject);
					EdgeObj->SetStringField(TEXT("from_node"),
						FString::Printf(TEXT("EXPR_%d"), *ConnectedIdx));
					EdgeObj->SetStringField(TEXT("from_pin"), ConnectedPinId);
					EdgeObj->SetStringField(TEXT("to_node"), ResultNodeId);
					EdgeObj->SetStringField(TEXT("to_pin"), PinId);
					EdgeObj->SetBoolField(TEXT("is_exec"), false);
					EdgesArray.Add(MakeShareable(new FJsonValueObject(EdgeObj)));
				}
			}
			PinObj->SetArrayField(TEXT("links"), Links);
			ResultPinsArray.Add(MakeShareable(new FJsonValueObject(PinObj)));
		}

		TSharedPtr<FJsonObject> ResultNode = MakeShareable(new FJsonObject);
		ResultNode->SetStringField(TEXT("node_id"), ResultNodeId);
		ResultNode->SetStringField(TEXT("node_class"), TEXT("MaterialResultNode"));
		ResultNode->SetStringField(TEXT("title"), TEXT("Material Attributes"));
		TArray<TSharedPtr<FJsonValue>> PosArr;
		PosArr.Add(MakeShareable(new FJsonValueNumber(0)));
		PosArr.Add(MakeShareable(new FJsonValueNumber(0)));
		ResultNode->SetArrayField(TEXT("pos"), PosArr);
		ResultNode->SetArrayField(TEXT("pins"), ResultPinsArray);
		ResultNode->SetObjectField(TEXT("extra"), MakeShareable(new FJsonObject));
		NodesArray.Add(MakeShareable(new FJsonValueObject(ResultNode)));
	}

	GraphObj->SetArrayField(TEXT("nodes"), NodesArray);
	GraphObj->SetArrayField(TEXT("edges"), EdgesArray);
	return GraphObj;
}

// ---------------------------------------------------------------------------
// FMaterialExporter
// ---------------------------------------------------------------------------

TSharedPtr<FJsonObject> FMaterialExporter::Export(UObject* Asset)
{
	// ---- UMaterial ---------------------------------------------------------
	if (UMaterial* Material = Cast<UMaterial>(Asset))
	{
		TSharedPtr<FJsonObject> Root = MakeShareable(new FJsonObject);
		Root->SetStringField(TEXT("assetPath"), Material->GetPathName());
		Root->SetStringField(TEXT("assetType"), TEXT("Material"));

		TArray<TSharedPtr<FJsonValue>> GraphsArray;
		GraphsArray.Add(MakeShareable(new FJsonValueObject(BuildMaterialGraph(Material))));
		Root->SetArrayField(TEXT("graphs"), GraphsArray);
		return Root;
	}

	// ---- UMaterialInstance -------------------------------------------------
	if (UMaterialInstance* MI = Cast<UMaterialInstance>(Asset))
	{
		TSharedPtr<FJsonObject> Root = MakeShareable(new FJsonObject);
		Root->SetStringField(TEXT("assetPath"), MI->GetPathName());
		Root->SetStringField(TEXT("assetType"), TEXT("MaterialInstance"));
		Root->SetStringField(TEXT("parent"),
			MI->Parent ? MI->Parent->GetPathName() : TEXT(""));

		// Scalar parameters (read directly from override arrays)
		TArray<TSharedPtr<FJsonValue>> ScalarsArray;
		for (const FScalarParameterValue& Param : MI->ScalarParameterValues)
		{
			TSharedPtr<FJsonObject> ParamObj = MakeShareable(new FJsonObject);
			ParamObj->SetStringField(TEXT("name"), Param.ParameterInfo.Name.ToString());
			ParamObj->SetNumberField(TEXT("value"), Param.ParameterValue);
			ScalarsArray.Add(MakeShareable(new FJsonValueObject(ParamObj)));
		}
		Root->SetArrayField(TEXT("scalarParameters"), ScalarsArray);

		// Vector parameters
		TArray<TSharedPtr<FJsonValue>> VectorsArray;
		for (const FVectorParameterValue& Param : MI->VectorParameterValues)
		{
			TSharedPtr<FJsonObject> ParamObj = MakeShareable(new FJsonObject);
			ParamObj->SetStringField(TEXT("name"), Param.ParameterInfo.Name.ToString());
			TSharedPtr<FJsonObject> ColorObj = MakeShareable(new FJsonObject);
			ColorObj->SetNumberField(TEXT("r"), Param.ParameterValue.R);
			ColorObj->SetNumberField(TEXT("g"), Param.ParameterValue.G);
			ColorObj->SetNumberField(TEXT("b"), Param.ParameterValue.B);
			ColorObj->SetNumberField(TEXT("a"), Param.ParameterValue.A);
			ParamObj->SetObjectField(TEXT("value"), ColorObj);
			VectorsArray.Add(MakeShareable(new FJsonValueObject(ParamObj)));
		}
		Root->SetArrayField(TEXT("vectorParameters"), VectorsArray);

		return Root;
	}

	return nullptr;
}
