#include "T5Mechanics.h"
#include "PhysicalMaterials/PhysicalMaterial.h"
#include "T5Articulation.h"

#include "T5Bridge.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "T5Coordinates.h"
#include "T5Hkx.h"
#include "T5HkxAnimInstance.h"
#include "T5SkinnedComponent.h"
#include "T5WeatherFogComponent.h"
#include "Chaos/Convex.h"
#include "Chaos/TriangleMeshImplicitObject.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/ExponentialHeightFogComponent.h"
#include "Components/PostProcessComponent.h"
#include "Components/SkyLightComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Dom/JsonObject.h"
#include "Engine/DirectionalLight.h"
#include "Engine/SkyLight.h"
#include "Engine/TextureCube.h"
#include "Engine/StaticMesh.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/World.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "EngineUtils.h"
#include "UObject/UObjectIterator.h"
#include "PhysicsEngine/BodySetup.h"
#include "PhysicsEngine/PhysicsAsset.h"
#include "PhysicsEngine/PhysicsConstraintTemplate.h"
#include "PhysicsEngine/SkeletalBodySetup.h"
#include "UObject/StrongObjectPtr.h"

DEFINE_LOG_CATEGORY_STATIC(LogT5Mechanics, Log, All);

struct FT5RagdollRig
{
    TStrongObjectPtr<UPhysicsAsset> Asset;
    TArray<FName> Bones;
    TMap<FName, FName> Parents;
};

namespace
{
    FVector Position(const TArray<TSharedPtr<FJsonValue>>& Row)
    {
        return T5ToUnrealPosition(FVector(Row[0]->AsNumber(), Row[1]->AsNumber(), Row[2]->AsNumber()));
    }

    FTransform Transform(const TArray<TSharedPtr<FJsonValue>>& Row)
    {
        return FTransform(T5ToUnrealRotation(FQuat(Row[3]->AsNumber(), Row[4]->AsNumber(),
            Row[5]->AsNumber(), Row[6]->AsNumber())), Position(Row), FVector(Row[7]->AsNumber()));
    }

    enum class EContact { Solid, Pick, Camera };

    void AddHulls(UBodySetup& Setup, const TArray<TSharedPtr<FJsonValue>>& Hulls)
    {
        for (const auto& Hull : Hulls)
        {
            TArray<Chaos::FConvex::FVec3Type> Vertices;
            for (const auto& Point : Hull->AsArray())
            {
                const FVector Vertex = Position(Point->AsArray());
                Vertices.Emplace(float(Vertex.X), float(Vertex.Y), float(Vertex.Z));
            }
            if (Vertices.Num() < 4)
                continue;
            auto Convex = Chaos::FConvexPtr(new Chaos::FConvex(Vertices, 0));
            if (Convex->NumVertices() < 4)
                continue;
            auto& Element = Setup.AggGeom.ConvexElems.AddDefaulted_GetRef();
            for (const auto& Vertex : Vertices)
                Element.VertexData.Add(FVector(Vertex));
            Element.UpdateElemBox();
            Element.SetConvexMeshObject(MoveTemp(Convex));
        }
    }

    void AddRoundShapes(UBodySetup& Setup, const TArray<TSharedPtr<FJsonValue>>& Shapes)
    {
        for (const auto& Shape : Shapes)
        {
            const auto& Row = Shape->AsArray();
            const auto A = Position(Row[0]->AsArray());
            const auto B = Position(Row[1]->AsArray());
            const float RadiusA = Row[2]->AsNumber() * T5CentimetersPerUnit;
            const float RadiusB = Row[3]->AsNumber() * T5CentimetersPerUnit;
            const double Length = FVector::Distance(A, B);
            if (Length < UE_SMALL_NUMBER)
            {
                auto& Sphere = Setup.AggGeom.SphereElems.Emplace_GetRef(FMath::Max(RadiusA, RadiusB));
                Sphere.Center = A;
                continue;
            }
            const auto Rotation = FRotationMatrix::MakeFromZ(B - A).Rotator();
            if (FMath::IsNearlyEqual(RadiusA, RadiusB))
            {
                auto& Capsule = Setup.AggGeom.SphylElems.Emplace_GetRef(RadiusA, Length);
                Capsule.Center = (A + B) * .5;
                Capsule.Rotation = Rotation;
            }
            else
            {
                auto& Capsule = Setup.AggGeom.TaperedCapsuleElems.Emplace_GetRef(RadiusA, RadiusB, Length);
                Capsule.Center = (A + B) * .5;
                Capsule.Rotation = Rotation;
            }
        }
    }

    FBox AddTriangles(UBodySetup& Setup, const FJsonObject& Data)
    {
        const auto& Vertices = Data.GetArrayField(TEXT("vertices"));
        FBox Bounds(ForceInit);
        Chaos::FTriangleMeshImplicitObject::ParticlesType Points;
        Points.AddParticles(Vertices.Num());
        for (int32 Index = 0; Index < Vertices.Num(); ++Index)
        {
            const auto Point = Position(Vertices[Index]->AsArray());
            Bounds += Point;
            Points.SetX(Index, Chaos::FVec3f(Point));
        }
        TArray<Chaos::TVec3<int32>> Faces;
        for (const auto& Value : Data.GetArrayField(TEXT("faces")))
        {
            const auto& Row = Value->AsArray();
            const int32 A = Row[0]->AsNumber(), B = Row[1]->AsNumber(), C = Row[2]->AsNumber();
            if (Vertices.IsValidIndex(A) && Vertices.IsValidIndex(B) && Vertices.IsValidIndex(C)
                && !FVector3f::CrossProduct(Points.GetX(B) - Points.GetX(A), Points.GetX(C) - Points.GetX(A)).IsNearlyZero())
                // This is a direct Chaos mesh, bypassing UE's bFlipNormals cooker.
                // The T5 -> UE reflection reverses orientation: restore the outward
                // cross-product normal for contacts, sweeps and one-sided triangles.
                Faces.Emplace(B, A, C);
        }
        if (!Faces.IsEmpty())
            Setup.TriMeshGeometries.Emplace(new Chaos::FTriangleMeshImplicitObject(MoveTemp(Points), MoveTemp(Faces), TArray<uint16>{}));
        return Bounds;
    }

    void SetCollisionBounds(UStaticMesh& Mesh, const FBox& Bounds)
    {
        auto* Setup = Mesh.GetBodySetup();
        Setup->bCreatedPhysicsMeshes = true;
        Mesh.SetPositiveBoundsExtension(Bounds.Max.ComponentMax(FVector::ZeroVector));
        Mesh.SetNegativeBoundsExtension((-Bounds.Min).ComponentMax(FVector::ZeroVector));
    }

    UStaticMeshComponent* CollisionComponent(AActor& Owner, UStaticMesh& Mesh, USceneComponent& Parent, FName Bone,
        const FTransform& Pose, EContact Contact, EComponentMobility::Type Mobility, bool bEnabled)
    {
        auto* Component = NewObject<UStaticMeshComponent>(&Owner, NAME_None, RF_Transient);
        Component->SetupAttachment(&Parent, Bone);
        Component->SetMobility(Mobility);
        Component->SetRelativeTransform(Pose);
        Component->SetStaticMesh(&Mesh);
        Component->SetCollisionProfileName(Mobility == EComponentMobility::Movable ? TEXT("BlockAllDynamic") : TEXT("BlockAll"));
        if (Contact != EContact::Solid)
        {
            Component->SetCollisionResponseToAllChannels(ECR_Ignore);
            Component->SetCollisionResponseToChannel(Contact == EContact::Pick ? ECC_Visibility : ECC_Camera,
                Contact == EContact::Pick ? ECR_Overlap : ECR_Block);
        }
        Component->SetCollisionEnabled(!bEnabled ? ECollisionEnabled::NoCollision
            : Contact == EContact::Solid ? ECollisionEnabled::QueryAndPhysics : ECollisionEnabled::QueryOnly);
        Component->SetGenerateOverlapEvents(false);
        Component->ComponentTags.Add(TEXT("T5NifCollision"));
        Component->SetHiddenInGame(true);
        Component->RegisterComponent();
        return Component;
    }

    UStaticMeshComponent* WheelBody(AActor& Owner, const TArray<TSharedPtr<FJsonValue>>& Hulls)
    {
        auto* Mesh = NewObject<UStaticMesh>(&Owner, NAME_None, RF_Transient);
        Mesh->CreateBodySetup();
        auto& Setup = *Mesh->GetBodySetup();
        Setup.CollisionTraceFlag = CTF_UseSimpleAsComplex;
        AddHulls(Setup, Hulls);
        if (Setup.AggGeom.ConvexElems.IsEmpty())
            return nullptr;
        SetCollisionBounds(*Mesh, Setup.AggGeom.CalcAABB(FTransform::Identity));
        auto* Component=CollisionComponent(Owner, *Mesh, *Owner.GetRootComponent(), NAME_None, FTransform::Identity,
            EContact::Solid, EComponentMobility::Movable, false);
        // ComponentSweep needs a live query shape. NoCollision removes the Chaos
        // actor, silently making the initial wheel-support sweep return no hits.
        // QueryOnly keeps the authored hull available without duplicate contact forces.
        Component->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
        return Component;
    }

