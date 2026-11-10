#include "T5SkeletalMesh.h"
#include "T5SkinnedComponent.h"

#include "Animation/Skeleton.h"
#include "Animation/MorphTarget.h"
#include "RHI.h"
#include "Components/SkeletalMeshComponent.h"
#include "Dom/JsonObject.h"
#include "Engine/SkeletalMesh.h"
#include "GameFramework/Actor.h"
#include "Rendering/SkeletalMeshRenderData.h"

namespace
{
    constexpr int32 InfluenceCount = 8;
    struct FSourceSkin
    {
        uint16 Bones[InfluenceCount];
        float Weights[InfluenceCount];
    };
    static_assert(sizeof(FSourceSkin) == 48);
    static_assert(InfluenceCount <= MAX_TOTAL_INFLUENCES);

    bool ReadTransform(const TArray<TSharedPtr<FJsonValue>>& Values, FTransform& Out)
    {
        if (Values.Num() != 16)
            return false;
        FMatrix Matrix = FMatrix::Identity;
        for (int32 R = 0; R < 4; ++R)
            for (int32 C = 0; C < 4; ++C)
            {
                const double Value = Values[T5AxisForUnreal(R) * 4 + T5AxisForUnreal(C)]->AsNumber();
                if (!FMath::IsFinite(Value))
                    return false;
                Matrix.M[R][C] = Value * (R == 3 && C < 3 ? T5CentimetersPerUnit : 1.0);
            }
        Out = FTransform(Matrix);
        return !Out.ContainsNaN();
    }

    bool ReadSkeleton(USkeletalMesh& Mesh, const TArray<TSharedPtr<FJsonValue>>& Joints)
    {
        if (Joints.IsEmpty() || Joints.Num() > MAX_uint16)
            return false;
        FReferenceSkeleton Ref;
        {
            FReferenceSkeletonModifier Modifier(Ref, nullptr);
            for (int32 I = 0; I < Joints.Num(); ++I)
            {
                const auto& Row = Joints[I]->AsArray();
                if (Row.Num() != 3)
                    return false;
                const FString Name = Row[0]->AsString();
                const FName Bone(*Name);
                const int32 Parent = Row[1]->AsNumber();
                FTransform Local;
                if (Bone.IsNone() || Ref.FindRawBoneIndex(Bone) != INDEX_NONE
                    || (I == 0 ? Parent != INDEX_NONE : Parent < 0 || Parent >= I)
                    || !ReadTransform(Row[2]->AsArray(), Local))
                    return false;
                Modifier.Add(FMeshBoneInfo(Bone, Name, Parent), Local);
            }
        }
        Mesh.SetRefSkeleton(Ref);
        Mesh.CalculateInvRefMatrices();
        auto* Skeleton = NewObject<USkeleton>(&Mesh, NAME_None, RF_Transient | RF_DuplicateTransient);
        if (!Skeleton->MergeAllBonesToBoneTree(&Mesh, false))
            return false;
        Mesh.SetSkeleton(Skeleton);
        return true;
    }

