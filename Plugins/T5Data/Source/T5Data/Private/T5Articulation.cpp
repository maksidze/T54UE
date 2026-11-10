#include "T5Articulation.h"
#include "T5Coordinates.h"
#include "Components/StaticMeshComponent.h"
#include "Dom/JsonObject.h"
#include "Engine/World.h"
#include "Engine/OverlapResult.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/Paths.h"
#include "PhysicsEngine/BodyInstance.h"
#include "PhysicsEngine/PhysicsSettings.h"
#include "PhysicsEngine/PhysicsConstraintComponent.h"
#include "Physics/PhysicsInterfaceCore.h"
#include "Physics/PhysicsInterfaceUtils.h"
#include "PhysicalMaterials/PhysicalMaterial.h"

DEFINE_LOG_CATEGORY_STATIC(LogT5Articulation, Log, All);
namespace
{
    FVector Vec(const TArray<TSharedPtr<FJsonValue>>& V) { return FVector(V[0]->AsNumber(), V[1]->AsNumber(), V[2]->AsNumber()); }
    FTransform Rest(const TArray<TSharedPtr<FJsonValue>>& R)
    {
        return FTransform(T5ToUnrealRotation(FQuat(R[3]->AsNumber(), R[4]->AsNumber(), R[5]->AsNumber(), R[6]->AsNumber())),
            T5ToUnrealPosition(Vec(R)), FVector(R[7]->AsNumber()));
    }
    FTransform Frame(const FJsonObject& F, double Scale)
    {
        // Rotation axes are axial vectors under our reflection; offsets are polar vectors.
        const FVector Axis = -T5ToUnrealDirection(Vec(F.GetArrayField(TEXT("axis"))));
        const FVector Perp = T5ToUnrealDirection(Vec(F.GetArrayField(TEXT("perpendicular"))));
        return FTransform(FRotationMatrix::MakeFromXY(Axis, Perp).ToQuat(),
            T5ToUnrealPosition(Vec(F.GetArrayField(TEXT("pivot")))) * Scale);
    }
    void PrincipalInertia(const TArray<TSharedPtr<FJsonValue>>& Values, FVector& Diagonal, FQuat& Rotation)
    {
        double A[3][3], V[3][3] = {{1,0,0},{0,1,0},{0,0,1}};
        for (int I=0; I<3; ++I) for (int J=0; J<3; ++J)
            A[I][J] = Values[3*T5AxisForUnreal(I)+T5AxisForUnreal(J)]->AsNumber() * 10000.; // kg m² -> kg cm²
        for (int Iter=0; Iter<32; ++Iter)
        {
            int P=0,Q=1;
            for (int I=0; I<3; ++I) for (int J=I+1; J<3; ++J) if (FMath::Abs(A[I][J]) > FMath::Abs(A[P][Q])) { P=I; Q=J; }
            if (FMath::Abs(A[P][Q]) < 1.e-8) break;
            const double Angle=.5*FMath::Atan2(2*A[P][Q],A[Q][Q]-A[P][P]), C=FMath::Cos(Angle), S=FMath::Sin(Angle);
            const double PP=A[P][P], QQ=A[Q][Q], PQ=A[P][Q];
            A[P][P]=C*C*PP-2*S*C*PQ+S*S*QQ; A[Q][Q]=S*S*PP+2*S*C*PQ+C*C*QQ; A[P][Q]=A[Q][P]=0;
            for (int I=0; I<3; ++I)
            {
                if (I!=P && I!=Q) { const double X=A[I][P],Y=A[I][Q]; A[I][P]=A[P][I]=C*X-S*Y; A[I][Q]=A[Q][I]=S*X+C*Y; }
                const double X=V[I][P],Y=V[I][Q]; V[I][P]=C*X-S*Y; V[I][Q]=S*X+C*Y;
            }
        }
        Diagonal = FVector(A[0][0],A[1][1],A[2][2]);
        const FMatrix Basis(FPlane(V[0][0],V[1][0],V[2][0],0),FPlane(V[0][1],V[1][1],V[2][1],0),
            FPlane(V[0][2],V[1][2],V[2][2],0),FPlane(0,0,0,1));
        Rotation = FQuat(Basis).GetNormalized();
    }
    UPhysicsConstraintComponent* Joint(AActor& Owner)
    {
        auto* C = NewObject<UPhysicsConstraintComponent>(&Owner, NAME_None, RF_Transient);
        C->SetDisableCollision(true);
        // No artificial spring, drive, mass conditioning or positional teleport correction.
        C->ConstraintInstance.ProfileInstance.bEnableProjection = false;
        C->ConstraintInstance.ProfileInstance.bEnableMassConditioning = false;
        C->RegisterComponent();
        return C;
    }
}