    void UpdateBody(UStaticMeshComponent* Body, const FTransform& Pose, bool bEnabled)
    {
        if (!Body)
            return;
        Body->SetRelativeTransform(Pose, false, nullptr, ETeleportType::TeleportPhysics);
        Body->SetCollisionEnabled(bEnabled ? ECollisionEnabled::QueryAndPhysics : ECollisionEnabled::NoCollision);
    }

    FTransform Sample(const TArray<FTransform>& Frames, double Phase)
    {
        const double Position = FMath::Clamp(Phase, 0., 1.) * (Frames.Num() - 1);
        const int32 Index = FMath::FloorToInt(Position);
        if (Index == Frames.Num() - 1)
            return Frames.Last();
        FTransform Pose;
        Pose.Blend(Frames[Index], Frames[Index + 1], Position - Index);
        return Pose;
    }
}

FT5Mechanics::~FT5Mechanics()
{
    if (auto* Volume = WeatherFogVolume.Get()) Volume->DestroyComponent();
    if (auto* Owner = CollisionOwner.Get())
        Owner->Destroy();
}

void FT5Mechanics::SetActive(bool bActive)
{
    if (auto* Owner = CollisionOwner.Get())
        Owner->SetActorEnableCollision(bActive);
}

void FT5Mechanics::SetPaused(bool bPaused)
{
    bSimulationPaused=bPaused;
    for (auto& [Id,Cart] : Carts) if (Cart.Physics) Cart.Physics->SetPaused(bPaused);
}

void FT5Mechanics::Load(AActor& Owner, const FJsonObject& Models)
{
    if (Models.Values.IsEmpty())
        return;
    // Chaos component sweeps exclude the source actor, so wheel bodies and
    // streamed road collision need distinct owners.
    FActorSpawnParameters Spawn;
    Spawn.ObjectFlags = RF_Transient;
    Spawn.Owner = &Owner;
    auto* Bodies = Owner.GetWorld()->SpawnActor<AActor>(AActor::StaticClass(), Owner.GetActorTransform(), Spawn);
    if (!Bodies)
        return;
    CollisionOwner = Bodies;
    auto* Root = NewObject<USceneComponent>(Bodies, NAME_None, RF_Transient);
    Bodies->SetRootComponent(Root);
    Root->RegisterComponent();
    Root->SetWorldTransform(Owner.GetActorTransform());
    const auto Add = [&](const TArray<TSharedPtr<FJsonValue>>& Hulls)
    {
        auto* Component = WheelBody(*Bodies, Hulls);
        if (Component)
            CollisionParts.Add(Component);
        return Component;
    };
    for (const auto& [Key, Value] : Models.Values)
    {
        const uint32 Id = FCString::Strtoui64(*Key, nullptr, 10);
        const auto& Object = Value->AsObject();
        const auto& Model = Object->GetObjectField(TEXT("model"));
        const double Scale = Object->GetNumberField(TEXT("scale"));
        const TArray<TSharedPtr<FJsonValue>>* Wheels = nullptr;
        if (Model->TryGetArrayField(TEXT("wheels"), Wheels))
        {
            // The bundle also uses this field for jointed clutter with no wheels.
            if (Wheels->IsEmpty()) continue;
            auto& Cart = Carts.Add(Id);
            if (Model->HasField(TEXT("bodies")))
            {
                Cart.Physics=MakeShared<FT5Articulation>();
                Cart.Physics->Configure(DataPath,PluginPath);
                if (!Cart.Physics->Load(*Bodies,*Model,Scale,Add))
                { UE_LOG(LogT5Mechanics,Error,TEXT("Cannot construct NIF articulation for %08X"),Id); Cart.Physics.Reset(); }
            }
            Cart.Chassis.Rest.SetScale3D(FVector(Scale));
            Cart.Chassis.Collision = Add(Model->GetArrayField(TEXT("chassis")));
            for (const auto& ValueWheel : *Wheels)
            {
                const auto& Wheel = ValueWheel->AsObject();
                auto& Part = Cart.Wheels.AddDefaulted_GetRef();
                Wheel->TryGetStringField(TEXT("node"),Part.Node);
                Part.Rest = Transform(Wheel->GetArrayField(TEXT("rest"))) * Cart.Chassis.Rest;
                Part.Collision = Add(Wheel->GetArrayField(TEXT("hulls")));
            }
            continue;
        }
        const TSharedPtr<FJsonObject>* FoundSequences = nullptr;
        if (!Model->TryGetObjectField(TEXT("sequences"), FoundSequences)
            && !Model->TryGetObjectField(TEXT("doors"), FoundSequences))
            continue;
        const auto& Sequences = *FoundSequences;
        const FTransform RefScale(FQuat::Identity, FVector::ZeroVector, FVector(Scale));
        auto& Playback = ModelsByReference.FindOrAdd(Id);
        for (const auto& [Name, Entry] : Sequences->Values)
        {
            const auto& Sequence = Entry->AsObject();
            auto& Definition = Playback.Sequences.Add(FString(*Name));
            Definition.Seconds = Sequence->GetNumberField(TEXT("seconds"));
            for (const auto& ValueTrack : Sequence->GetArrayField(TEXT("tracks")))
            {
                const auto& Track = ValueTrack->AsObject();
                auto& Part = Definition.Tracks.Add(Track->GetStringField(TEXT("node")));
                Part.Rest = Transform(Track->GetArrayField(TEXT("rest"))) * RefScale;
                for (const auto& Frame : Track->GetArrayField(TEXT("frames")))
                    Part.Frames.Add(Transform(Frame->AsArray()) * RefScale);
            }
        }
        if (!Playback.Sequences.Contains(TEXT("open")) || !Playback.Sequences.Contains(TEXT("close")))
            continue;
        auto& Door = Doors.Add(Id);
        for (const bool bOpen : {false, true})
        {
            const auto& Sequence = Sequences->GetObjectField(bOpen ? TEXT("open") : TEXT("close"));
            (bOpen ? Door.OpenSeconds : Door.CloseSeconds) = Sequence->GetNumberField(TEXT("seconds"));
            for (const auto& ValueTrack : Sequence->GetArrayField(TEXT("tracks")))
            {
                const auto& Track = ValueTrack->AsObject();
                auto& Part = Door.Parts.FindOrAdd(Track->GetStringField(TEXT("node")));
                Part.Rest = Transform(Track->GetArrayField(TEXT("rest"))) * RefScale;
                Part.Pose = Part.Rest;
                auto& Frames = bOpen ? Part.Open : Part.Closed;
                for (const auto& Frame : Track->GetArrayField(TEXT("frames")))
                    Frames.Add(Transform(Frame->AsArray()) * RefScale);
            }
        }
    }
}

