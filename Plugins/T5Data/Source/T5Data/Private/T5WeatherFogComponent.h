#pragma once

#include "Components/ExponentialHeightFogComponent.h"
#include "T5WeatherFogComponent.generated.h"

// UE 5.8's ExponentialHeightFogStateStream Render_OnUpdate is empty. Use
// FSceneInterface's supported legacy submission for changing WTHR parameters.
UCLASS(Transient)
class UT5WeatherFogComponent : public UExponentialHeightFogComponent
{
    GENERATED_BODY()
public:
    void RefreshWeather();
protected:
    void CreateRenderState_Concurrent(FRegisterComponentContext* Context) override;
    void SendRenderTransform_Concurrent() override;
    void DestroyRenderState_Concurrent() override;
};