bool FT5Articulation::Load(AActor& InOwner, const FJsonObject& Model, double Scale,
    TFunctionRef<UStaticMeshComponent*(const TArray<TSharedPtr<FJsonValue>>&)> MakeBody)
{
    Owner = &InOwner; ModelScale = Scale; Root = Model.GetStringField(TEXT("root"));
    for (const auto& Value : Model.GetArrayField(TEXT("bodies")))
    {
        const auto& Row = *Value->AsObject();
        auto* Component = MakeBody(Row.GetArrayField(TEXT("hulls")));
        if (!Component) return false;
        auto& B = Bodies.Add(Row.GetStringField(TEXT("node")));
        B.Component = Component; B.Rest = Rest(Row.GetArrayField(TEXT("rest"))) * FTransform(FQuat::Identity,FVector::ZeroVector,FVector(Scale));
        B.Mass=Row.GetNumberField(TEXT("mass")); B.Gravity=Row.GetNumberField(TEXT("gravity"));
        B.Center=T5ToUnrealPosition(Vec(Row.GetArrayField(TEXT("center"))))*Scale;
        PrincipalInertia(Row.GetArrayField(TEXT("inertia")),B.Inertia,B.MassRotation);
        B.Inertia *= Scale*Scale;
        if (B.Mass<=0 || B.Inertia.GetMin()<=0) { UE_LOG(LogT5Articulation, Error, TEXT("Invalid NIF body inertia/mass")); return false; }
        Component->SetLinearDamping(Row.GetNumberField(TEXT("linearDamping")));
        Component->SetAngularDamping(Row.GetNumberField(TEXT("angularDamping")));
        auto* Material=NewObject<UPhysicalMaterial>(&InOwner, NAME_None, RF_Transient);
        Material->Friction=Row.GetNumberField(TEXT("friction")); Material->Restitution=Row.GetNumberField(TEXT("restitution"));
        Component->SetPhysMaterialOverride(Material);
        Component->BodyInstance.bUseCCD = true;
        Component->BodyInstance.SetOverrideIterationCounts(true);
        Component->BodyInstance.SetPositionSolverIterationCount(16);
        Component->BodyInstance.SetVelocitySolverIterationCount(8);
    }
    for (const auto& Value : Model.GetArrayField(TEXT("joints")))
    {
        const auto& Row=*Value->AsObject(); const FString Type=Row.GetStringField(TEXT("type"));
        if (Type!=TEXT("bhkHingeConstraint") && Type!=TEXT("bhkLimitedHingeConstraint"))
        { UE_LOG(LogT5Articulation, Error, TEXT("Unsupported NIF joint %s"),*Type); return false; }
        auto& J=Joints.AddDefaulted_GetRef(); J.A=Row.GetStringField(TEXT("a")); J.B=Row.GetStringField(TEXT("b"));
        J.FrameA=Frame(*Row.GetObjectField(TEXT("frameA")),Scale); J.FrameB=Frame(*Row.GetObjectField(TEXT("frameB")),Scale);
        auto* C=Joint(InOwner); J.Component=C;
        C->SetLinearXLimit(LCM_Locked,0); C->SetLinearYLimit(LCM_Locked,0); C->SetLinearZLimit(LCM_Locked,0);
        C->SetAngularSwing1Limit(ACM_Locked,0); C->SetAngularSwing2Limit(ACM_Locked,0);
        if (Type==TEXT("bhkHingeConstraint"))
        { C->SetAngularTwistLimit(ACM_Free,0); Bodies.FindChecked(J.A).bWheel=true; }
        else
        {
            const double Min=Row.GetNumberField(TEXT("min")),Max=Row.GetNumberField(TEXT("max"));
            C->SetAngularTwistLimit(ACM_Limited,FMath::RadiansToDegrees((Max-Min)*.5));
            J.FrameB.SetRotation(J.FrameB.GetRotation()*FQuat(FVector::ForwardVector,(Max+Min)*.5));
            if (J.B==Root) HitchNode=HitchNode.IsEmpty()?J.A:TEXT("ambiguous");
        }
    }
    if (!Bodies.Contains(Root)) return false;
    // A noncolliding kinematic anchor reuses an authored hull; no substitute collision shape.
    Anchor=MakeBody(Model.GetArrayField(TEXT("bodies"))[0]->AsObject()->GetArrayField(TEXT("hulls")));
    return Anchor.IsValid();
}

