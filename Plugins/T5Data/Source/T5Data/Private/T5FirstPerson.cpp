#include "T5Scene.h"
#include "T5Bridge.h"
#include "T5Hkx.h"
#include "T5PlayerPawn.h"

#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "GameFramework/PlayerController.h"

namespace
{
    // WEAP DNAM selects an animation family. The names are the source clip stems.
    struct FViewClips
    {
        const TCHAR* Idle;
        const TCHAR* Walk;
        const TCHAR* Draw;
        const TCHAR* Holster;
        const TCHAR* Attack;
    };

    const FViewClips& ClipsFor(int32 Type)
    {
        static constexpr FViewClips Unarmed{
            TEXT("h2h_idle"), TEXT("mt_walkforward"), TEXT("h2h_equip"), TEXT("h2h_unequip"), TEXT("h2h_attackright")};
        static constexpr FViewClips OneHand{
            TEXT("1hm_idle"), TEXT("1hm_walkforward"), TEXT("1hm_equip"), TEXT("1hm_unequip"), TEXT("1hm_attackright")};
        static constexpr FViewClips TwoHand{
            TEXT("2hm_idle"), TEXT("2hm_walkforward"), TEXT("2hw_equip"), TEXT("2hw_unequip"), TEXT("2hm_attackright")};
        static constexpr FViewClips TwoHandHeavy{
            TEXT("2hm_idle"), TEXT("2hm_walkforward"), TEXT("2hw_equip"), TEXT("2hw_unequip"), TEXT("2hw_attackright")};
        static constexpr FViewClips Bow{
            TEXT("bow_idleheld"), TEXT("bow_walkforward"), TEXT("bow_equip"), TEXT("bow_unequip"), TEXT("bow_release")};
        static constexpr FViewClips Crossbow{TEXT("dlc01/crossbow_idleheld"), TEXT("dlc01/crossbow_walkforward"),
            TEXT("dlc01/crossbow_equip"), TEXT("dlc01/crossbow_unequip"), TEXT("dlc01/crossbow_release")};
        switch (Type)
        {
        case 1: case 2: case 3: case 4: return OneHand;
        case 5: return TwoHand;
        case 6: return TwoHandHeavy;
        case 7: return Bow;
        case 9: return Crossbow;
        default: return Unarmed;
        }
    }
}

void AT5Scene::DestroyFirstPersonView()
{
    if (auto* View = FirstPersonActor.Get()) View->Destroy();
    FirstPersonActor.Reset();
    FirstPersonArms.Reset();
    FirstPersonWeapon.Reset();
    FirstPersonKey = 0;
    FirstPersonClip.Reset();
    MissingViewClips.Reset();
    bViewDrawn = bViewTransition = bViewWeaponVisible = false;
    ViewScriptSerial = 0;
}

bool AT5Scene::PlayFirstPersonClip(const FString& Clip, bool bLoop)
{
    auto* Mesh = FirstPersonArms.Get();
    if (!Mesh || Clip.IsEmpty() || MissingViewClips.Contains(Clip)) return false;
    if (FirstPersonClip == Clip && bLoop) return true;
    const FString Path = TEXT("meshes/actors/character/_1stperson/animations/") + Clip + TEXT(".hkx");
    const FString Skeleton = TEXT("meshes/actors/character/_1stperson/characterassets/skeletonfirst.hkx");
    const TWeakObjectPtr<AT5Scene> Weak(this);
    FString Error;
    if (!PlayT5Hkx(*Mesh, {}, {}, Path, [this](const FString& Name, TArray<uint8>& Bytes) {
            return Papyrus->ReadResource(Name, Bytes);
        }, Error, Skeleton, [Weak](const FString& Event) {
            if (auto* Scene = Weak.Get())
            {
                if (Event.Equals(TEXT("weaponDraw"), ESearchCase::IgnoreCase)) Scene->bViewWeaponVisible = true;
                if (Event.Equals(TEXT("weaponSheathe"), ESearchCase::IgnoreCase)) Scene->bViewWeaponVisible = false;
                if (Event == TEXT("T5ClipEnd"))
                {
                    Scene->bViewTransition = false;
                    Scene->bViewWeaponVisible = Scene->bViewDrawn;
                    Scene->FirstPersonClip.Reset();
                }
            }
        }, 1.f, !bLoop, bLoop))
    {
        MissingViewClips.Add(Clip);
        UE_LOG(LogTemp, Warning, TEXT("T5_VIEW_CLIP_UNAVAILABLE %s %s"), *Clip, *Error);
        return false;
    }
    FirstPersonClip = Clip;
    bViewTransition = !bLoop;
    UE_LOG(LogTemp, Display, TEXT("T5_VIEW_CLIP %s loop=%d"), *Clip, bLoop);
    return true;
}

