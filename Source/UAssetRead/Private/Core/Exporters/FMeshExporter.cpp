// Copyright Epic Games, Inc. All Rights Reserved.

#include "Core/Exporters/FMeshExporter.h"

#include "Engine/StaticMesh.h"
#include "Engine/SkeletalMesh.h"
#include "Animation/Skeleton.h"
#include "PhysicsEngine/BodySetup.h"
#include "PhysicsEngine/AggregateGeom.h"
#include "StaticMeshResources.h"
#include "Rendering/SkeletalMeshRenderData.h"
#include "Rendering/SkeletalMeshLODRenderData.h"
#include "RawIndexBuffer.h"
#include "Engine/SkinnedAssetCommon.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static TSharedPtr<FJsonObject> BoxToJson(const FBox& Box)
{
	TSharedPtr<FJsonObject> Obj = MakeShareable(new FJsonObject);

	auto VecToJson = [](const FVector& V) -> TSharedPtr<FJsonObject>
	{
		TSharedPtr<FJsonObject> VObj = MakeShareable(new FJsonObject);
		VObj->SetNumberField(TEXT("x"), V.X);
		VObj->SetNumberField(TEXT("y"), V.Y);
		VObj->SetNumberField(TEXT("z"), V.Z);
		return VObj;
	};

	Obj->SetObjectField(TEXT("min"), VecToJson(Box.Min));
	Obj->SetObjectField(TEXT("max"), VecToJson(Box.Max));
	Obj->SetObjectField(TEXT("size"), VecToJson(Box.GetSize()));
	return Obj;
}

static FString CollisionTraceToString(ECollisionTraceFlag Flag)
{
	switch (Flag)
	{
	case CTF_UseSimpleAndComplex: return TEXT("UseSimpleAndComplex");
	case CTF_UseSimpleAsComplex: return TEXT("UseSimpleAsComplex");
	case CTF_UseComplexAsSimple: return TEXT("UseComplexAsSimple");
	default:                     return TEXT("UseDefault");
	}
}

static TSharedPtr<FJsonObject> ExportBodySetup(UBodySetup* BodySetup)
{
	TSharedPtr<FJsonObject> Obj = MakeShareable(new FJsonObject);
	if (!BodySetup)
	{
		Obj->SetBoolField(TEXT("hasSimpleCollision"), false);
		Obj->SetBoolField(TEXT("hasComplexCollision"), false);
		return Obj;
	}

	const FKAggregateGeom& Agg = BodySetup->AggGeom;
	const int32 PrimCount = Agg.BoxElems.Num() + Agg.SphereElems.Num()
	                      + Agg.SphylElems.Num() + Agg.ConvexElems.Num();

	Obj->SetBoolField(TEXT("hasSimpleCollision"), PrimCount > 0);
	Obj->SetBoolField(TEXT("hasComplexCollision"), true);
	Obj->SetStringField(TEXT("simpleCollisionType"),
		CollisionTraceToString(BodySetup->CollisionTraceFlag));

	TArray<TSharedPtr<FJsonValue>> PrimsArray;

	auto AddCenter = [](TSharedPtr<FJsonObject>& P, const FVector& C)
	{
		TSharedPtr<FJsonObject> CObj = MakeShareable(new FJsonObject);
		CObj->SetNumberField(TEXT("x"), C.X);
		CObj->SetNumberField(TEXT("y"), C.Y);
		CObj->SetNumberField(TEXT("z"), C.Z);
		P->SetObjectField(TEXT("center"), CObj);
	};

	for (const FKBoxElem& Box : Agg.BoxElems)
	{
		TSharedPtr<FJsonObject> P = MakeShareable(new FJsonObject);
		P->SetStringField(TEXT("type"), TEXT("Box"));
		AddCenter(P, Box.Center);
		TSharedPtr<FJsonObject> ExtentObj = MakeShareable(new FJsonObject);
		ExtentObj->SetNumberField(TEXT("x"), Box.X * 0.5f);
		ExtentObj->SetNumberField(TEXT("y"), Box.Y * 0.5f);
		ExtentObj->SetNumberField(TEXT("z"), Box.Z * 0.5f);
		P->SetObjectField(TEXT("extent"), ExtentObj);
		PrimsArray.Add(MakeShareable(new FJsonValueObject(P)));
	}
	for (const FKSphereElem& Sphere : Agg.SphereElems)
	{
		TSharedPtr<FJsonObject> P = MakeShareable(new FJsonObject);
		P->SetStringField(TEXT("type"), TEXT("Sphere"));
		AddCenter(P, Sphere.Center);
		P->SetNumberField(TEXT("radius"), Sphere.Radius);
		PrimsArray.Add(MakeShareable(new FJsonValueObject(P)));
	}
	for (const FKSphylElem& Capsule : Agg.SphylElems)
	{
		TSharedPtr<FJsonObject> P = MakeShareable(new FJsonObject);
		P->SetStringField(TEXT("type"), TEXT("Capsule"));
		AddCenter(P, Capsule.Center);
		P->SetNumberField(TEXT("radius"), Capsule.Radius);
		P->SetNumberField(TEXT("length"), Capsule.Length);
		PrimsArray.Add(MakeShareable(new FJsonValueObject(P)));
	}
	for (const FKConvexElem& Convex : Agg.ConvexElems)
	{
		TSharedPtr<FJsonObject> P = MakeShareable(new FJsonObject);
		P->SetStringField(TEXT("type"), TEXT("Convex"));
		P->SetNumberField(TEXT("vertexCount"), Convex.VertexData.Num());
		PrimsArray.Add(MakeShareable(new FJsonValueObject(P)));
	}

	Obj->SetArrayField(TEXT("collisionPrimitives"), PrimsArray);
	return Obj;
}