void FT5Articulation::Configure(const FString& Data, const FString& Plugins)
{
    // Defaults verified in the source executable's INISettingCollection, then overridden by game/profile INIs.
    for (const FString& Path : {FPaths::GetPath(Data)/TEXT("Skyrim.ini"),FPaths::GetPath(Data)/TEXT("SkyrimCustom.ini"),FPaths::GetPath(Plugins)/TEXT("T5.ini")})
    {
        FConfigFile Config; Config.Read(Path);
        auto Number=[&](const TCHAR* Key,double& Value) { double V; if (Config.GetDouble(TEXT("Cart"),Key,V) && FMath::IsFinite(V)) Value=V; };
        Number(TEXT("fCartPivotX"),CartPivot.X); Number(TEXT("fCartPivotY"),CartPivot.Y); Number(TEXT("fCartPivotZ"),CartPivot.Z);
        Number(TEXT("fHorsePivotX"),HorsePivot.X); Number(TEXT("fHorsePivotY"),HorsePivot.Y); Number(TEXT("fHorsePivotZ"),HorsePivot.Z);
        Number(TEXT("fCartLimitMin"),LimitMin); Number(TEXT("fCartLimitMax"),LimitMax);
        Number(TEXT("fCartRot1"),CartRot); Number(TEXT("fCartRot2"),HorseRot);
        Number(TEXT("fGravMult"),WheelGravity); Number(TEXT("fFriction"),WheelFriction); Number(TEXT("fWheelAngDamp"),WheelAngularDamping);
        Config.GetString(TEXT("Cart"),TEXT("sHorseConnect"),HorseBone);
    }
}

void FT5Articulation::ApplyMass(FBody& B)
{
    auto* C=B.Component.Get(); if (!C) return;
    C->SetMassOverrideInKg(NAME_None,B.Mass,true);
    C->BodyInstance.SetMassSpaceLocal(FTransform(B.MassRotation,B.Center));
    FPhysicsCommand::ExecuteWrite(C->BodyInstance.ActorHandle,[&](const FPhysicsActorHandle& Handle) {
        FPhysicsInterface::SetMassSpaceInertiaTensor_AssumesLocked(Handle,B.Inertia);
    });
}