void FT5Mechanics::LoadCollisions(AActor& Owner, const FJsonObject& Manifest,
    const TMultiMap<uint32, USkeletalMeshComponent*>& Skeletons,
    TFunctionRef<void(uint32, UStaticMeshComponent*, const FString&)> Register)
{
    for (const auto& [Id, Mesh] : Skeletons)
    {
        // Quadruped support points come from the original rig, not fabricated capsules.
        if (Mesh->GetBoneIndex(TEXT("SaddleBone")) == INDEX_NONE)
            continue;
        auto& Actor = GroundActors.FindOrAdd(Id);
        if (Actor.Mesh.IsValid())
            continue;
        Actor.Mesh = Mesh;
        Actor.Original = Mesh->GetRelativeTransform();
        const auto& Ref = Mesh->GetSkeletalMeshAsset()->GetRefSkeleton();
        TArray<FTransform> Rest;
        for (int32 I = 0; I < Ref.GetNum(); ++I)
        {
            const int32 Parent = Ref.GetParentIndex(I);
            Rest.Add(Parent == INDEX_NONE ? Ref.GetRefBonePose()[I] : Ref.GetRefBonePose()[I] * Rest[Parent]);
            if (Ref.GetBoneName(I).ToString().EndsWith(TEXT("Hoof")))
            {
                FVector Point = Rest.Last().GetLocation();
                const auto* Skin = Cast<UT5SkinnedComponent>(Mesh);
                const FBox Hoof = Skin && Skin->BoneLocalBounds.IsValidIndex(I) && Skin->BoneLocalBounds[I].IsValid
                    ? Skin->BoneLocalBounds[I].TransformBy(Rest.Last())
                    : Mesh->GetSkeletalMeshAsset()->GetImportedBounds().GetBox();
                Point.Z = Hoof.Min.Z;
                Actor.Feet.Add(Point);
            }
        }
    }
    struct FShape
    {
        UStaticMesh* Mesh;
        FString Motion;
        FName Bone;
        EContact Contact;
        bool bCartHull;
    };
    TMap<FString, TArray<FShape>> Cache;
    const auto& Models = Manifest.GetObjectField(TEXT("models"));
    int32 TriangleMeshes = 0, SimpleBodies = 0, BoneBodies = 0, Instances = 0;
    for (const auto& [Path, Model] : Models->Values)
    {
        auto& Parts = Cache.Add(FString(Path));
        for (const auto& Part : Model->AsArray())
        {
            const auto& Data = Part->AsObject();
            const auto Motion = Data->GetStringField(TEXT("motion"));
            FString Bone;
            Data->TryGetStringField(TEXT("bone"), Bone);
            const auto Contact = static_cast<EContact>(Data->GetIntegerField(TEXT("contact")));
            const auto& Hulls = Data->GetArrayField(TEXT("hulls"));
            const auto& Round = Data->GetArrayField(TEXT("round"));
            const bool bCartHull = Data->GetBoolField(TEXT("cartHull"));
            const bool bSplitRound = bCartHull && !Hulls.IsEmpty() && !Round.IsEmpty();
            enum class EShape { Simple, Triangles, RoundOnly };
            for (const auto Type : {EShape::Simple, EShape::Triangles, EShape::RoundOnly})
            {
                if ((Type == EShape::Triangles && Data->GetArrayField(TEXT("faces")).IsEmpty())
                    || (Type == EShape::Simple && Hulls.IsEmpty() && Round.IsEmpty())
                    || (Type == EShape::RoundOnly && !bSplitRound))
                    continue;
                auto* Mesh = NewObject<UStaticMesh>(&Owner, NAME_None, RF_Transient);
                Mesh->CreateBodySetup();
                auto& Setup = *Mesh->GetBodySetup();
                FBox Bounds(ForceInit);
                if (Type == EShape::Triangles)
                {
                    Setup.CollisionTraceFlag = CTF_UseComplexAsSimple;
                    Setup.bDoubleSidedGeometry = true;
                    Bounds = AddTriangles(Setup, *Data);
                    if (Setup.TriMeshGeometries.IsEmpty())
                        continue;
                    ++TriangleMeshes;
                }
                else
                {
                    Setup.CollisionTraceFlag = CTF_UseSimpleAsComplex;
                    if (Type == EShape::Simple)
                        AddHulls(Setup, Hulls);
                    if (Type == EShape::RoundOnly || !bSplitRound)
                        AddRoundShapes(Setup, Round);
                    if (!Setup.AggGeom.GetElementCount())
                        continue;
                    Bounds = Setup.AggGeom.CalcAABB(FTransform::Identity);
                    ++SimpleBodies;
                }
                SetCollisionBounds(*Mesh, Bounds);
                Parts.Add({Mesh, Motion, FName(*Bone), Contact, bCartHull && Type == EShape::Simple && !Hulls.IsEmpty()});
            }
        }
    }
    TMap<FString, TSharedPtr<FT5RagdollRig>> PhysicsByModel;
    const TSharedPtr<FJsonObject>* Ragdolls = nullptr;
    if (Manifest.TryGetObjectField(TEXT("ragdolls"), Ragdolls))
        for (const auto& [Path, Value] : (*Ragdolls)->Values)
        {
            const auto* Shapes = Cache.Find(FString(Path));
            if (!Shapes) continue;
            const auto& Definition = *Value->AsObject();
            auto Rig = MakeShared<FT5RagdollRig>();
            Rig->Asset.Reset(NewObject<UPhysicsAsset>(&Owner, NAME_None, RF_Transient));
            auto& Asset = *Rig->Asset;
            for (const auto& Body : Definition.GetArrayField(TEXT("bones")))
            {
                const auto& Row = Body->AsArray();
                const FName Bone(*Row[0]->AsString());
                const auto* Shape = Shapes->FindByPredicate([Bone](const FShape& Part) {
                    return Part.Bone == Bone && Part.Mesh->GetBodySetup()->AggGeom.GetElementCount() != 0;
                });
                if (!Shape) continue;
                auto* Setup = NewObject<USkeletalBodySetup>(&Asset);
                Setup->BoneName = Bone;
                Setup->AggGeom = Shape->Mesh->GetBodySetup()->AggGeom;
                Setup->bCreatedPhysicsMeshes = true;
                Setup->CollisionTraceFlag = CTF_UseSimpleAsComplex;
                Setup->DefaultInstance.SetMassOverride(FMath::Max(.1f, float(Row[1]->AsNumber())), true);
                Setup->DefaultInstance.LinearDamping = Row[2]->AsNumber();
                Setup->DefaultInstance.AngularDamping = Row[3]->AsNumber();
                Setup->DefaultInstance.SetPositionSolverIterationCount(12);
                Setup->DefaultInstance.SetVelocitySolverIterationCount(4);
                if (Row.IsValidIndex(5))
                {
                    auto* Material = NewObject<UPhysicalMaterial>(Setup);
                    Material->Friction = Material->StaticFriction = FMath::Max(0., Row[4]->AsNumber());
                    Material->Restitution = FMath::Clamp(Row[5]->AsNumber(), 0., 1.);
                    Setup->PhysMaterial = Material;
                }
                Rig->Bones.Add(Bone);
                Asset.SkeletalBodySetups.Add(Setup);
            }
            Asset.UpdateBodySetupIndexMap();
            Asset.UpdateBoundsBodiesArray();
            const auto Direction = [](const TSharedPtr<FJsonValue>& V) { return Position(V->AsArray()) / T5CentimetersPerUnit; };
            const auto Frame = [&](const TSharedPtr<FJsonValue>& Value) {
                const auto& Row = Value->AsArray();
                return FTransform(FRotationMatrix::MakeFromXY(Direction(Row[1]), Direction(Row[2])).Rotator(),
                    Position(Row[0]->AsArray()));
            };
            for (const auto& Joint : Definition.GetArrayField(TEXT("joints")))
            {
                const auto& Row = Joint->AsArray();
                const FName Child(*Row[0]->AsString()), Parent(*Row[1]->AsString());
                const int32 A = Asset.FindBodyIndex(Child), B = Asset.FindBodyIndex(Parent);
                if (A == INDEX_NONE || B == INDEX_NONE) continue;
                Rig->Parents.Add(Child, Parent);
                auto* Template = NewObject<UPhysicsConstraintTemplate>(&Asset);
                auto& Constraint = Template->DefaultInstance;
                Constraint.JointName = Child;
                Constraint.ConstraintBone1 = Child;
                Constraint.ConstraintBone2 = Parent;
                Constraint.SetRefFrame(EConstraintFrame::Frame1, Frame(Row[2]));
                Constraint.SetRefFrame(EConstraintFrame::Frame2, Frame(Row[3]));
                Constraint.SetLinearXLimit(LCM_Locked, 0);
                Constraint.SetLinearYLimit(LCM_Locked, 0);
                Constraint.SetLinearZLimit(LCM_Locked, 0);
                const auto Degrees = [](double Angle) { return float(FMath::Clamp(FMath::RadiansToDegrees(Angle), 0., 179.)); };
                const float Cone = Degrees(Row[4]->AsNumber());
                const float Plane = Degrees(FMath::Max(FMath::Abs(Row[5]->AsNumber()), FMath::Abs(Row[6]->AsNumber())));
                const float Twist = Degrees(FMath::Max(FMath::Abs(Row[7]->AsNumber()), FMath::Abs(Row[8]->AsNumber())));
                Constraint.SetAngularSwing1Limit(Cone > 0 ? ACM_Limited : ACM_Locked, Cone);
                Constraint.SetAngularSwing2Limit(Plane > 0 ? ACM_Limited : ACM_Locked, Plane);
                Constraint.SetAngularTwistLimit(Twist > 0 ? ACM_Limited : ACM_Locked, Twist);
                // Solve joints and ground contacts together. Chaos projection
                // corrects a joint after contacts and can move limbs into the floor.
                Constraint.SetProjectionParams(false, 0, 0, 0, 0);
                Constraint.SetDisableCollision(true);
                Asset.DisableCollision(A, B);
                Asset.ConstraintSetup.Add(Template);
            }
            if (!Asset.ConstraintSetup.IsEmpty()) PhysicsByModel.Add(FString(Path), MoveTemp(Rig));
        }
    for (const auto& Value : Manifest.GetArrayField(TEXT("instances")))
    {
        const auto& Instance = Value->AsObject();
        const auto* Parts = Cache.Find(Instance->GetStringField(TEXT("model")));
        if (!Parts)
            continue;
        const uint32 Id = Instance->GetNumberField(TEXT("ref"));
        const auto Pose = Transform(Instance->GetArrayField(TEXT("pose")));
        TArray<USkeletalMeshComponent*> Rigs;
        if (Instance->GetBoolField(TEXT("skeleton")))
        {
            Skeletons.MultiFind(Id, Rigs);
            if (const auto* Physics = PhysicsByModel.Find(Instance->GetStringField(TEXT("model"))))
                RagdollRigs.Add(Id, *Physics);
        }
        for (const auto& Part : *Parts)
        {
            if (Part.bCartHull && Carts.Contains(Id))
                continue;
            USceneComponent* Parent = Owner.GetRootComponent();
            if (!Part.Bone.IsNone())
            {
                const auto* Rig = Rigs.FindByPredicate([&](const USkeletalMeshComponent* Component) {
                    return Component->GetBoneIndex(Part.Bone) != INDEX_NONE;
                });
                if (!Rig)
                    continue;
                Parent = *Rig;
                ++BoneBodies;
            }
            auto* Component = CollisionComponent(Owner, *Part.Mesh, *Parent, Part.Bone,
                Part.Bone.IsNone() ? Pose : FTransform::Identity, Part.Contact,
                Id ? EComponentMobility::Movable : Parent->Mobility.GetValue(), true);
            // The front axle/shafts and any extra collision pieces are part of
            // this same vehicle. A wheel sweep must never support itself on them.
            // Animated bodies cannot be the road beneath a wheel or their own hoof.
            if (Carts.Contains(Id) || !Part.Bone.IsNone())
                CollisionParts.Add(Component);
            Component->ComponentTags.Add(FName(*Instance->GetStringField(TEXT("model"))));
            Register(Id, Component, Part.Motion);
            ++Instances;
        }
    }
    UE_LOG(LogT5Mechanics, Display, TEXT("T5_NIF_COLLISION models=%d instances=%d triangleMeshes=%d simpleBodies=%d boneBodies=%d"),
        Cache.Num(), Instances, TriangleMeshes, SimpleBodies, BoneBodies);
}