void AT5Scene::PlayViewAttack()
{
    if (bViewDrawn)
        PlayFirstPersonClip(ClipsFor(FirstPersonWeaponType).Attack, false);
}

void AT5Scene::TickFirstPerson()
{
    const auto* Controller = GetWorld()->GetFirstPlayerController();
    const auto* Pawn = Controller ? Controller->GetPawnOrSpectator() : nullptr;
    const auto* Character = Cast<AT5PlayerPawn>(Pawn);
    if (!Papyrus || !Pawn || Controller->GetViewTarget() != Pawn || (bSceneLoading && bFullLoadingScreen) || bStartupPreparing
        || CharacterCreator || (Character && Character->IsThirdPerson()) || !(AppliedGameFlags & 1u))
    {
        if (auto* View = FirstPersonActor.Get()) View->SetActorHiddenInGame(true);
        return;
    }
    const auto* Script = (AppliedControls & (1u << 1)) ? Papyrus->AnimationRequests().Find(0x14) : nullptr;
    const bool bScripted = Script && !Script->Clip.IsEmpty();
    bool bDrawn = false;
    const uint64 Key = Papyrus->PlayerViewKey(bDrawn);
    if (!Key || (!bDrawn && !bScripted && !FirstPersonActor.IsValid())) return;
    if (Key != FirstPersonKey)
    {
        if (bSceneLoading) return;
        const bool bWasDrawn = bViewDrawn;
        DestroyFirstPersonView();
        FirstPersonKey = Key;
        if (const auto Data = Papyrus->PlayerView(); Data && BuildFirstPersonView(*Data))
            bViewDrawn = bViewWeaponVisible = bWasDrawn && bDrawn;
        else
            UE_LOG(LogTemp, Warning, TEXT("T5_VIEW_BUILD_ERROR %s"), Data ? *Data->Error : TEXT("Bridge unavailable"));
    }
    auto* View = FirstPersonActor.Get();
    auto* Mesh = FirstPersonArms.Get();
    if (!View || !Mesh) return;
    const auto& Clips = ClipsFor(FirstPersonWeaponType);
    if (bScripted && Script->Serial != ViewScriptSerial)
    {
        ViewScriptSerial = Script->Serial;
        FString Clip = Script->Clip;
        Clip.ReplaceInline(TEXT("\\"), TEXT("/"));
        PlayFirstPersonClip(FPaths::GetBaseFilename(Clip), false);
    }
    if (!bScripted)
    {
        if (ViewScriptSerial)
        {
            ViewScriptSerial = 0;
            bViewTransition = false;
        }
        if (bDrawn != bViewDrawn)
        {
            bViewDrawn = bDrawn;
            if (!PlayFirstPersonClip(bDrawn ? Clips.Draw : Clips.Holster, false))
                bViewWeaponVisible = bDrawn;
        }
        if (bDrawn && !bViewTransition)
        {
            const bool bMoving = Pawn->GetVelocity().SizeSquared2D() > FMath::Square(20.);
            if (!PlayFirstPersonClip(bMoving ? Clips.Walk : Clips.Idle, true) && bMoving)
                PlayFirstPersonClip(Clips.Idle, true);
        }
    }
    const bool bVisible = bViewDrawn || bViewTransition;
    View->SetActorHiddenInGame(!bVisible);
    if (!bVisible) return;
    if (auto* Weapon = FirstPersonWeapon.Get()) Weapon->SetVisibility(bViewWeaponVisible && !bScripted);
    FVector Eye;
    FRotator Aim;
    Controller->GetPlayerViewPoint(Eye, Aim);
    const FName Camera(TEXT("Camera1st [Cam1]"));
    const auto CameraPose = Mesh->GetSocketTransform(Camera, RTS_Component);
    View->SetActorTransform(CameraPose.Inverse() * FTransform(Aim, Eye));
}