void FT5Articulation::Start(const FTransform& ModelWorld)
{
    if (bStarted) return;
    for (auto& [Name,B] : Bodies)
    {
        auto* C=B.Component.Get(); C->SetWorldTransform(B.Rest*ModelWorld,false,nullptr,ETeleportType::TeleportPhysics);
        C->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics); C->SetSimulatePhysics(true); ApplyMass(B);
    }
    for (auto& J : Joints)
    {
        auto* C=J.Component.Get(); C->SetConstrainedComponents(Bodies.FindChecked(J.A).Component.Get(),NAME_None,Bodies.FindChecked(J.B).Component.Get(),NAME_None);
        C->SetConstraintReferenceFrame(EConstraintFrame::Frame1,J.FrameA); C->SetConstraintReferenceFrame(EConstraintFrame::Frame2,J.FrameB);
    }
    // Source controller gives the assembly a no-self-collision group. These
    // all-free joints register pair filters only; they exert no constraint forces.
    if (!bCollisionFiltersCreated)
    for (auto& [Name,B] : Bodies) for (auto& [Other,D] : Bodies) if (Name.Compare(Other)<0)
    {
        if (Joints.ContainsByPredicate([&](const auto& J) { return (J.A==Name && J.B==Other) || (J.A==Other && J.B==Name); })) continue;
        auto* Filter=Joint(*Owner.Get());
        Filter->SetLinearXLimit(LCM_Free,0); Filter->SetLinearYLimit(LCM_Free,0); Filter->SetLinearZLimit(LCM_Free,0);
        Filter->SetAngularTwistLimit(ACM_Free,0); Filter->SetAngularSwing1Limit(ACM_Free,0); Filter->SetAngularSwing2Limit(ACM_Free,0);
        Filter->SetConstrainedComponents(B.Component.Get(),NAME_None,D.Component.Get(),NAME_None);
    }
    bCollisionFiltersCreated=true;

    for (const auto& [Name,B]:Bodies)
    {
        TArray<FOverlapResult> Overlaps; FComponentQueryParams Q; Q.AddIgnoredActor(Owner.Get());
        B.Component->ComponentOverlapMulti(Overlaps,Owner->GetWorld(),B.Component->GetComponentLocation(),B.Component->GetComponentQuat(),ECC_WorldDynamic,Q);
        for (const auto& H:Overlaps) if (auto* Other=H.GetComponent())
            UE_LOG(LogT5Articulation,Display,TEXT("T5_BODY_INITIAL_OVERLAP body=%s component=%s tags=%s parent=%s"),*Name,*Other->GetName(),
                *FString::JoinBy(Other->ComponentTags,TEXT("|"),[](FName N){return N.ToString();}),*GetNameSafe(Other->GetAttachParent()));
    }
    bStarted=true;
    UE_LOG(LogT5Articulation,Display,TEXT("T5_NIF_ASSEMBLY root=%s bodies=%d joints=%d"),*Root,Bodies.Num(),Joints.Num());
    UE_LOG(LogT5Articulation,Display,TEXT("T5_PHYSICS_STEP max=%.4f substep=%.5f count=%d enabled=%d initialJointError=%.4f root=%s"),
        UPhysicsSettings::Get()->MaxPhysicsDeltaTime,UPhysicsSettings::Get()->MaxSubstepDeltaTime,UPhysicsSettings::Get()->MaxSubsteps,
        UPhysicsSettings::Get()->bSubstepping,JointError(),*ModelWorld.ToHumanReadableString());
}