bool FT5Mechanics::StartRagdoll(uint32 Id, USkeletalMeshComponent& Mesh, const TArray<TSharedPtr<FJsonValue>>* Authored,
    const TArray<FTransform>* Previous)
{
    const auto* Entry = RagdollRigs.Find(Id);
    if (!Entry || !Mesh.GetSkeletalMeshAsset()) return false;
    const auto& Rig = **Entry;
    const auto& Skeleton = Mesh.GetSkeletalMeshAsset()->GetRefSkeleton();
    TArray<FTransform> Local;
    const bool bRetained = Previous && Previous->Num() == Skeleton.GetNum();
    if (bRetained) Local = *Previous;
    bool bAuthored = !bRetained && Authored
        && (Authored->Num() == Rig.Bones.Num() || Authored->Num() == Rig.Bones.Num() + 1);
    if (bAuthored)
    {
        TMap<FName, FTransform> BodyPose;
        for (int32 Index = 0; Index < Rig.Bones.Num(); ++Index)
        {
            const auto Bone = Rig.Bones[Index];
            const auto& Row = (*Authored)[Index]->AsArray();
            if (Row.Num() != 6) { bAuthored = false; break; }
            const FQuat X(FVector::XAxisVector, Row[3]->AsNumber());
            const FQuat Y(FVector::YAxisVector, Row[4]->AsNumber());
            const FQuat Z(FVector::ZAxisVector, Row[5]->AsNumber());
            FTransform Pose(T5ToUnrealRotation(X * Y * Z), Position(Row));
            if (const auto* Parent = BodyPose.Find(Rig.Parents.FindRef(Bone))) Pose *= *Parent;
            BodyPose.Add(Bone, Pose);
        }
        if (bAuthored)
        {
            Local = Skeleton.GetRefBonePose();
            TArray<FTransform> Model;
            Model.Reserve(Local.Num());
            for (int32 Index = 0; Index < Local.Num(); ++Index)
            {
                const auto Parent = Skeleton.GetParentIndex(Index);
                const auto ParentPose = Parent == INDEX_NONE ? FTransform::Identity : Model[Parent];
                const auto* Fixed = BodyPose.Find(Skeleton.GetBoneName(Index));
                Model.Add(Fixed ? *Fixed : Local[Index] * ParentPose);
                if (Fixed) Local[Index] = Fixed->GetRelativeTransform(ParentPose);
            }
        }
    }
    if (!Local.IsEmpty())
    {
        Mesh.SetAnimInstanceClass(UT5HkxAnimInstance::StaticClass());
        if (auto* Anim = Cast<UT5HkxAnimInstance>(Mesh.GetAnimInstance())) Anim->SetFixedPose(MoveTemp(Local));
        Mesh.TickAnimation(0, false);
        Mesh.RefreshBoneTransforms();
    }
    Mesh.SetCollisionProfileName(TEXT("Ragdoll"));
    Mesh.SetCollisionResponseToChannel(ECC_Pawn, ECR_Ignore);
    Mesh.SetCollisionResponseToChannel(ECC_Visibility, ECR_Block);
    Mesh.SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
    Mesh.SetPhysicsAsset(Rig.Asset.Get(), true);
    Mesh.SetAllBodiesSimulatePhysics(true);
    Mesh.SetPhysicsBlendWeight(1);
    if (bAuthored || bRetained) Mesh.PutAllRigidBodiesToSleep();
    UE_LOG(LogT5Mechanics, Display, TEXT("T5_RAGDOLL ref=%08X bodies=%d joints=%d authored=%d retained=%d"), Id,
        Rig.Bones.Num(), Rig.Asset->ConstraintSetup.Num(), bAuthored, bRetained);
    return true;
}

void FT5Mechanics::ResistRagdollDrift(uint32 Id, USkeletalMeshComponent& Mesh, float DeltaTime) const
{
    const auto* Entry = RagdollRigs.Find(Id);
    if (!Entry || DeltaTime <= 0) return;
    const auto& Rig = **Entry;
    TArray<FBodyInstance*, TInlineAllocator<32>> Bodies;
    FVector Momentum = FVector::ZeroVector;
    double Mass = 0, SquareSpeed = 0;
    for (const auto& Bone : Rig.Bones)
        if (auto* Body = Mesh.GetBodyInstance(Bone); Body && Body->IsInstanceSimulatingPhysics() && Body->IsInstanceAwake())
        {
            const double Weight = Body->GetBodyMass();
            const FVector Speed = Body->GetUnrealWorldVelocity();
            Bodies.Add(Body);
            Momentum += Weight * Speed;
            SquareSpeed += Weight * Speed.SizeSquared();
            Mass += Weight;
        }
    if (Mass <= 0) return;

    constexpr double ContactTolerance = 2; // cm
    const FVector Velocity = Momentum / Mass;
    FCollisionQueryParams Query(SCENE_QUERY_STAT(T5RagdollSupport), true);
    Query.AddIgnoredComponent(&Mesh);
    FVector Support = FVector::ZeroVector;
    double Friction = 0, SupportedMass = 0;
    int32 Contacts = 0;
    for (const auto* Body : Bodies)
    {
        const auto* Material = Body->GetSimplePhysicalMaterial();
        if (!Material || Material->StaticFriction <= 0) continue;
        const FBox Bounds = Body->GetBodyBounds();
        const FVector Center = Bounds.GetCenter();
        FHitResult Hit;
        if (!Mesh.GetWorld()->LineTraceSingleByChannel(Hit, Center,
                FVector(Center.X, Center.Y, Bounds.Min.Z - ContactTolerance), ECC_Visibility, Query)
            || !Hit.GetComponent() || !CollisionEnabledHasPhysics(Hit.GetComponent()->GetCollisionEnabled())
            || Hit.ImpactNormal.Z <= 0) continue;
        Support += Hit.ImpactNormal;
        Friction += Material->StaticFriction;
        SupportedMass += Body->GetBodyMass();
        ++Contacts;
    }
    if (Contacts < 2) return;
    const FVector Normal = Support.GetSafeNormal();
    const double Coefficient = Friction / Contacts;
    const FVector Gravity(0, 0, Mesh.GetWorld()->GetGravityZ());
    const double NormalAcceleration = -FVector::DotProduct(Gravity, Normal);
    const double Resistance = Coefficient * NormalAcceleration - FVector::VectorPlaneProject(Gravity, Normal).Size();
    if (Resistance <= 0) return;
    // Stop solver jitter once the support bears the body and the remaining
    // kinetic energy would move it by less than the contact tolerance.
    if (SupportedMass >= Mass * .25 && SquareSpeed / Mass <= 2 * Resistance * ContactTolerance)
    {
        Mesh.PutAllRigidBodiesToSleep();
        return;
    }
    // Resist the shared motion of the supported body without stopping limb rotation,
    // vertical motion, or a fall where the slope exceeds the authored friction.
    const FVector Correction = -FVector::VectorPlaneProject(Velocity, Normal)
        .GetClampedToMaxSize(Resistance * FMath::Min(DeltaTime, .1f));
    if (!Correction.IsNearlyZero())
        for (auto* Body : Bodies) Body->SetLinearVelocity(Correction, true, false);
}

int32 FT5Mechanics::WheelContacts(uint32 Id) const
{
    const auto* Cart = Carts.Find(Id);
    return Cart ? Cart->Contacts : 0;
}