// ---------------------------------------------------------------------------
// FMeshExporter
// ---------------------------------------------------------------------------

TSharedPtr<FJsonObject> FMeshExporter::Export(UObject* Asset)
{
	// ---- UStaticMesh -------------------------------------------------------
	if (UStaticMesh* SM = Cast<UStaticMesh>(Asset))
	{
		TSharedPtr<FJsonObject> Root = MakeShareable(new FJsonObject);
		Root->SetStringField(TEXT("assetPath"), SM->GetPathName());
		Root->SetStringField(TEXT("assetType"), TEXT("StaticMesh"));

		FStaticMeshRenderData* RenderData = SM->GetRenderData();
		int32 TotalVerts = 0;
		int32 TotalTris  = 0;

		TArray<TSharedPtr<FJsonValue>> LODsArray;
		if (RenderData)
		{
			for (int32 LODIdx = 0; LODIdx < RenderData->LODResources.Num(); ++LODIdx)
			{
				const FStaticMeshLODResources& LOD = RenderData->LODResources[LODIdx];
				const int32 VertCount = LOD.GetNumVertices();
				const int32 TriCount  = LOD.IndexBuffer.GetNumIndices() / 3;

				if (LODIdx == 0)
				{
					TotalVerts = VertCount;
					TotalTris  = TriCount;
				}

				TSharedPtr<FJsonObject> LODObj = MakeShareable(new FJsonObject);
				LODObj->SetNumberField(TEXT("lod"), LODIdx);
				LODObj->SetNumberField(TEXT("vertexCount"), VertCount);
				LODObj->SetNumberField(TEXT("triangleCount"), TriCount);
				LODsArray.Add(MakeShareable(new FJsonValueObject(LODObj)));
			}
		}
		Root->SetNumberField(TEXT("vertexCount"), TotalVerts);
		Root->SetNumberField(TEXT("triangleCount"), TotalTris);
		Root->SetNumberField(TEXT("lodCount"), LODsArray.Num());
		Root->SetArrayField(TEXT("lods"), LODsArray);

		// Material slots
		TArray<TSharedPtr<FJsonValue>> MaterialsArray;
		for (int32 SlotIdx = 0; SlotIdx < SM->GetStaticMaterials().Num(); ++SlotIdx)
		{
			const FStaticMaterial& Mat = SM->GetStaticMaterials()[SlotIdx];
			TSharedPtr<FJsonObject> MatObj = MakeShareable(new FJsonObject);
			MatObj->SetNumberField(TEXT("index"), SlotIdx);
			MatObj->SetStringField(TEXT("name"), Mat.MaterialSlotName.ToString());
			MatObj->SetStringField(TEXT("materialPath"),
				Mat.MaterialInterface ? Mat.MaterialInterface->GetPathName() : TEXT(""));
			MaterialsArray.Add(MakeShareable(new FJsonValueObject(MatObj)));
		}
		Root->SetArrayField(TEXT("materialSlots"), MaterialsArray);

		// Bounding box
		Root->SetObjectField(TEXT("boundingBox"), BoxToJson(SM->GetBoundingBox()));

		// Collision
		Root->SetObjectField(TEXT("collision"), ExportBodySetup(SM->GetBodySetup()));

		return Root;
	}

	// ---- USkeletalMesh -----------------------------------------------------
	if (USkeletalMesh* SKM = Cast<USkeletalMesh>(Asset))
	{
		TSharedPtr<FJsonObject> Root = MakeShareable(new FJsonObject);
		Root->SetStringField(TEXT("assetPath"), SKM->GetPathName());
		Root->SetStringField(TEXT("assetType"), TEXT("SkeletalMesh"));

		FSkeletalMeshRenderData* RenderData = SKM->GetResourceForRendering();
		int32 TotalVerts = 0;
		int32 TotalTris  = 0;

		TArray<TSharedPtr<FJsonValue>> LODsArray;
		if (RenderData)
		{
			for (int32 LODIdx = 0; LODIdx < RenderData->LODRenderData.Num(); ++LODIdx)
			{
				const FSkeletalMeshLODRenderData& LOD = RenderData->LODRenderData[LODIdx];
				const int32 VertCount = LOD.GetNumVertices();
				const int32 TriCount  = LOD.MultiSizeIndexContainer.IsIndexBufferValid()
				                      ? LOD.MultiSizeIndexContainer.GetIndexBuffer()->Num() / 3
				                      : 0;

				if (LODIdx == 0)
				{
					TotalVerts = VertCount;
					TotalTris  = TriCount;
				}

				TSharedPtr<FJsonObject> LODObj = MakeShareable(new FJsonObject);
				LODObj->SetNumberField(TEXT("lod"), LODIdx);
				LODObj->SetNumberField(TEXT("vertexCount"), VertCount);
				LODObj->SetNumberField(TEXT("triangleCount"), TriCount);
				LODsArray.Add(MakeShareable(new FJsonValueObject(LODObj)));
			}
		}
		Root->SetNumberField(TEXT("vertexCount"), TotalVerts);
		Root->SetNumberField(TEXT("triangleCount"), TotalTris);
		Root->SetNumberField(TEXT("lodCount"), LODsArray.Num());
		Root->SetArrayField(TEXT("lods"), LODsArray);

		// Material slots
		TArray<TSharedPtr<FJsonValue>> MaterialsArray;
		const TArray<FSkeletalMaterial>& SkelMaterials = SKM->GetMaterials();
		for (int32 SlotIdx = 0; SlotIdx < SkelMaterials.Num(); ++SlotIdx)
		{
			const FSkeletalMaterial& Mat = SkelMaterials[SlotIdx];
			TSharedPtr<FJsonObject> MatObj = MakeShareable(new FJsonObject);
			MatObj->SetNumberField(TEXT("index"), SlotIdx);
			MatObj->SetStringField(TEXT("name"), Mat.MaterialSlotName.ToString());
			MatObj->SetStringField(TEXT("materialPath"),
				Mat.MaterialInterface ? Mat.MaterialInterface->GetPathName() : TEXT(""));
			MaterialsArray.Add(MakeShareable(new FJsonValueObject(MatObj)));
		}
		Root->SetArrayField(TEXT("materialSlots"), MaterialsArray);

		// Bounding box from imported bounds
		Root->SetObjectField(TEXT("boundingBox"), BoxToJson(SKM->GetImportedBounds().GetBox()));

		// Skeleton reference
		USkeleton* Skeleton = SKM->GetSkeleton();
		Root->SetStringField(TEXT("skeletonRef"), Skeleton ? Skeleton->GetPathName() : TEXT(""));

		// Collision (physics asset)
		Root->SetObjectField(TEXT("collision"), ExportBodySetup(nullptr));

		return Root;
	}

	return nullptr;
}
