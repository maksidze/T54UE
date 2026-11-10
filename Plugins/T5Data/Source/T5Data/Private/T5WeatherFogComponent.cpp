#include "T5WeatherFogComponent.h"
#include "Engine/World.h"
#include "SceneInterface.h"
#include "StateStream/ExponentialHeightFogStateStream.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"

void UT5WeatherFogComponent::RefreshWeather()
{
    auto* World=GetWorld();
    if(!World || !World->Scene) return;
    World->Scene->RemoveExponentialHeightFog(uint64(this));
    if(!IsRegistered() || !ShouldRender() || FogDensity<=0 || FogMaxOpacity<=0) return;
    FExponentialHeightFogDynamicState State;
    State.SetFogDensity(FogDensity);
    State.SetFogHeightFalloff(FogHeightFalloff);
    State.SetSecondFogData(SecondFogData);
    State.SetHeight(GetComponentLocation().Z);
    State.SetFogInscatteringLuminance(FogInscatteringLuminance);
    State.SetSkyAtmosphereAmbientContributionColorScale(SkyAtmosphereAmbientContributionColorScale);
    State.SetVolumetricFogEmissive(FogInscatteringLuminance);
    State.SetDirectionalInscatteringLuminance(DirectionalInscatteringLuminance);
    State.SetDirectionalInscatteringExponent(DirectionalInscatteringExponent);
    State.SetDirectionalInscatteringStartDistance(DirectionalInscatteringStartDistance);
    State.SetFogMaxOpacity(FogMaxOpacity);
    State.SetStartDistance(StartDistance);
    State.SetEndDistance(EndDistance);
    State.SetFogCutoffDistance(FogCutoffDistance);
    State.SetRenderInMainPass(true);
    State.SetVisibleInReflectionCaptures(true);
    State.SetVisibleInRealTimeSkyCaptures(true);
    if(FParse::Param(FCommandLine::Get(),TEXT("T5WeatherFogDiagnostic"))) {
        State.SetFogDensity(10.f);
        State.SetFogInscatteringLuminance(FLinearColor(100000,0,0));
        State.SetVolumetricFogEmissive(FLinearColor(100000,0,0));
        State.SetSkyAtmosphereAmbientContributionColorScale(FLinearColor::Black);
    }
    World->Scene->AddExponentialHeightFog(uint64(this),State);
}

void UT5WeatherFogComponent::CreateRenderState_Concurrent(FRegisterComponentContext* Context)
{
    USceneComponent::CreateRenderState_Concurrent(Context);
    RefreshWeather();
}
void UT5WeatherFogComponent::SendRenderTransform_Concurrent()
{
    USceneComponent::SendRenderTransform_Concurrent();
    RefreshWeather();
}
void UT5WeatherFogComponent::DestroyRenderState_Concurrent()
{
    if(auto* World=GetWorld();World && World->Scene) World->Scene->RemoveExponentialHeightFog(uint64(this));
    USceneComponent::DestroyRenderState_Concurrent();
}