UStaticMeshComponent* FT5Mechanics::DetachBodyPart(uint32 Id, FName Bone, AActor& Owner, const FTransform& Pose) const
{
    const auto* Entry = RagdollRigs.Find(Id);
    if (!Entry) return nullptr;
    const auto& Asset = *(*Entry)->Asset;
    const int32 Index = Asset.FindBodyIndex(Bone);
    if (!Asset.SkeletalBodySetups.IsValidIndex(Index)) return nullptr;
    const auto& Source = *Asset.SkeletalBodySetups[Index];
    auto* Mesh = NewObject<UStaticMesh>(&Owner, NAME_None, RF_Transient);
    Mesh->CreateBodySetup();
    auto& Setup = *Mesh->GetBodySetup();
    Setup.CollisionTraceFlag = CTF_UseSimpleAsComplex;
    Setup.AggGeom = Source.AggGeom;
    Setup.DefaultInstance = Source.DefaultInstance;
    Setup.PhysMaterial = Source.PhysMaterial;
    SetCollisionBounds(*Mesh, Setup.AggGeom.CalcAABB(FTransform::Identity));
    auto* Body = NewObject<UStaticMeshComponent>(&Owner, NAME_None, RF_Transient);
    Owner.SetRootComponent(Body);
    Body->SetMobility(EComponentMobility::Movable);
    Body->SetStaticMesh(Mesh);
    Body->SetCollisionProfileName(TEXT("PhysicsActor"));
    Body->SetCollisionResponseToChannel(ECC_Pawn, ECR_Ignore);
    Body->SetCollisionResponseToChannel(ECC_Visibility, ECR_Ignore);
    Body->SetHiddenInGame(true);
    Body->SetWorldTransform(Pose);
    Body->RegisterComponent();
    return Body;
}
TOptional<double> FT5Mechanics::ArticulationError(uint32 Id) const
{
    const auto* C=Carts.Find(Id);
    if (!C || !C->Physics || !C->Physics->bAttached) return {};
    return C->Physics->JointError();
}

bool FT5Mechanics::HasVehicleSurface(uint32 Id) const
{
    const auto* Cart = Carts.Find(Id);
    return !Cart || Cart->TerrainContacts == Cart->Wheels.Num();
}

void FT5Mechanics::ContinueMotionFrom(const FT5Mechanics& Previous)
{
    for (auto& [Id, Actor] : GroundActors)
        if (const auto* Old = Previous.GroundActors.Find(Id))
        { Actor.Route = Old->Route; Actor.Pose = Old->Pose; Actor.bGrounded = Old->bGrounded; }
    for (auto& [Id, Cart] : Carts)
        if (const auto* Old = Previous.Carts.Find(Id))
        {
            Cart.Route = Old->Route;
            Cart.Pose = Old->Pose;
            Cart.bGrounded = Old->bGrounded;
            Cart.LastLoggedLift = Old->LastLoggedLift;
            Cart.LastSample = Old->LastSample;
            Cart.TerrainContacts = Old->TerrainContacts;
            if (Cart.Physics && Old->Physics) Cart.Physics->CopyState(*Old->Physics);
        }
}

FTransform FT5Mechanics::CorrectPose(uint32 Id, uint32 Parent, const FTransform& Pose) const
{
    if (const auto* Actor = GroundActors.Find(Id); Actor && Actor->bGrounded)
        return Actor->Pose;
    if (const auto* Cart = Carts.Find(Id); Cart && Cart->bGrounded)
        return Cart->Pose;
    if (const auto* Cart = Carts.Find(Parent); Cart && Cart->bGrounded)
        return Pose * Cart->Route.Inverse() * Cart->Pose;
    if (const auto* Actor = GroundActors.Find(Parent); Actor && Actor->bGrounded)
        return Pose * Actor->Route.Inverse() * Actor->Pose;
    return Pose;
}

void FT5Mechanics::SetSequence(uint32 Id, const FString& Name, double Elapsed)
{
    if (auto* Playback = ModelsByReference.Find(Id); Playback && Playback->Sequences.Contains(Name))
    {
        if (Playback->Name != Name)
            UE_LOG(LogT5Mechanics, Display, TEXT("T5_MODEL_SEQUENCE ref=%08X clip=%s"), Id, *Name);
        Playback->Name = Name;
        Playback->Elapsed = Elapsed;
        Playback->Layer.Reset();
    }
}

void FT5Mechanics::SetHkxSequence(uint32 Id, const FString& Rig, const FString& Animation, double Elapsed,
    FT5PapyrusRuntime& Papyrus, const FString& Layer, double LayerElapsed)
{
    auto& Playback = ModelsByReference.FindOrAdd(Id);
    if (!Playback.Sequences.Contains(Animation))
    {
        auto& Sequence = Playback.Sequences.Add(Animation);
        TArray<uint8> RigBytes, AnimationBytes;
        FT5HkxClip Clip;
        FString Error;
        if (!Papyrus.ReadResource(Rig, RigBytes) || !Papyrus.ReadResource(Animation, AnimationBytes)
            || !ReadT5HkxClip(RigBytes, AnimationBytes, Clip, Error))
        {
            UE_LOG(LogT5Mechanics, Warning, TEXT("T5 object clip %s could not be decoded: %s"), *Animation, *Error);
            return;
        }
        Sequence.Seconds = Clip.Duration;
        TArray<FSequenceTrack*> Tracks;
        for (const auto& Bone : Clip.Bones)
            Sequence.Tracks.Add(Bone.Name.ToString());
        for (const auto& Bone : Clip.Bones)
        {
            auto& Track = Sequence.Tracks.FindChecked(Bone.Name.ToString());
            Track.Rest = Bone.Rest;
            if (Bone.Parent != INDEX_NONE)
                Track.Rest *= Tracks[Bone.Parent]->Rest;
            Track.Frames.Reserve(Clip.Frames);
            Tracks.Add(&Track);
        }
        for (int32 Frame = 0; Frame < Clip.Frames; ++Frame)
            for (int32 BoneIndex = 0; BoneIndex < Clip.Bones.Num(); ++BoneIndex)
            {
                const auto& Bone = Clip.Bones[BoneIndex];
                const auto Source = Clip.BoneToTrack[BoneIndex];
                auto Pose = Source == INDEX_NONE ? Bone.Rest : Clip.Samples[Frame * Clip.Tracks + Source];
                if (Bone.Parent != INDEX_NONE)
                    Pose *= Tracks[Bone.Parent]->Frames[Frame];
                Tracks[BoneIndex]->Frames.Add(Pose);
            }
    }
    SetSequence(Id, Animation, Elapsed);
    Playback.Layer = Layer;
    Playback.LayerElapsed = LayerElapsed;
}

FTransform FT5Mechanics::DoorDelta(uint32 Id, const FString& Node, const FTransform& Authored, const FTransform& Pose) const
{
    if (const auto* Playback = ModelsByReference.Find(Id))
    {
        const auto Delta = [&](const FString& Name, double Elapsed) -> TOptional<FTransform> {
            if (const auto* Sequence = Playback->Sequences.Find(Name); Sequence && Sequence->Seconds > 0)
                if (const auto* Track = Sequence->Tracks.Find(Node); Track && !Track->Frames.IsEmpty())
                    return Authored.Inverse() * Track->Rest.Inverse()
                        * Sample(Track->Frames, Elapsed / Sequence->Seconds) * Pose;
            return {};
        };
        if (const auto Motion = Delta(Playback->Name, Playback->Elapsed)) return *Motion;
        if (const auto Motion = Delta(Playback->Layer, Playback->LayerElapsed)) return *Motion;
    }
    if (const auto* Cart=Carts.Find(Id); Cart && Cart->Physics && CollisionOwner.IsValid())
        if (auto Delta=Cart->Physics->NodeDelta(Node,Authored,CollisionOwner->GetActorTransform())) return *Delta;
    if (const auto* Door = Doors.Find(Id))
        if (const auto* Part = Door->Parts.Find(Node))
            return (Part->Rest * Authored).Inverse() * (Part->Pose * Pose);
    return Authored.Inverse() * Pose;
}