    bool AddSkinSection(const FT5SkinSection& Source, TConstArrayView64<uint8> Geometry, TConstArrayView64<uint8> Skin,
        int32 BoneCount, TArray<FStaticMeshBuildVertex>& Vertices, TArray<uint32>& Indices,
        TArray<FSkinWeightInfo>& Weights, FSkelMeshRenderSection& Section)
    {
        const int32 NumVertices = Source.Data->GetIntegerField(TEXT("vertices"));
        const int32 NumIndices = Source.Data->GetIntegerField(TEXT("indices"));
        const int64 Offset = Source.Data->GetNumberField(TEXT("offset"));
        const int64 SkinOffset = Source.Data->GetNumberField(TEXT("skinOffset"));
        const int64 VertexBytes = int64(NumVertices) * int64(sizeof(FT5SourceVertex));
        if (!Source.Material || NumVertices <= 0 || NumIndices <= 0 || NumIndices % 3 || Offset < 0 || SkinOffset < 0
            || Offset > Geometry.Num() || VertexBytes + int64(NumIndices) * int64(sizeof(uint32)) > Geometry.Num() - Offset
            || SkinOffset > Skin.Num() || int64(NumVertices) * int64(sizeof(FSourceSkin)) > Skin.Num() - SkinOffset)
            return false;

        Section.BaseVertexIndex = Vertices.Num();
        Section.BaseIndex = Indices.Num();
        Section.NumVertices = NumVertices;
        Section.NumTriangles = NumIndices / 3;
        Section.bVisibleInRayTracing = false;
        TMap<uint16, uint16> LocalBones;
        for (int32 I = 0; I < NumVertices; ++I)
        {
            FT5SourceVertex Raw;
            FMemory::Memcpy(&Raw, Geometry.GetData() + Offset + int64(I) * sizeof(Raw), sizeof(Raw));
            auto& Vertex = Vertices.AddDefaulted_GetRef();
            Vertex.Position = T5ToUnrealPosition(Raw.Position);
            Vertex.TangentZ = T5ToUnrealDirection(Raw.Normal).GetSafeNormal(UE_SMALL_NUMBER, FVector3f::UpVector);
            Vertex.UVs[0] = Raw.UV;
            Vertex.Color = FLinearColor(Raw.Color.X, Raw.Color.Y, Raw.Color.Z, Raw.Color.W).ToFColor(false);

            FSourceSkin SourceWeights;
            FMemory::Memcpy(&SourceWeights, Skin.GetData() + SkinOffset + int64(I) * sizeof(SourceWeights), sizeof(SourceWeights));
            auto& Weight = Weights.AddZeroed_GetRef();
            double Total = 0;
            for (int32 Influence = 0; Influence < InfluenceCount; ++Influence)
            {
                const float Value = SourceWeights.Weights[Influence];
                if (!FMath::IsFinite(Value) || Value < 0 || (Value > 0 && SourceWeights.Bones[Influence] >= BoneCount))
                    return false;
                Total += Value;
            }
            if (Total <= 0)
                return false;
            int32 Remaining = MAX_uint16;
            for (int32 Influence = 0; Influence < InfluenceCount; ++Influence)
            {
                const float Value = SourceWeights.Weights[Influence];
                if (Value == 0)
                    continue;
                const uint16 Bone = SourceWeights.Bones[Influence];
                const uint16* Local = LocalBones.Find(Bone);
                const int32 LocalIndex = Local ? *Local : Section.BoneMap.Add(Bone);
                if (LocalIndex > MAX_uint8)
                    return false;
                LocalBones.Add(Bone, LocalIndex);
                Weight.InfluenceBones[Influence] = LocalIndex;
                const int32 Quantized = FMath::Min(Remaining, FMath::RoundToInt(Value / Total * MAX_uint16));
                Weight.InfluenceWeights[Influence] = Quantized;
                Remaining -= Quantized;
                Section.MaxBoneInfluences = FMath::Max(Section.MaxBoneInfluences, Influence + 1);
            }
            Weight.InfluenceWeights[0] += Remaining;
        }
        for (int32 I = 0; I < NumIndices; ++I)
        {
            uint32 Index;
            FMemory::Memcpy(&Index, Geometry.GetData() + Offset + VertexBytes + int64(I) * sizeof(Index), sizeof(Index));
            if (Index >= static_cast<uint32>(NumVertices))
                return false;
            Indices.Add(Section.BaseVertexIndex + Index);
        }
        return true;
    }

    void BuildTangents(TArray<FStaticMeshBuildVertex>& Vertices, const TArray<uint32>& Indices)
    {
        TArray<FVector3f> Bitangents;
        Bitangents.Init(FVector3f::ZeroVector, Vertices.Num());
        for (auto& V : Vertices)
            V.TangentX = FVector3f::ZeroVector;
        for (int32 I = 0; I < Indices.Num(); I += 3)
        {
            auto& A = Vertices[Indices[I]];
            auto& B = Vertices[Indices[I + 1]];
            auto& C = Vertices[Indices[I + 2]];
            const FVector2f U = B.UVs[0] - A.UVs[0], V = C.UVs[0] - A.UVs[0];
            const float Det = U.X * V.Y - U.Y * V.X;
            if (FMath::IsNearlyZero(Det))
                continue;
            const FVector3f E = B.Position - A.Position, F = C.Position - A.Position;
            const FVector3f X = (E * V.Y - F * U.Y) / Det, Y = (F * U.X - E * V.X) / Det;
            for (int32 Corner = 0; Corner < 3; ++Corner)
            {
                Vertices[Indices[I + Corner]].TangentX += X;
                Bitangents[Indices[I + Corner]] += Y;
            }
        }
        for (int32 I = 0; I < Vertices.Num(); ++I)
        {
            auto& V = Vertices[I];
            V.TangentX = (V.TangentX - V.TangentZ * (V.TangentZ | V.TangentX)).GetSafeNormal();
            if (V.TangentX.IsNearlyZero())
                V.TangentZ.FindBestAxisVectors(V.TangentX, V.TangentY);
            else
            {
                V.TangentY = V.TangentZ ^ V.TangentX;
                if ((V.TangentY | Bitangents[I]) < 0)
                    V.TangentY *= -1;
            }
        }
    }
}