bool FT5Articulation::Attach(const FTransform& AnchorWorld)
{
    if (!bStarted || !Bodies.Contains(HitchNode) || !Anchor.IsValid()) return false;
    Anchor->SetWorldTransform(AnchorWorld,false,nullptr,ETeleportType::TeleportPhysics);
    Anchor->SetCollisionEnabled(ECollisionEnabled::PhysicsOnly); Anchor->SetCollisionResponseToAllChannels(ECR_Ignore);
    auto* C=Joint(*Owner.Get()); Hitch=C;
    const auto Basis=[](const FVector& Axis) { return FRotationMatrix::MakeFromXY(T5ToUnrealDirection(Axis),T5ToUnrealDirection(FVector::ForwardVector)).ToQuat(); };
    const FVector CartAxis=FQuat(FVector::ForwardVector,FMath::DegreesToRadians(CartRot)).RotateVector(FVector::UpVector);
    const FVector HorseAxis=FQuat(FVector::ForwardVector,FMath::DegreesToRadians(HorseRot)).RotateVector(FVector::RightVector);
    HitchA=FTransform(Basis(CartAxis),T5ToUnrealDirection(CartPivot)*100.*ModelScale);
    HitchB=FTransform(Basis(HorseAxis),T5ToUnrealDirection(HorsePivot)*100.);
    HitchB.AddToTranslation(HitchB.TransformVectorNoScale(FVector((LimitMin+LimitMax)*50.,0,0)));
    C->SetLinearXLimit(LCM_Limited,(LimitMax-LimitMin)*50.); C->SetLinearYLimit(LCM_Locked,0); C->SetLinearZLimit(LCM_Locked,0);
    // Source tether explicitly sets m_atoms.m_ang.m_numConstrainedAxes=0
    // (hkpPrismaticConstraintData +32 +176 +3). Its linear shaft is a line,
    // not an orientation lock: HorseSpine2's shaft points opposite the cart's.
    C->SetAngularTwistLimit(ACM_Free,0); C->SetAngularSwing1Limit(ACM_Free,0); C->SetAngularSwing2Limit(ACM_Free,0);
    C->SetConstrainedComponents(Bodies.FindChecked(HitchNode).Component.Get(),NAME_None,Anchor.Get(),NAME_None);
    C->SetConstraintReferenceFrame(EConstraintFrame::Frame1,HitchA); C->SetConstraintReferenceFrame(EConstraintFrame::Frame2,HitchB);
    for (auto& [Name,B] : Bodies) if (B.bWheel)
    {
        B.Component->SetAngularDamping(WheelAngularDamping);
        B.Component->BodyInstance.GetSimplePhysicalMaterial()->Friction=WheelFriction;
        B.Component->BodyInstance.UpdatePhysicalMaterials();
    }
    bAttached=true;
    const auto WA=HitchA*Bodies.FindChecked(HitchNode).Component->GetComponentTransform(),WB=HitchB*AnchorWorld;
    UE_LOG(LogT5Articulation,Display,TEXT("T5_HITCH_FRAMES linear=%.3f angular=%.3f cart=%s horse=%s jointError=%.3f"),
        FVector::Dist(WA.GetLocation(),WB.GetLocation()),FMath::RadiansToDegrees(WA.GetRotation().AngularDistance(WB.GetRotation())),
        *WA.ToHumanReadableString(),*WB.ToHumanReadableString(),JointError());
    UE_LOG(LogT5Articulation,Display,TEXT("T5_NIF_HITCH node=%s bone=%s prismatic=%.3f..%.3fm"),*HitchNode,*HorseBone,LimitMin,LimitMax);
    return true;
}

void FT5Articulation::Detach()
{
    if (auto* C=Hitch.Get()) C->DestroyComponent();
    Hitch.Reset(); bAttached=false;
    if (Anchor.IsValid()) Anchor->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
}

void FT5Articulation::Drive(const FTransform& AnchorWorld)
{
    if (bAttached && !bPaused) Anchor->SetWorldTransform(AnchorWorld); // kinematic target, interpolated by Chaos substeps
}

void FT5Articulation::Tick()
{
    if (!bStarted || bPaused || !Owner.IsValid()) return;
    for (auto& [Name,B] : Bodies)
    {
        const double ExtraGravity=(bAttached && B.bWheel ? WheelGravity : 0.) + B.Gravity-1.;
        if (ExtraGravity!=0) B.Component->AddForce(FVector(0,0,Owner->GetWorld()->GetGravityZ()*ExtraGravity*B.Mass));
    }
}

void FT5Articulation::SetPaused(bool Paused)
{
    const bool bKeyframed=MotionType==4 || MotionType==5;
    Paused |= bKeyframed;
    if (!bStarted || bPaused==Paused) return;
    for (auto& [Name,B] : Bodies)
    {
        if (Paused) { B.SavedLinear=bKeyframed?FVector::ZeroVector:B.Component->GetPhysicsLinearVelocity(); B.SavedAngular=bKeyframed?FVector::ZeroVector:B.Component->GetPhysicsAngularVelocityInRadians(); }
        B.Component->SetSimulatePhysics(!Paused);
        if (!Paused) { ApplyMass(B); B.Component->SetPhysicsLinearVelocity(B.SavedLinear); B.Component->SetPhysicsAngularVelocityInRadians(B.SavedAngular); }
    }
    bPaused=Paused;
}