void FT5Mechanics::Tick(AActor& Owner, FT5PapyrusRuntime& Papyrus, bool bExterior)
{
    UWorld& World = *Owner.GetWorld();
    const FTransform Origin = Owner.GetActorTransform();
    FComponentQueryParams Query(SCENE_QUERY_STAT(T5WheelSupport));
    Query.bTraceComplex = true;
    Query.bIgnoreTouches = true;
    TInlineComponentArray<UStaticMeshComponent*> StaticMeshes(&Owner);
    StaticMeshes.RemoveAll([](const auto* Component) { return !Component->ComponentHasTag(TEXT("T5Terrain")); });
    const double Now = World.GetTimeSeconds();
    static const bool bSupportTraceEnabled = FParse::Param(FCommandLine::Get(), TEXT("T5SupportTrace"));
    static const double SupportTraceInterval = [] { double Value = 0; FParse::Value(FCommandLine::Get(), TEXT("T5SupportTraceInterval="), Value); return FMath::Max(0., Value); }();
    const bool bTraceSupport = bSupportTraceEnabled && Now - LastSupportLog >= SupportTraceInterval;
    if (bTraceSupport) LastSupportLog = Now;
    for (const auto& Part : CollisionParts)
        if (auto* Component = Part.Get())
            Query.AddIgnoredComponent(Component);

    const double SearchHeight = 4096 * T5CentimetersPerUnit;
    const auto GroundAt = [&](const FVector& Point, FHitResult& Ground) {
        bool bFound = false;
        for (auto* Terrain : StaticMeshes)
        {
            FHitResult Hit;
            if (Terrain->LineTraceComponent(Hit, Point + FVector(0, 0, SearchHeight),
                    Point - FVector(0, 0, SearchHeight), Query)
                && (!bFound || FMath::Abs(Hit.ImpactPoint.Z - Point.Z) < FMath::Abs(Ground.ImpactPoint.Z - Point.Z)))
            {
                Ground = Hit;
                bFound = true;
            }
        }
        return bFound;
    };
    for (auto& [Id, Actor] : GroundActors)
    {
        FTransform Authored;
        uint32 Parent;
        const auto Route = Papyrus.ReferenceTransform(Id, Authored, Parent);
        const bool bWasGrounded = Actor.bGrounded;
        Actor.bGrounded = false;
        if (!Route || Parent || Actor.Feet.Num() < 3 || Papyrus.IsDisabled(Id).Get(false))
            continue;
        const FTransform Local = Actor.Original * Authored.Inverse();
        TArray<FVector> Points;
        double Height = 0;
        for (const auto& Foot : Actor.Feet)
        {
            const FVector Center = (Local * *Route * Origin).TransformPosition(Foot);
            FHitResult Ground;
            if (!GroundAt(Center, Ground))
                continue;
            // Select road NIF collision close to LAND, so a roof above the road cannot win.
            TArray<FHitResult> Hits;
            World.LineTraceMultiByChannel(Hits, Ground.ImpactPoint + FVector(0, 0, 200),
                Ground.ImpactPoint - FVector(0, 0, 200), ECC_Visibility, Query);
            for (const auto& Hit : Hits)
                if (Hit.bBlockingHit && Hit.ImpactNormal.Z > .35)
                {
                    Ground = Hit;
                    break;
                }
            const FVector Point = Origin.InverseTransformPosition(Ground.ImpactPoint);
            Points.Add(Point);
        }
        if (Points.Num() != Actor.Feet.Num())
        {
            if (bWasGrounded)
            {
                Actor.Pose = *Route * Actor.Route.Inverse() * Actor.Pose;
                Actor.Route = *Route;
                Actor.bGrounded = true;
            }
            continue;
        }
        FVector Mean = FVector::ZeroVector;
        for (const auto& Point : Points) Mean += Point;
        Mean /= Points.Num();
        double XX=0, XY=0, YY=0, XZ=0, YZ=0;
        for (const auto& Point : Points)
        {
            const FVector D = Point - Mean;
            XX += D.X*D.X; XY += D.X*D.Y; YY += D.Y*D.Y; XZ += D.X*D.Z; YZ += D.Y*D.Z;
        }
        Actor.Route = *Route;
        Actor.Pose = *Route;
        const double Det = XX*YY-XY*XY;
        if (Det > UE_SMALL_NUMBER)
        {
            const FVector Up(-(XZ*YY-YZ*XY)/Det, -(YZ*XX-XZ*XY)/Det, 1);
            Actor.Pose.SetRotation(FQuat::FindBetweenNormals(FVector::UpVector, Up.GetSafeNormal()) * Route->GetRotation());
        }
        for (int32 I = 0; I < Points.Num(); ++I)
            Height += Points[I].Z - (Local * Actor.Pose).TransformPosition(Actor.Feet[I]).Z;
        Actor.Pose.AddToTranslation(FVector(0, 0, Height / Points.Num()));
        Actor.bGrounded = true;
        if (bTraceSupport)
            UE_LOG(LogT5Mechanics, Display, TEXT("T5_HOOF_SUPPORT time=%.4f ref=%08X feet=%d z=%.3f routeZ=%.3f"),
                Now, Id, Points.Num(), Actor.Pose.GetLocation().Z, Route->GetLocation().Z);
    }

    for (auto& [Id, Cart] : Carts)
    {
        FTransform Authored;
        uint32 Horse;
        const auto Pose = Papyrus.ReferenceTransform(Id, Authored, Horse);
        if (!Pose)
            continue;
        const bool bEnabled = !Papyrus.IsDisabled(Id).Get(false);
        if (Cart.Physics && Cart.Physics->bStarted)
        {
            Cart.Physics->SetMotionType(Papyrus.MotionType(Id));
            Cart.Physics->SetPaused(bSimulationPaused || !bEnabled);
            if (!Horse && Cart.Physics->bAttached) Cart.Physics->Detach();
            if (auto* Animal=GroundActors.Find(Horse); Animal && Animal->Mesh.IsValid() && Animal->bGrounded)
            {
                FTransform HorseAuthored; uint32 HorseParent;
                if (Papyrus.ReferenceTransform(Horse,HorseAuthored,HorseParent)
                    && Animal->Mesh->GetBoneIndex(FName(*Cart.Physics->HorseBone))!=INDEX_NONE)
                {
                    const auto Anchor=Animal->Mesh->GetSocketTransform(FName(*Cart.Physics->HorseBone),RTS_Component)
                        * Animal->Original * HorseAuthored.Inverse() * Animal->Pose * Origin;
                    if (!Cart.Physics->bAttached) Cart.Physics->Attach(Anchor);
                    Cart.Physics->Drive(Anchor);
                }
            }
            Cart.Physics->Tick();
            Cart.Route=*Pose;
            Cart.Pose=Cart.Physics->Pose()*Origin.Inverse();
            Papyrus.PhysicsPose(Id,Cart.Pose,true);
            Cart.bGrounded=true;
            Cart.Contacts=0; Cart.TerrainContacts=0;
            for (const auto& Wheel : Cart.Wheels)
            {
                const FTransform W=Cart.Physics->NodePose(Wheel.Node).Get(Wheel.Rest*Cart.Pose*Origin);
                const FVector Center=W.GetLocation();
                FHitResult Ground;
                if (GroundAt(Center,Ground)) ++Cart.TerrainContacts;
                TArray<FHitResult> Hits;
                if (auto* C=Wheel.Collision.Get())
                {
                    World.ComponentSweepMultiByChannel(Hits,C,Center+FVector(0,0,2),Center-FVector(0,0,5),W.GetRotation(),ECC_Visibility,Query);
                    if (Hits.ContainsByPredicate([](const auto& H) { return H.bBlockingHit; })) ++Cart.Contacts;
                }
            }
            if (bTraceSupport)
                UE_LOG(LogT5Mechanics,Display,TEXT("T5_CHAOS_POSE time=%.4f ref=%08X x=%.3f y=%.3f z=%.3f pitch=%.3f roll=%.3f contacts=%d jointError=%.4f"),
                    Now,Id,Cart.Pose.GetLocation().X,Cart.Pose.GetLocation().Y,Cart.Pose.GetLocation().Z,Cart.Pose.Rotator().Pitch,Cart.Pose.Rotator().Roll,Cart.Contacts,Cart.Physics->JointError());
            continue;
        }
        FTransform Result = *Pose;
        const bool bWasGrounded = Cart.bGrounded;
        const double Delta = FMath::Clamp(Now - Cart.LastSample, 0., .12);
        Cart.LastSample = Now;
        Cart.Contacts = 0;
        int32 NifContacts = 0;
        if (Horse && bEnabled)
        {
            const auto Solve = [&](FTransform& Base, bool bTilt)
            {
                TArray<FVector, TInlineAllocator<4>> Supports;
                TArray<FVector, TInlineAllocator<4>> Centers;
                NifContacts = 0;
                Cart.TerrainContacts = 0;
                for (const auto& Wheel : Cart.Wheels)
                {
                    auto* Component = Wheel.Collision.Get();
                    if (!Component)
                        continue;
                    const FTransform WheelWorld = Wheel.Rest * Base * Origin;
                    // The sweep uses the original convex wheel and the road's Chaos collision.
                    if (Component->IsZeroExtent())
                        continue;
                    const FVector Center = WheelWorld.GetLocation();
                    const auto Bounds = Component->GetStaticMesh()->GetBodySetup()->AggGeom.CalcAABB(WheelWorld);
                    const double Radius = FMath::Max(Bounds.GetExtent().Z, UE_SMALL_NUMBER);
                    const double BottomOffset = Center.Z - Bounds.Min.Z;
                    // Establish the local ground under this wheel from the LAND mesh.
                    // Buildings above the road must not win a sweep that starts in midair.
                    FHitResult Ground;
                    const bool bTerrain = GroundAt(Center, Ground);
                    if (bTerrain)
                        ++Cart.TerrainContacts;
                    const double Reach = Radius * 2.5;
                    const double PreviousLift = bWasGrounded ? Cart.Pose.GetLocation().Z - Cart.Route.GetLocation().Z : 0;
                    // LAND may be well below an authored road/bridge. Starting only from
                    // LAND can put the wheel inside that NIF, reject its penetrating hit,
                    // and fit a false slope. Cover both the carried wheel and LAND support.
                    const double StartZ = bTerrain ? FMath::Max(Ground.ImpactPoint.Z + BottomOffset + Reach,
                        Center.Z + PreviousLift + Reach) : Center.Z + PreviousLift + Reach;
                    const double EndZ = bTerrain ? FMath::Min(Ground.ImpactPoint.Z + BottomOffset - Reach,
                        Center.Z + PreviousLift - Reach) : Center.Z + PreviousLift - Reach;
                    TArray<FHitResult> Hits;
                    World.ComponentSweepMultiByChannel(Hits, Component, FVector(Center.X, Center.Y, StartZ),
                        FVector(Center.X, Center.Y, EndZ), WheelWorld.GetRotation(), ECC_Visibility, Query);
                    if (const auto* Hit = Hits.FindByPredicate([](const FHitResult& H) {
                        return H.bBlockingHit && !H.bStartPenetrating && H.ImpactNormal.Z > .35;
                    }))
                    {
                        if (bTraceSupport && !bTilt)
                            UE_LOG(LogT5Mechanics, Display, TEXT("T5_SUPPORT_HIT time=%.4f ref=%08X wheel=%d z=%.3f land=%.3f normal=%.3f component=%s tags=%s"),
                                Now, Id, Supports.Num(), Hit->Location.Z, Ground.ImpactPoint.Z, Hit->ImpactNormal.Z,
                                *GetNameSafe(Hit->GetComponent()), Hit->GetComponent() ? *FString::JoinBy(Hit->GetComponent()->ComponentTags, TEXT("|"), [](FName N){ return N.ToString(); }) : TEXT(""));
                        Supports.Add(Origin.InverseTransformPosition(Hit->Location));
                        Centers.Add((Wheel.Rest * Base).GetLocation());
                        if (const auto* Surface = Hit->GetComponent(); Surface && Surface->ComponentHasTag(TEXT("T5NifCollision")))
                            ++NifContacts;
                    }
                    else if (bTerrain)
                    {
                        if (bTraceSupport && !bTilt)
                            UE_LOG(LogT5Mechanics, Display, TEXT("T5_SUPPORT_LAND time=%.4f ref=%08X wheel=%d z=%.3f hits=%d"),
                                Now, Id, Supports.Num(), Ground.ImpactPoint.Z + BottomOffset, Hits.Num());
                        Supports.Add(Origin.InverseTransformPosition(Ground.ImpactPoint + FVector(0, 0, BottomOffset)));
                        Centers.Add((Wheel.Rest * Base).GetLocation());
                    }
                }
                Cart.Contacts = Supports.Num();
                if (Supports.IsEmpty())
                    return;
                if (bTilt && Supports.Num() >= 3)
                {
                    // Fit the slope of the corrections, so unequal front/rear wheel sizes
                    // do not tilt the cart when its four authored offsets already fit.
                    FVector Mean = FVector::ZeroVector;
                    for (int32 I = 0; I < Supports.Num(); ++I)
                        Mean += FVector(Centers[I].X, Centers[I].Y, Supports[I].Z - Centers[I].Z);
                    Mean /= Supports.Num();
                    double XX = 0, XY = 0, YY = 0, XZ = 0, YZ = 0;
                    for (int32 I = 0; I < Supports.Num(); ++I)
                    {
                        const FVector D = FVector(Centers[I].X, Centers[I].Y, Supports[I].Z - Centers[I].Z) - Mean;
                        XX += D.X * D.X; XY += D.X * D.Y; YY += D.Y * D.Y;
                        XZ += D.X * D.Z; YZ += D.Y * D.Z;
                    }
                    const double Det = XX * YY - XY * XY;
                    if (Det > 1)
                    {
                        // Fit the chassis to the supported wheel centers.
                        FVector Up(-(XZ * YY - YZ * XY) / Det, -(YZ * XX - XZ * XY) / Det, 1);
                        Base.SetRotation(FQuat::FindBetweenNormals(FVector::UpVector, Up.GetSafeNormal()) * Base.GetRotation());
                    }
                }
                else
                {
                    double Height = -TNumericLimits<double>::Max();
                    for (int32 I = 0; I < Supports.Num(); ++I)
                        Height = FMath::Max(Height, Supports[I].Z - Centers[I].Z);
                    Base.AddToTranslation(FVector(0, 0, Height));
                }
            };
            Solve(Result, true);
            if (Cart.Contacts && bWasGrounded)
            {
                const auto Carried = *Pose * Cart.Route.Inverse() * Cart.Pose;
                Result.SetRotation(FQuat::Slerp(Carried.GetRotation(), Result.GetRotation(), 1 - FMath::Exp(-Delta * 9.)));
            }
            if (Cart.Contacts)
                Solve(Result, false);
        }
        if (Cart.Contacts)
        {
            // Vertical position is the final contact solve, never a blend toward route Z.
            Cart.Route = *Pose;
            Cart.Pose = Result;
            Cart.bGrounded = true;
            const double Lift = Result.GetLocation().Z - Pose->GetLocation().Z;
            if (!bWasGrounded || Lift > Cart.LastLoggedLift + 50)
            {
                Cart.LastLoggedLift = Lift;
                UE_LOG(LogT5Mechanics, Display, TEXT("T5_CART_CONTACT ref=%08X wheels=%d lift=%.1f nif=%d at=%s"),
                    Id, Cart.Contacts, Lift, NifContacts, *Pose->GetLocation().ToCompactString());
            }
        }
        else if (bWasGrounded)
        {
            Cart.Pose = *Pose * Cart.Route.Inverse() * Cart.Pose;
            Cart.Route = *Pose;
        }
        if (bTraceSupport)
            UE_LOG(LogT5Mechanics, Display, TEXT("T5_SUPPORT_POSE time=%.4f ref=%08X x=%.3f y=%.3f z=%.3f routeZ=%.3f pitch=%.3f roll=%.3f contacts=%d nif=%d"),
                Now, Id, Cart.Pose.GetLocation().X, Cart.Pose.GetLocation().Y, Cart.Pose.GetLocation().Z,
                Pose->GetLocation().Z, Cart.Pose.Rotator().Pitch, Cart.Pose.Rotator().Roll, Cart.Contacts, NifContacts);
        const auto& BodyPose = Cart.bGrounded ? Cart.Pose : *Pose;
        if (Cart.Physics)
        {
            // Contact fitting is only the initial placement. Once started, only Chaos
            // moves the body/axles/wheels; route samples must never teleport them.
            if (Horse && Cart.bGrounded && !bSimulationPaused && bEnabled) Cart.Physics->Start(BodyPose*Origin);
            continue;
        }
        UpdateBody(Cart.Chassis.Collision.Get(), Cart.Chassis.Rest * BodyPose, bEnabled);
        for (const auto& Wheel : Cart.Wheels)
            UpdateBody(Wheel.Collision.Get(), Wheel.Rest * BodyPose, bEnabled);
    }

    for (auto& [Id, Door] : Doors)
    {
        FTransform Authored;
        uint32 Parent;
        const auto Pose = Papyrus.ReferenceTransform(Id, Authored, Parent);
        if (!Pose)
            continue;
        double Elapsed = 0;
        const auto State = Papyrus.DoorState(Id, Elapsed);
        if (State && (!Door.OpenState || *Door.OpenState != *State))
        {
            Door.OpenState = *State;
            UE_LOG(LogT5Mechanics, Display, TEXT("T5_DOOR ref=%08X open=%d nodes=%d"), Id, *State, Door.Parts.Num());
        }
        for (auto& [Node, Part] : Door.Parts)
        {
            const auto& Frames = State.Get(false) ? Part.Open : Part.Closed;
            const auto Duration = State.Get(false) ? Door.OpenSeconds : Door.CloseSeconds;
            Part.Pose = !State || Frames.IsEmpty() ? Part.Rest : Sample(Frames, Elapsed / Duration);
        }
    }

    float Data[21]{};
    constexpr float WeatherRadianceScale=5000.f;
    const double WeatherNow=World.GetTimeSeconds();
    const uint32 Weather = bExterior ? Papyrus.Environment(Data) : 0;
    if (Weather && (Weather != ActiveWeather || WeatherNow-WeatherUpdatedAt >= .5))
    {
        const bool bChanged=Weather!=ActiveWeather;
        ActiveWeather = Weather;
        WeatherUpdatedAt=WeatherNow;
        const auto Color = [&](int32 I) {
            // Sky::skyColor contains normalized linear RGB. These are lighting
            // values, not an sRGB texture; decoding them again darkens the fog.
            return FLinearColor(Data[I],Data[I+1],Data[I+2]);
        };
        for (TActorIterator<ADirectionalLight> It(&World); It; ++It)
        {
            auto* Light = Cast<UDirectionalLightComponent>(It->GetLightComponent());
            if (Light && Light->bAtmosphereSunLight) Light->SetLightColor(Color(0));
        }
        // WTHR ambient is an independent hemispherical light, not a multiplier
        // on UE's unrelated bootstrap atmosphere capture. Use it as the actual
        // irradiance source so runtime skeletal meshes receive the same fill.
        auto* AmbientCube = WeatherAmbientCube.Get();
        if(!AmbientCube){AmbientCube=UTextureCube::CreateTransient(16,16);WeatherAmbientCube=AmbientCube;}
        if (AmbientCube)
        {
            auto& Mip = AmbientCube->GetPlatformData()->Mips[0];
            auto* Pixels = static_cast<FColor*>(Mip.BulkData.Lock(LOCK_READ_WRITE));
            const FColor Ambient = Color(3).ToFColorSRGB();
            for (int32 I = 0; I < 16 * 16 * 6; ++I) Pixels[I] = Ambient;
            Mip.BulkData.Unlock(); AmbientCube->SRGB = true; AmbientCube->UpdateResource();
        }
        for (TActorIterator<ASkyLight> It(&World); It; ++It)
        {
            auto* Sky = Cast<USkyLightComponent>(It->GetLightComponent());
            Sky->SetLightColor(FLinearColor::White);
            if (AmbientCube)
            {
                Sky->bRealTimeCapture = false;
                Sky->SourceType = SLS_SpecifiedCubemap;
                Sky->SetCubemap(AmbientCube);
                Sky->SetIntensity(WeatherRadianceScale);
                Sky->SetCastShadows(false);
                Sky->RecaptureSky();
            }
        }
        // Startup prepares its scene while the owner is hidden. UE's fog state
        // stream cannot update a handle that was never created in that state.
        // Register only after reveal, with all initial properties already set.
        if (Owner.IsHidden()) return;
        if (!bTriedWeatherFogMaterial)
        {
            bTriedWeatherFogMaterial = true;
            if (!FParse::Param(FCommandLine::Get(), TEXT("T5WeatherFogDiagnostic")))
            {
                if (auto* Base = LoadObject<UMaterialInterface>(nullptr, TEXT("/T5Data/Runtime/M_T5WeatherFog.M_T5WeatherFog")))
                {
                    auto* Volume = NewObject<UPostProcessComponent>(&Owner, NAME_None, RF_Transient);
                    Volume->SetupAttachment(Owner.GetRootComponent());
                    Volume->bUnbound = true;
                    Volume->Priority = 1001;
                    auto* Material = UMaterialInstanceDynamic::Create(Base, Volume);
                    Volume->AddOrUpdateBlendable(Material, 1.f);
                    Volume->RegisterComponent();
                    WeatherFogVolume = Volume;
                    WeatherFogMaterial = Material;
                    UE_LOG(LogT5Mechanics, Display, TEXT("T5_WEATHER_DEPTH_FOG"));
                }
            }
        }
        if (auto* Material = WeatherFogMaterial.Get())
        {
            Material->SetVectorParameterValue(TEXT("FogRange"), FLinearColor(
                Data[9] * T5CentimetersPerUnit, Data[10] * T5CentimetersPerUnit, Data[14]));
            Material->SetVectorParameterValue(TEXT("FogColor"), Color(6));
            Material->SetVectorParameterValue(TEXT("SkyColor"), Color(15));
            Material->SetVectorParameterValue(TEXT("FogFarColor"), Color(18));
            Material->SetScalarParameterValue(TEXT("FogOpacity"), Data[11]);
            if (bChanged) UE_LOG(LogT5Mechanics, Display,
                TEXT("T5_WEATHER form=%08X fog=%.0f..%.0f hour=%.3f transition=%.3f"), Weather, Data[9], Data[10], Data[12], Data[13]);
            return;
        }
        if (!WeatherFog.IsValid())
        {
            auto* Fog = NewObject<UT5WeatherFogComponent>(&Owner, NAME_None, RF_Transient);
            Fog->SetupAttachment(Owner.GetRootComponent());
            WeatherFog = Fog;
        }
        // UE renders ExponentialFogs[0]. A bootstrap/cell fog registered first
        // would silently hide all WTHR changes on this component.
        for(TObjectIterator<UExponentialHeightFogComponent> It;It;++It)
            if(It->GetWorld()==&World && It->IsRegistered() && *It!=WeatherFog.Get() && It->IsVisible())
            {
                It->SetVisibility(false);
                It->UnregisterComponent();
                UE_LOG(LogT5Mechanics,Display,TEXT("T5_WEATHER_REPLACED_FOG %s"),*It->GetPathName());
            }
        WeatherFog->SetVisibility(true);
        // UE 5.8 expects luminance here, whereas WTHR stores normalized RGB.
        // Use the same radiometric scale as the weather ambient source.
        WeatherFog->SetFogInscatteringColor(Color(6)*WeatherRadianceScale);
        WeatherFog->SkyAtmosphereAmbientContributionColorScale=FLinearColor::Black;
        // Skyrim WTHR fog is distance based. UE's default vertical falloff
        // effectively removes it on Helgen's high-altitude road.
        WeatherFog->SetFogHeightFalloff(0.f);
        WeatherFog->SetStartDistance(FMath::Max(0.f, Data[9]) * T5CentimetersPerUnit);
        WeatherFog->SetFogMaxOpacity(FMath::Clamp(Data[11], 0.f, 1.f));
        const double Range = (Data[10] - Data[9]) * T5CentimetersPerUnit;
        const double MaxOpacity = FMath::Clamp(double(Data[11]), 0.0, 1.0);
        const double Power = FMath::Max(double(Data[14]), 0.01);
        double Density = 0;
        if (Range > 1 && MaxOpacity > UE_DOUBLE_SMALL_NUMBER)
        {
            // Preserve the mean opacity of the WTHR power curve from near to far.
            // UE uses min(MaxOpacity, 1 - exp(-OpticalDepth * normalizedDistance)).
            const double CapDistance = FMath::Pow(MaxOpacity, 1 / Power);
            const double Target = MaxOpacity * (1 - CapDistance * Power / (1 + Power));
            const double ClampDepth = -FMath::Loge(FMath::Max(1 - MaxOpacity, UE_DOUBLE_SMALL_NUMBER));
            const double ClampMean = 1 - MaxOpacity / ClampDepth;
            double OpticalDepth;
            if (MaxOpacity < 1 && Target > ClampMean)
                OpticalDepth = (MaxOpacity - (1 - MaxOpacity) * ClampDepth) / (MaxOpacity - Target);
            else
            {
                double Low = 0, High = ClampDepth;
                for (int32 Step = 0; Step < 24; ++Step)
                {
                    const double Middle = (Low + High) / 2;
                    const double Mean = 1 - (1 - FMath::Exp(-Middle)) / Middle;
                    if (Mean < Target) Low = Middle;
                    else High = Middle;
                }
                OpticalDepth = (Low + High) / 2;
            }
            // HeightFogCommon applies ln(2) in the zero-falloff integral and
            // then uses exp2 for transmittance. SceneCore divides density by 1000.
            constexpr double Ln2 = 0.6931471805599453;
            Density = FMath::Min(10.0, 1000 * OpticalDepth / (Ln2 * Ln2 * Range));
        }
        WeatherFog->SetFogDensity(float(Density));
        if(!WeatherFog->IsRegistered()) WeatherFog->RegisterComponent();
        else WeatherFog->RefreshWeather();
        if(bChanged)UE_LOG(LogT5Mechanics, Display, TEXT("T5_WEATHER form=%08X fog=%.0f..%.0f hour=%.3f transition=%.3f"), Weather, Data[9], Data[10],Data[12],Data[13]);
    }
}