USkeletalMeshComponent* BuildT5SkeletalMesh(AActor& Owner, USceneComponent& Root,
    const TArray<TSharedPtr<FJsonValue>>& Joints, TConstArrayView<FT5SkinSection> Sections,
    TConstArrayView64<uint8> Geometry, TConstArrayView64<uint8> Skin, TConstArrayView64<uint8> Morphs)
{
    if (Sections.IsEmpty() || Sections.Num() > MAX_uint16)
        return nullptr;
    FTransform Placement;
    if (!ReadTransform(Sections[0].Data->GetArrayField(TEXT("placement")), Placement))
        return nullptr;

    constexpr EObjectFlags Flags = RF_Transient | RF_DuplicateTransient;
    auto* Mesh = NewObject<USkeletalMesh>(&Owner, NAME_None, Flags);
    if (!ReadSkeleton(*Mesh, Joints))
        return nullptr;
    Mesh->AllocateResourceForRendering();
    Mesh->AddLODInfo().ScreenSize = 1.0f;
    Mesh->NeverStream = true;
    Mesh->SetHasVertexColors(true);
    Mesh->SetSupportRayTracing(false);
    auto& Render = *Mesh->GetResourceForRendering();
    Render.NumInlinedLODs = Render.NumNonOptionalLODs = 1;
    auto& LOD = *new FSkeletalMeshLODRenderData;
    Render.LODRenderData.Add(&LOD);
    const int32 BoneCount = Mesh->GetRefSkeleton().GetNum();
    TArray<FStaticMeshBuildVertex> Vertices;
    TArray<uint32> Indices;
    TArray<FSkinWeightInfo> Weights;
    TArray<FSkeletalMaterial> Materials;
    int32 MaxInfluences = 4;
    for (const auto& Source : Sections)
    {
        auto& Section = LOD.RenderSections.AddDefaulted_GetRef();
        Section.MaterialIndex = Materials.Num();
        if (!AddSkinSection(Source, Geometry, Skin, BoneCount, Vertices, Indices, Weights, Section))
            return nullptr;
        MaxInfluences = FMath::Max(MaxInfluences, Section.MaxBoneInfluences);
        const FName Slot(*FString::Printf(TEXT("Section%d"), Section.MaterialIndex));
        Materials.Emplace(Source.Material, Slot);
        Materials.Last().UVChannelData = FMeshUVChannelInfo(1.0f);
    }
    for (int32 I = 0; I < BoneCount; ++I)
        LOD.RequiredBones.Add(I);
    LOD.ActiveBoneIndices = LOD.RequiredBones;
    BuildTangents(Vertices, Indices);
    LOD.StaticVertexBuffers.PositionVertexBuffer.Init(Vertices, true);
    LOD.StaticVertexBuffers.StaticMeshVertexBuffer.Init(Vertices, 1, true);
    LOD.StaticVertexBuffers.ColorVertexBuffer.Init(Vertices, true);
    LOD.SkinWeightVertexBuffer.SetNeedsCPUAccess(true);
    LOD.SkinWeightVertexBuffer.SetMaxBoneInfluences(MaxInfluences);
    LOD.SkinWeightVertexBuffer.SetUse16BitBoneWeight(true);
    LOD.SkinWeightVertexBuffer = Weights;
    LOD.MultiSizeIndexContainer.CreateIndexBuffer(sizeof(uint32));
    LOD.MultiSizeIndexContainer.CopyIndexBuffer(Indices);
    Mesh->SetMaterials(Materials);
    FBox Bounds(ForceInit);
    for (const auto& Vertex : Vertices)
        Bounds += FVector(Vertex.Position);
    Mesh->SetImportedBounds(FBoxSphereBounds(Bounds));
    TMap<FName, UMorphTarget*> Targets;
    TMap<FName, FVector> MorphExtents;
    for (int32 S = 0; S < Sections.Num(); ++S)
    {
        const TArray<TSharedPtr<FJsonValue>>* Expressions = nullptr;
        if (!Sections[S].Data->TryGetArrayField(TEXT("expressions"), Expressions))
            continue;
        auto& Section = LOD.RenderSections[S];
        Section.bRecomputeTangent = true;
        for (const auto& Value : *Expressions)
        {
            const auto& Entry = Value->AsArray();
            const int64 Offset = Entry[1]->AsNumber();
            if (Offset < 0 || Offset > Morphs.Num() || int64(Section.NumVertices)*12 > Morphs.Num()-Offset)
                return nullptr;
            const FName Name(*Entry[0]->AsString());
            auto*& Target = Targets.FindOrAdd(Name);
            if (!Target)
            {
                Target = NewObject<UMorphTarget>(Mesh, Name, Flags);
                Target->GetMorphLODModels().AddDefaulted();
            }
            auto& Model = Target->GetMorphLODModels()[0];
            Model.NumBaseMeshVerts = Vertices.Num();
            Model.SectionIndices.AddUnique(S);
            for (uint32 V = 0; V < Section.NumVertices; ++V)
            {
                FVector3f Raw;
                FMemory::Memcpy(&Raw, Morphs.GetData()+Offset+int64(V)*12, 12);
                const auto Delta = T5ToUnrealPosition(Raw);
                auto& Extent = MorphExtents.FindOrAdd(Name);
                Extent = Extent.ComponentMax(FVector(Delta.GetAbs()));
                if (!Delta.IsNearlyZero())
                {
                    auto& D = Model.Vertices.AddDefaulted_GetRef();
                    D.SourceIdx = Section.BaseVertexIndex+V;
                    D.PositionDelta = Delta;
                }
            }
            Model.NumVertices = Model.Vertices.Num();
        }
    }
    TArray<const FMorphTargetLODModel*> MorphModels;
    for (const auto& [Name, Target] : Targets)
        if (Target->HasValidData())
            Mesh->RegisterMorphTarget(Target, false);
    Mesh->InitMorphTargets();
    for (const auto& Target : Mesh->GetMorphTargets())
        MorphModels.Add(&Target->GetMorphLODModels()[0]);
    if (!MorphModels.IsEmpty())
        LOD.MorphTargetVertexInfoBuffers.InitMorphResourcesStreaming(GMaxRHIShaderPlatform,
            LOD.RenderSections, MorphModels, Vertices.Num(), .01f);
    Mesh->InitResources();

    auto* Component = NewObject<UT5SkinnedComponent>(&Owner, NAME_None, Flags);
    Component->BoneLocalBounds.Init(FBox(ForceInit), BoneCount);
    // Conservative bound for concurrent phonemes, expressions and modifiers.
    for (const auto& [Name, Extent] : MorphExtents) Component->MorphBoundsExpansion += Extent;
    const auto& InverseBind = Mesh->GetRefBasesInvMatrix();
    for (const auto& Section : LOD.RenderSections)
        for (uint32 I = Section.BaseVertexIndex; I < Section.BaseVertexIndex + Section.NumVertices; ++I)
            for (int32 Influence = 0; Influence < MaxInfluences; ++Influence)
                if (Weights[I].InfluenceWeights[Influence])
                {
                    const int32 Bone = Section.BoneMap[Weights[I].InfluenceBones[Influence]];
                    Component->BoneLocalBounds[Bone] += FVector(InverseBind[Bone].TransformPosition(Vertices[I].Position));
                }
    Component->SetupAttachment(&Root);
    Component->SetRelativeTransform(Placement);
    Component->SetSkeletalMesh(Mesh);
    Component->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    Component->RegisterComponent();
    return Component;
}