FTransform FT5Articulation::Pose() const
{
    const auto& B=Bodies.FindChecked(Root);
    return B.Rest.Inverse()*B.Component->GetComponentTransform();
}
TOptional<FTransform> FT5Articulation::NodePose(const FString& Node) const
{
    const auto* B=Bodies.Find(Node); return bStarted && B ? TOptional<FTransform>(B->Component->GetComponentTransform()) : TOptional<FTransform>{};
}
double FT5Articulation::JointError() const
{
    double Max=0;
    for (const auto& J : Joints) Max=FMath::Max(Max,FVector::Dist((J.FrameA*Bodies.FindChecked(J.A).Component->GetComponentTransform()).GetLocation(),
        (J.FrameB*Bodies.FindChecked(J.B).Component->GetComponentTransform()).GetLocation()));
    return Max;
}
TOptional<FTransform> FT5Articulation::NodeDelta(const FString& Node, const FTransform& Authored, const FTransform& Origin) const
{
    const auto* B=Bodies.Find(Node);
    if (!bStarted || !B) return {};
    return (B->Rest*Authored).Inverse()*(B->Component->GetComponentTransform()*Origin.Inverse());
}
void FT5Articulation::CopyState(const FT5Articulation& Previous)
{
    if (!Previous.bStarted || Bodies.Num()!=Previous.Bodies.Num()) return;
    for (const auto& [Name,B] : Bodies) if (!Previous.Bodies.Contains(Name)) return;
    Start(Previous.Pose());
    for (auto& [Name,B] : Bodies)
    {
        const auto& Old=Previous.Bodies.FindChecked(Name);
        B.Component->SetWorldTransform(Old.Component->GetComponentTransform(),false,nullptr,ETeleportType::TeleportPhysics);
        B.Component->SetPhysicsLinearVelocity(Previous.bPaused?Old.SavedLinear:Old.Component->GetPhysicsLinearVelocity());
        B.Component->SetPhysicsAngularVelocityInRadians(Previous.bPaused?Old.SavedAngular:Old.Component->GetPhysicsAngularVelocityInRadians());
    }
    MotionType=Previous.MotionType;
    if (Previous.bAttached) Attach(Previous.Anchor->GetComponentTransform());
    SetPaused(Previous.bPaused);
}

void FT5Articulation::SerializeCheckpoint(FArchive& Ar)
{
    bool Started = bStarted, Attached = bAttached;
    Ar << Started << Attached << MotionType;
    FTransform RootPose = bStarted ? Pose() : FTransform::Identity;
    FTransform AnchorPose = Anchor.IsValid() ? Anchor->GetComponentTransform() : FTransform::Identity;
    Ar << RootPose << AnchorPose;
    if (Ar.IsLoading() && Started) Start(RootPose);
    int32 Count = Bodies.Num(); Ar << Count;
    if (Count < 0 || Count > 4096) { Ar.SetError(); return; }
    const auto BodyState = [&](FBody& Body) {
        FTransform Pose = Body.Component->GetComponentTransform();
        FVector Linear = bPaused ? Body.SavedLinear : Body.Component->GetPhysicsLinearVelocity();
        FVector Angular = bPaused ? Body.SavedAngular : Body.Component->GetPhysicsAngularVelocityInRadians();
        Ar << Pose << Linear << Angular;
        if (Ar.IsLoading())
        {
            Body.Component->SetWorldTransform(Pose, false, nullptr, ETeleportType::TeleportPhysics);
            Body.SavedLinear = Linear; Body.SavedAngular = Angular;
            Body.Component->SetPhysicsLinearVelocity(Linear); Body.Component->SetPhysicsAngularVelocityInRadians(Angular);
        }
    };
    if (Ar.IsLoading())
        for (int32 I = 0; I < Count && !Ar.IsError(); ++I)
        {
            FString Name; Ar << Name;
            auto* Body = Bodies.Find(Name);
            if (!Body) { Ar.SetError(); return; } BodyState(*Body);
        }
    else for (auto& [Name, Body] : Bodies) { FString Key = Name; Ar << Key; BodyState(Body); }
    if (Ar.IsLoading() && Started)
    {
        if (Attached) Attach(AnchorPose);
        SetPaused(true);
    }
}