void FT5Mechanics::SerializeCheckpoint(FArchive& Ar)
{
    int32 Count = Carts.Num(); Ar << Count;
    if (Count < 0 || Count > 4096) { Ar.SetError(); return; }
    const auto CartState = [&](FCart& Cart) {
        Ar << Cart.Route << Cart.Pose << Cart.Contacts << Cart.TerrainContacts << Cart.bGrounded << Cart.LastLoggedLift << Cart.LastSample;
        bool HasPhysics = Cart.Physics.IsValid(); Ar << HasPhysics;
        if (HasPhysics) { if (!Cart.Physics) { Ar.SetError(); return; } Cart.Physics->SerializeCheckpoint(Ar); }
    };
    if (Ar.IsLoading())
        for (int32 I = 0; I < Count && !Ar.IsError(); ++I)
        { uint32 Id = 0; Ar << Id; auto* Cart = Carts.Find(Id); if (!Cart) { Ar.SetError(); return; } CartState(*Cart); }
    else for (auto& [Id, Cart] : Carts) { uint32 Key = Id; Ar << Key; CartState(Cart); }
    Count = GroundActors.Num(); Ar << Count;
    if (Count < 0 || Count > 100000) { Ar.SetError(); return; }
    const auto ActorState = [&](FGroundActor& Actor) { Ar << Actor.Route << Actor.Pose << Actor.bGrounded; };
    if (Ar.IsLoading())
        for (int32 I = 0; I < Count && !Ar.IsError(); ++I)
        { uint32 Id = 0; Ar << Id; auto* Actor = GroundActors.Find(Id); if (!Actor) { Ar.SetError(); return; } ActorState(*Actor); }
    else for (auto& [Id, Actor] : GroundActors) { uint32 Key = Id; Ar << Key; ActorState(Actor); }
}
