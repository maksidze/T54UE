#include "T5Scene.h"
#include "Widgets/Notifications/SProgressBar.h"
#include "Widgets/Layout/SBox.h"
#include "T5FrameTask.h"
#include "T5Lip.h"
#include "UObject/StrongObjectPtr.h"

#include "T5Audio.h"
#include "T5Archery.h"
#include "T5Bridge.h"
#include "T5Mechanics.h"
#include "PhysicsEngine/PhysicsSettings.h"
#include "T5Hkx.h"
#include "T5HkxAnimInstance.h"
#include "T5HkxCamera.h"
#include "Camera/PlayerCameraManager.h"
#include "Misc/ConfigCacheIni.h"
#include "T5ReferenceAnimation.h"
#include "T5MapBrowser.h"
#include "T5CharacterCreator.h"
#include "T5SkeletalMesh.h"
#include "T5MaterialSemantics.h"
#include "T5Titles.h"
#include "T5Compass.h"
#include "T5UIResources.h"
#include "T5PlayerPawn.h"

#include "Async/Async.h"
#include "Components/InputComponent.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/ExponentialHeightFogComponent.h"
#include "T5WeatherFogComponent.h"
#include "Components/PointLightComponent.h"
#include "Components/PostProcessComponent.h"
#include "Components/SkyAtmosphereComponent.h"
#include "Components/SkyLightComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "DDSFile.h"
#include "Dom/JsonObject.h"
#include "Engine/StaticMesh.h"
#include "Engine/GameViewportClient.h"
#include "Engine/DirectionalLight.h"
#include "Engine/SkyLight.h"
#include "Engine/Texture2D.h"
#include "Engine/TextureCube.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerInput.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformProperties.h"
#include "GameFramework/SpectatorPawn.h"
#include "GameFramework/PawnMovementComponent.h"
#include "GameFramework/HUD.h"
#include "Hash/CityHash.h"
#include "ImageUtils.h"
#include "Interfaces/IPluginManager.h"
#include "Internationalization/Regex.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Misc/App.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "PhysicsEngine/BodySetup.h"
#include "Serialization/JsonSerializer.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "StaticMeshAttributes.h"
#include "StaticMeshOperations.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/SOverlay.h"
#include "Widgets/Text/STextBlock.h"

#if PLATFORM_WINDOWS
#include "Windows/WindowsHWrapper.h"
#endif

DEFINE_LOG_CATEGORY_STATIC(LogT5Data, Log, All);

namespace
{
    constexpr float CentimetersPerT5Unit = T5CentimetersPerUnit;
    constexpr int32 TerrainLayerLimit = 8;
    constexpr double T5CellSize = 4096.0;

    FString ResolveDataJunction(FString Directory)
    {
#if PLATFORM_WINDOWS
        // The native save revision includes the master paths. A junction to
        // the existing Data directory should keep the original revision.
        FString WindowsPath = Directory;
        WindowsPath.ReplaceCharInline(TEXT('/'), TEXT('\\'));
        const HANDLE Handle = ::CreateFileW(*WindowsPath, FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        if (Handle != INVALID_HANDLE_VALUE)
        {
            FString Resolved;
            const DWORD Capacity = ::GetFinalPathNameByHandleW(Handle, nullptr, 0, FILE_NAME_NORMALIZED);
            Resolved.GetCharArray().SetNumUninitialized(Capacity);
            const DWORD Length = Capacity > 0
                ? ::GetFinalPathNameByHandleW(Handle, Resolved.GetCharArray().GetData(), Capacity, FILE_NAME_NORMALIZED)
                : 0;
            ::CloseHandle(Handle);
            if (Length > 0 && Length < Capacity)
            {
                Resolved.GetCharArray().SetNum(Length + 1);
                if (Resolved.RemoveFromStart(TEXT("\\\\?\\UNC\\")))
                    Resolved = TEXT("\\\\") + Resolved;
                else
                    Resolved.RemoveFromStart(TEXT("\\\\?\\"));
                FPaths::NormalizeDirectoryName(Resolved);
                return Resolved;
            }
        }
#endif
        return Directory;
    }

    void SetStartupDaylight(UWorld* World, bool bShowSky, bool bUseSkyLighting)
    {
        if (!World || !World->IsGameWorld() || !World->GetOutermost()->GetName().StartsWith(TEXT("/T5Data/Runtime/")))
            return;
        // UE's cached lighting covers [EV - 12, EV + 8]. Use a range that
        // contains the exposure limits for the current lighting source.
        if (auto* PreExposure = IConsoleManager::Get().FindConsoleVariable(TEXT("r.EyeAdaptation.CachedLightingPreExposure")))
            PreExposure->Set(bUseSkyLighting ? 8.f : 2.f, ECVF_SetByCode);
        for (TActorIterator<ADirectionalLight> It(World); It; ++It)
            It->GetLightComponent()->SetVisibility(bUseSkyLighting);
        for (TActorIterator<ASkyLight> It(World); It; ++It)
        {
            auto* Sky = Cast<USkyLightComponent>(It->GetLightComponent());
            Sky->SetVisibility(bUseSkyLighting);
            if (bUseSkyLighting)
            {
                Sky->bLowerHemisphereIsBlack = false;
                Sky->SetLowerHemisphereColor(FLinearColor(.18f, .20f, .24f));
                Sky->SetMinOcclusion(.6f);
                Sky->MarkRenderStateDirty();
            }
        }
        for (TActorIterator<ASkyAtmosphere> It(World); It; ++It)
            It->GetComponent()->SetVisibility(bShowSky);
    }

    TSharedPtr<FJsonObject> ReadJson(const FT5LoadResult& Result, const TCHAR* Name)
    {
        if (const auto* Prepared = Result.PreparedJson.Find(Name))
            return *Prepared;
        const auto Json = Result.Read(Name);
        if (Json.IsEmpty() || Json.Num() > MAX_int32)
            return nullptr;
        const FUTF8ToTCHAR Text(reinterpret_cast<const ANSICHAR*>(Json.GetData()), static_cast<int32>(Json.Num()));
        TSharedPtr<FJsonObject> Object;
        return FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(FString(Text.Length(), Text.Get())), Object)
            ? Object : nullptr;
    }

    FVector T5Position(const TArray<TSharedPtr<FJsonValue>>& Values)
    {
        return T5ToUnrealPosition(FVector(Values[0]->AsNumber(), Values[1]->AsNumber(), Values[2]->AsNumber()));
    }

    FIntPoint ReadCell(const TArray<TSharedPtr<FJsonValue>>& Row)
    {
        return FIntPoint(Row[0]->AsNumber(), Row[1]->AsNumber());
    }

    void ReadLocations(const TArray<TSharedPtr<FJsonValue>>& Rows, bool bInterior, TMap<uint32, TSharedPtr<FT5Location>>& Out)
    {
        for (const auto& Value : Rows)
        {
            const auto Row = Value->AsObject();
            auto Location = MakeShared<FT5Location>();
            Location->Id = static_cast<uint32>(Row->GetNumberField(TEXT("id")));
            Location->EditorId = Row->GetStringField(TEXT("editorId"));
            Location->Plugin = Row->GetStringField(TEXT("plugin"));
            Location->bInterior = bInterior;
            if (!bInterior)
            {
                Location->Parent = static_cast<uint32>(Row->GetNumberField(TEXT("parent")));
                Location->Map = static_cast<uint32>(Row->GetNumberField(TEXT("mapWorld")));
                Location->Lod = static_cast<uint32>(Row->GetNumberField(TEXT("lodWorld")));
                Location->Land = static_cast<uint32>(Row->GetNumberField(TEXT("landWorld")));
                const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
                TOptional<FIntPoint> Anchor;
                if (Row->TryGetArrayField(TEXT("center"), Values))
                    Anchor = ReadCell(*Values);
                if (Location->Map == Location->Id && Row->TryGetArrayField(TEXT("mapBounds"), Values))
                {
                    const auto& Bounds = *Values;
                    if (Bounds[0]->AsNumber() != Bounds[2]->AsNumber() && Bounds[1]->AsNumber() != Bounds[3]->AsNumber())
                    {
                        const FIntPoint NW = ReadCell(Bounds);
                        const FIntPoint SE(Bounds[2]->AsNumber(), Bounds[3]->AsNumber());
                        Location->MapBounds = FIntRect(FMath::Min(NW.X, SE.X), FMath::Min(NW.Y, SE.Y),
                            FMath::Max(NW.X, SE.X) + 1, FMath::Max(NW.Y, SE.Y) + 1);
                        if (!Anchor)
                            Anchor = (NW + SE) / 2;
                    }
                }
                if (Row->TryGetArrayField(TEXT("mapOffset"), Values))
                {
                    Location->MapScale = (*Values)[0]->AsNumber();
                    Location->MapOffset = FVector2D((*Values)[1]->AsNumber(), (*Values)[2]->AsNumber()) / T5CellSize;
                }
                FIntPoint Min(MAX_int32, MAX_int32), Max(MIN_int32, MIN_int32);
                TOptional<FIntPoint> Start;
                for (const auto& CellValue : Row->GetArrayField(TEXT("cells")))
                {
                    const auto& CellRow = CellValue->AsArray();
                    const FIntPoint Cell = ReadCell(CellRow);
                    Location->Cells.Add(Cell);
                    Min = FIntPoint(FMath::Min(Cell.X, Min.X), FMath::Min(Cell.Y, Min.Y));
                    Max = FIntPoint(FMath::Max(Cell.X, Max.X), FMath::Max(Cell.Y, Max.Y));
                    const FString Name = CellRow[3]->AsString();
                    if (!Name.IsEmpty())
                        Location->NamedCells.Add(Cell, Name);
                    if (Name.Equals(Location->EditorId + TEXT("Start"), ESearchCase::IgnoreCase)
                        || Name.Equals(Location->EditorId + TEXT("Origin"), ESearchCase::IgnoreCase))
                        Start = Cell;
                }
                if (!Location->Cells.IsEmpty())
                {
                    Location->MinCell = Min;
                    Location->MaxCell = Max;
                    const FIntPoint Center = Anchor.Get(Start.Get((Min + Max) / 2));
                    Location->SuggestedCell = Center;
                    int64 Best = MAX_int64;
                    const auto Closest = [&Best, &Location, Center](FIntPoint Cell)
                    {
                        const int64 DX = int64(Cell.X) - Center.X, DY = int64(Cell.Y) - Center.Y;
                        const int64 Distance = DX * DX + DY * DY;
                        if (Distance < Best)
                        {
                            Best = Distance;
                            Location->SuggestedCell = Cell;
                        }
                    };
                    if (Anchor || Location->NamedCells.IsEmpty())
                        for (FIntPoint Cell : Location->Cells)
                            Closest(Cell);
                    else
                        for (const auto& Pair : Location->NamedCells)
                            Closest(Pair.Key);
                }
            }
            Out.Add(Location->Id, Location);
        }
    }

    EPixelFormat GetPixelFormat(UE::DDS::EDXGIFormat Format)
    {
        using E = UE::DDS::EDXGIFormat;
        switch (Format)
        {
        case E::BC1_UNORM: case E::BC1_UNORM_SRGB: return PF_DXT1;
        case E::BC2_UNORM: case E::BC2_UNORM_SRGB: return PF_DXT3;
        case E::BC3_UNORM: case E::BC3_UNORM_SRGB: return PF_DXT5;
        case E::BC4_UNORM: return PF_BC4;
        case E::BC5_UNORM: return PF_BC5;
        case E::BC7_UNORM: case E::BC7_UNORM_SRGB: return PF_BC7;
        case E::R8G8B8A8_UNORM: case E::R8G8B8A8_UNORM_SRGB: return PF_R8G8B8A8;
        case E::B8G8R8A8_UNORM: case E::B8G8R8A8_UNORM_SRGB: return PF_B8G8R8A8;
        default: return PF_Unknown;
        }
    }

    void FillMip(FTexture2DMipMap& Mip, TConstArrayView64<uint8> Pixels)
    {
        Mip.BulkData.Lock(LOCK_READ_WRITE);
        FMemory::Memcpy(Mip.BulkData.Realloc(Pixels.Num()), Pixels.GetData(), Pixels.Num());
        Mip.BulkData.Unlock();
    }

    FColor AuthoredColor(const TArray<TSharedPtr<FJsonValue>>& RGB)
    {
        return FColor(RGB[0]->AsNumber(), RGB[1]->AsNumber(), RGB[2]->AsNumber());
    }

    FLinearColor AuthoredColor(const FJsonObject& Json, const TCHAR* Name)
    {
        return FLinearColor::FromSRGBColor(AuthoredColor(Json.GetArrayField(Name)));
    }

    void ApplyEnvironment(AActor* Owner, USceneComponent* Root, const TSharedPtr<FJsonObject>& Metadata, bool bInterior)
    {
        const TSharedPtr<FJsonObject>* Environment = nullptr;
        if (Metadata)
            Metadata->TryGetObjectField(TEXT("environment"), Environment);
        const auto* Data = Environment ? Environment->Get() : nullptr;
        const bool bShowSky = Data ? Data->GetIntegerField(TEXT("showSky")) != 0 : !bInterior;
        const bool bUseSky = Data ? Data->GetIntegerField(TEXT("useSkyLighting")) != 0 : !bInterior;
        SetStartupDaylight(Owner->GetWorld(), bShowSky, bUseSky);
        if (Data)
            UE_LOG(LogT5Data, Display, TEXT("T5_ENVIRONMENT cell=%08X template=%08X imageSpace=%08X showSky=%d skyLighting=%d"),
                static_cast<uint32>(Data->GetNumberField(TEXT("cell"))), static_cast<uint32>(Data->GetNumberField(TEXT("template"))),
                static_cast<uint32>(Data->GetNumberField(TEXT("imageSpace"))), bShowSky, bUseSky);
        if (!Metadata)
            return;

        constexpr EObjectFlags ObjectFlags = RF_Transient | RF_DuplicateTransient;
        auto* Exposure = NewObject<UPostProcessComponent>(Owner, NAME_None, ObjectFlags);
        Exposure->SetupAttachment(Root);
        Exposure->bUnbound = true;
        Exposure->Priority = 1000;
        auto& Settings = Exposure->Settings;
        if (!bUseSky)
        {
            // Interiors supply their own unoccluded ambient term in XCLL/LGTM.
            // Deferred skylighting applies that term independently of geometry.
            Settings.bOverride_DynamicGlobalIlluminationMethod = true;
            Settings.DynamicGlobalIlluminationMethod = EDynamicGlobalIlluminationMethod::None;
        }
        // Weather can reduce incident light by several stops; the cooked bootstrap
        // volume's fixed EV14 otherwise crushes every overcast scene to black.
        {
            Settings.bOverride_AutoExposureMinBrightness = true;
            Settings.bOverride_AutoExposureMaxBrightness = true;
            Settings.AutoExposureMinBrightness = bUseSky ? 12.5f : -6;
            Settings.AutoExposureMaxBrightness = bUseSky ? 14 : 10;
            Settings.bOverride_AutoExposureSpeedUp = true;
            Settings.bOverride_AutoExposureSpeedDown = true;
            Settings.AutoExposureSpeedUp = 3;
            Settings.AutoExposureSpeedDown = 1;
            if (bUseSky)
            {
                Settings.bOverride_AutoExposureBias = true;
                Settings.AutoExposureBias = -.2f;
                Settings.bOverride_ColorGammaShadows = true;
                Settings.ColorGammaShadows = FVector4(1,1,1,1.15f);
            }
        }
        if (Data)
        {
            const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
            if (Data->TryGetArrayField(TEXT("cinematic"), Values))
            {
                const auto Global = [](double Value) { return FVector4(1, 1, 1, FMath::Clamp(Value, 0., 4.)); };
                Settings.bOverride_ColorSaturation = true;
                Settings.bOverride_ColorGain = true;
                Settings.bOverride_ColorContrast = true;
                Settings.ColorSaturation = Global((*Values)[0]->AsNumber());
                Settings.ColorGain = Global((*Values)[1]->AsNumber());
                Settings.ColorContrast = Global((*Values)[2]->AsNumber());
            }
            if (Data->TryGetArrayField(TEXT("hdr"), Values))
            {
                Settings.bOverride_AutoExposureSpeedUp = true;
                Settings.bOverride_AutoExposureSpeedDown = true;
                Settings.bOverride_BloomThreshold = true;
                Settings.bOverride_BloomIntensity = true;
                Settings.AutoExposureSpeedUp = Settings.AutoExposureSpeedDown = FMath::Clamp((*Values)[0]->AsNumber(), .02, 20.);
                Settings.BloomThreshold = FMath::Clamp((*Values)[1]->AsNumber(), -1., 20.);
                Settings.BloomIntensity = FMath::Clamp((*Values)[2]->AsNumber(), 0., 8.);
            }
            if (Data->TryGetArrayField(TEXT("tint"), Values))
            {
                Settings.bOverride_SceneColorTint = true;
                Settings.SceneColorTint = FMath::Lerp(FLinearColor::White,
                    FLinearColor((*Values)[1]->AsNumber(), (*Values)[2]->AsNumber(), (*Values)[3]->AsNumber()),
                    FMath::Clamp((*Values)[0]->AsNumber(), 0., 1.));
            }
        }
        Exposure->RegisterComponent();
        if (!Data || !Data->HasField(TEXT("ambient")))
            return;

        if (!bUseSky)
        {
            constexpr int32 CubeSize = 16;
            if (auto* Cube = UTextureCube::CreateTransient(CubeSize, CubeSize))
            {
                const auto& Faces = Data->GetArrayField(TEXT("ambientFaces"));
                const FColor Ambient = AuthoredColor(Data->GetArrayField(TEXT("ambient")));
                TArray<FColor> Pixels;
                Pixels.SetNumUninitialized(CubeSize * CubeSize * 6);
                bool bDirectionalAmbient = false;
                FColor Colors[6];
                for (int32 I = 0; I < 6; ++I)
                {
                    const int32 Index = T5AxisForUnreal(I / 2) * 2 + I % 2;
                    Colors[I] = AuthoredColor(Faces[Index]->AsArray());
                    bDirectionalAmbient |= Colors[I].R || Colors[I].G || Colors[I].B;
                }
                for (int32 I = 0; I < 6; ++I)
                    for (int32 J = 0; J < CubeSize * CubeSize; ++J)
                        Pixels[I * CubeSize * CubeSize + J] = bDirectionalAmbient ? Colors[I] : Ambient;
                FillMip(Cube->GetPlatformData()->Mips[0],
                    {reinterpret_cast<const uint8*>(Pixels.GetData()), int64(Pixels.Num()) * int64(sizeof(FColor))});
                Cube->SRGB = true;
                Cube->UpdateResource();
                auto* AmbientLight = NewObject<USkyLightComponent>(Owner, NAME_None, ObjectFlags);
                AmbientLight->SetupAttachment(Root);
                AmbientLight->SetMobility(EComponentMobility::Movable);
                AmbientLight->SourceType = SLS_SpecifiedCubemap;
                AmbientLight->SetCubemap(Cube);
                // LGTM's ambient term is applied to the room independently of local light shadows.
                AmbientLight->SetCastShadows(false);
                AmbientLight->RegisterComponent();
            }
            const auto Direct = AuthoredColor(Data->GetArrayField(TEXT("directional")));
            const double Fade = FMath::Clamp(Data->GetNumberField(TEXT("directionalFade")), 0., 8.);
            if (FMath::Max3(Direct.R, Direct.G, Direct.B) > 0 && Fade > 0)
            {
                const auto& Rotation = Data->GetArrayField(TEXT("directionalRotation"));
                auto* Light = NewObject<UDirectionalLightComponent>(Owner, NAME_None, ObjectFlags);
                Light->SetupAttachment(Root);
                Light->SetMobility(EComponentMobility::Movable);
                const FRotator Direction(-Rotation[1]->AsNumber(), Rotation[0]->AsNumber(), 0);
                Light->SetRelativeRotation(T5ToUnrealDirection(Direction.Vector()).Rotation());
                Light->SetLightFColor(Direct);
                Light->SetIntensity(4.0 * Fade);
                Light->RegisterComponent();
            }
        }
        const double Near = FMath::Max(0., Data->GetNumberField(TEXT("fogNear"))) * CentimetersPerT5Unit;
        const double Far = Data->GetNumberField(TEXT("fogFar")) * CentimetersPerT5Unit;
        const double Max = FMath::Clamp(Data->GetNumberField(TEXT("fogMax")), 0., 1.);
        // Exterior fog belongs to the changing WTHR state. Submitting cell fog
        // first masks it because UE renders only ExponentialFogs[0].
        if (bInterior && Far > Near && Max > 0)
        {
            auto* Fog = NewObject<UT5WeatherFogComponent>(Owner, NAME_None, ObjectFlags);
            Fog->SetupAttachment(Root);
            Fog->SetFogHeightFalloff(0);
            Fog->SetStartDistance(Near);
            Fog->SetFogMaxOpacity(Max);
            const double Power = FMath::Clamp(Data->GetNumberField(TEXT("fogPower")), .01, 10.);
            Fog->SetFogDensity(FMath::Min(10., 1000.0 * Power * -FMath::Log2(1 - FMath::Min(.98, Max)) / (Far - Near)));
            Fog->SetFogInscatteringColor(FMath::Lerp(AuthoredColor(*Data, TEXT("fogNearColor")),
                AuthoredColor(*Data, TEXT("fogFarColor")), .5f));
            const double Clip = Data->GetNumberField(TEXT("fogClip")) * CentimetersPerT5Unit;
            if (Clip > Far)
                Fog->SetFogCutoffDistance(Clip);
            Fog->RegisterComponent();
        }
    }

    UTexture2D* MakeTexture(TConstArrayView64<uint8> Bytes, bool bColor)
    {
        if (Bytes.IsEmpty())
            return nullptr;
        if (!UE::DDS::FDDSFile::IsADDS(Bytes.GetData(), Bytes.Num()))
        {
            FImage Image;
            if (!FImageUtils::DecompressImage(Bytes.GetData(), Bytes.Num(), Image))
                return nullptr;
            Image.ChangeFormat(ERawImageFormat::BGRA8, Image.GammaSpace);
            UTexture2D* Texture = UTexture2D::CreateTransient(Image.SizeX, Image.SizeY, PF_B8G8R8A8);
            if (Texture)
            {
                FillMip(Texture->GetPlatformData()->Mips[0], {Image.RawData.GetData(), Image.RawData.Num()});
                Texture->SRGB = bColor;
                Texture->NeverStream = true;
                Texture->UpdateResource();
            }
            return Texture;
        }

        TUniquePtr<UE::DDS::FDDSFile> DDS(UE::DDS::FDDSFile::CreateFromDDSInMemory(Bytes.GetData(), Bytes.Num()));
        if (!DDS || !DDS->IsValidTexture2D() || DDS->Mips.IsEmpty())
            return nullptr;
        using E = UE::DDS::EDXGIFormat;
        const bool bRGB24 = DDS->DXGIFormat == E::R8G8B8 || DDS->DXGIFormat == E::B8G8R8;
        const EPixelFormat Format = bRGB24 ? PF_B8G8R8A8 : GetPixelFormat(DDS->DXGIFormat);
        if (Format == PF_Unknown)
            return nullptr;
        FImage Converted;
        const auto ReadMip = [&DDS, &Converted, bRGB24](int32 Index) -> TConstArrayView64<uint8>
        {
            const auto& Source = DDS->Mips[Index];
            if (!bRGB24)
                return {Source.Data, Source.DataSize};
            Converted.Init(Source.Width, Source.Height, ERawImageFormat::BGRA8);
            return DDS->GetMipImage(Converted, Index)
                ? TConstArrayView64<uint8>(Converted.RawData.GetData(), Converted.RawData.Num()) : TConstArrayView64<uint8>();
        };
        const auto& Top = DDS->Mips[0];
        const auto TopData = ReadMip(0);
        if (TopData.IsEmpty())
            return nullptr;
        // Supplying initial pixels would enqueue an RHI upload before the other mips exist.
        UTexture2D* Texture = UTexture2D::CreateTransient(Top.Width, Top.Height, Format);
        if (!Texture)
            return nullptr;
        FillMip(Texture->GetPlatformData()->Mips[0], TopData);
        Texture->SRGB = bColor;
        Texture->NeverStream = true;
        for (int32 Index = 1; Index < DDS->Mips.Num(); ++Index)
        {
            const auto& Source = DDS->Mips[Index];
            const auto Pixels = ReadMip(Index);
            if (Pixels.IsEmpty())
                break;
            auto* Mip = new FTexture2DMipMap;
            Mip->SizeX = Source.Width;
            Mip->SizeY = Source.Height;
            Mip->SizeZ = 1;
            FillMip(*Mip, Pixels);
            Texture->GetPlatformData()->Mips.Add(Mip);
        }
        Texture->UpdateResource();
        return Texture;
    }

    FLinearColor Color(const TSharedPtr<FJsonObject>& Json, const TCHAR* Field, const FLinearColor& Default)
    {
        const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
        if (!Json->TryGetArrayField(Field, Values) || Values->Num() < 3)
            return Default;
        return FLinearColor((*Values)[0]->AsNumber(), (*Values)[1]->AsNumber(), (*Values)[2]->AsNumber(),
            Values->Num() > 3 ? (*Values)[3]->AsNumber() : 1.0);
    }

    struct FMaterialFactory
    {
        AActor* Owner;
        const FT5LoadResult& Data;
        TMap<uint64, TWeakObjectPtr<UTexture2D>>& RecentTextures;
        TMap<FString, UTexture2D*> Textures;
        TMap<FName, UMaterialInterface*> Masters;
        TMap<FString, UMaterialInterface*> Surfaces;
        int32 MissingTextures = 0;
        TArray<TStrongObjectPtr<UObject>> Retained;

        UMaterialInstanceDynamic* Instance(FName Master)
        {
            UMaterialInterface*& Parent = Masters.FindOrAdd(Master);
            if (!Parent)
            {
                const FString Name = Master.ToString();
                Parent = LoadObject<UMaterialInterface>(nullptr, *FString::Printf(TEXT("/T5Data/Runtime/%s.%s"), *Name, *Name));
            }
            auto* Result = Parent ? UMaterialInstanceDynamic::Create(Parent, Owner) : nullptr;
            if (Result)
                Retained.Emplace(Result);
            return Result;
        }

        bool SetTexture(UMaterialInstanceDynamic& Material, FName Param, const TArray<TSharedPtr<FJsonValue>>& Maps, int32 Index, bool bColor)
        {
            if (!Maps.IsValidIndex(Index) || Maps[Index]->AsString().IsEmpty())
                return false;
            const FString Path = Maps[Index]->AsString();
            const FString Key = bColor ? Path + TEXT("#color") : Path;
            UTexture2D** Found = Textures.Find(Key);
            if (!Found)
            {
                const auto Bytes = Data.Read(Path);
                UTexture2D* Texture = nullptr;
                if (!Bytes.IsEmpty() && Bytes.Num() <= MAX_uint32)
                {
                    const uint64 Hash = CityHash64WithSeed(reinterpret_cast<const char*>(Bytes.GetData()),
                        static_cast<uint32>(Bytes.Num()), bColor);
                    auto& Shared = RecentTextures.FindOrAdd(Hash);
                    Texture = Shared.Get();
                    if (!Texture)
                    {
                        Texture = MakeTexture(Bytes, bColor);
                        Shared = Texture;
                    }
                }
                Found = &Textures.Add(Key, Texture);
                if (Texture)
                    Retained.Emplace(Texture);
                if (!Texture)
                {
                    ++MissingTextures;
                    UE_LOG(LogT5Data, Warning, TEXT("Unsupported T5 texture %s"), *Path);
                }
            }
            if (*Found)
                Material.SetTextureParameterValue(Param, *Found);
            return *Found != nullptr;
        }

        UMaterialInterface* Surface(const TSharedPtr<FJsonObject>& Json)
        {
            FString Key;
            const auto Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Key);
            FJsonSerializer::Serialize(Json.ToSharedRef(), Writer);
            if (auto* Found = Surfaces.Find(Key))
                return *Found;
            const auto& Maps = Json->GetArrayField(TEXT("maps"));
            const int32 Shader = Json->GetIntegerField(TEXT("shader"));
            const bool bBlend = Json->GetIntegerField(TEXT("alphaBlend")) != 0;
            const bool bMask = Json->GetIntegerField(TEXT("alphaTest")) != 0 || (bBlend && Shader == 6);
            auto* MID = Instance(bMask ? TEXT("M_T5Masked") : bBlend ? TEXT("M_T5Translucent")
                : (Shader == 4 || Shader == 5) ? TEXT("M_T5Skin") : TEXT("M_T5Surface"));
            if (!MID)
                return nullptr;

            const bool bBase = SetTexture(*MID, TEXT("Base"), Maps, 0, true);
            const bool bNormal = SetTexture(*MID, TEXT("Normal"), Maps, 1, false);
            const bool bTint = Shader == 4 && SetTexture(*MID, TEXT("FaceTint"), Maps, 6, true);
            const bool bDetail = Shader == 4 && SetTexture(*MID, TEXT("Detail"), Maps, 3, true);
            const bool bGlow = Shader == 2 && SetTexture(*MID, TEXT("Glow"), Maps, 2, true);
            const bool bEnv = SetTexture(*MID, TEXT("Env"), Maps, 5, false);
            const auto Tint = Color(Json, TEXT("color"), FLinearColor::White);
            MID->SetVectorParameterValue(TEXT("ColorTint"), Tint);
            MID->SetVectorParameterValue(TEXT("SkinTint"), Color(Json, TEXT("skinTone"), FLinearColor::White));
            MID->SetScalarParameterValue(TEXT("HasBase"), bBase);
            MID->SetScalarParameterValue(TEXT("HasNormal"), bNormal);
            MID->SetScalarParameterValue(TEXT("HasFaceTint"), bTint);
            MID->SetScalarParameterValue(TEXT("HasDetail"), bDetail);
            MID->SetScalarParameterValue(TEXT("HasSkinTint"), Json->HasField(TEXT("skinTone")));
            MID->SetScalarParameterValue(TEXT("HasGlow"), bGlow);
            MID->SetScalarParameterValue(TEXT("HasEnvironment"), bEnv);
            MID->SetScalarParameterValue(TEXT("HasVertexColor"), Json->GetIntegerField(TEXT("vertexColor")) != 0);
            double Flags2 = 0;
            Json->TryGetNumberField(TEXT("flags2"), Flags2);
            MID->SetScalarParameterValue(TEXT("HasVertexAlpha"), T5UsesVertexOpacity(
                static_cast<uint32>(Json->GetNumberField(TEXT("flags"))), static_cast<uint32>(Flags2), Shader));
            MID->SetScalarParameterValue(TEXT("Threshold"), Json->GetNumberField(TEXT("cutoff")));
            MID->SetVectorParameterValue(TEXT("Emission"), Color(Json, TEXT("emission"), FLinearColor::Black));
            const float Gloss = FMath::Max(0.0, Json->GetNumberField(TEXT("gloss")));
            MID->SetScalarParameterValue(TEXT("Roughness"), FMath::Clamp(FMath::Pow(2.0f / (Gloss + 2.0f), .25f),
                Shader == 16 ? .23f : bEnv ? .42f : .54f, .88f));
            MID->SetScalarParameterValue(TEXT("Specular"), FMath::Clamp(Json->GetNumberField(TEXT("specStrength")) * .42, 0.0, 1.0));

            const TArray<TSharedPtr<FJsonValue>>* Basis = nullptr;
            if (Json->TryGetArrayField(TEXT("basis"), Basis) && Basis->Num() == 3)
            {
                MID->SetScalarParameterValue(TEXT("HasBasis"), 1);
                for (int32 Axis = 0; Axis < 3; ++Axis)
                {
                    const auto& Row = (*Basis)[Axis]->AsArray();
                    if (Row.Num() == 3)
                        MID->SetVectorParameterValue(FName(*FString::Printf(TEXT("Axis%d"), Axis)),
                            FLinearColor(Row[0]->AsNumber(), Row[1]->AsNumber(), Row[2]->AsNumber(), 0));
                }
                const bool bSpec = SetTexture(*MID, TEXT("SkinSpec"), Maps, 2, false);
                MID->SetScalarParameterValue(TEXT("HasSkinSpec"), bSpec);
            }
            Surfaces.Add(MoveTemp(Key), MID);
            return MID;
        }

        UMaterialInterface* Terrain(const TSharedPtr<FJsonObject>& Json)
        {
            auto* MID = Instance(TEXT("M_T5Terrain"));
            if (!MID)
                return nullptr;
            auto Layer = [this, MID](int32 Index, const TArray<TSharedPtr<FJsonValue>>& Maps)
            {
                SetTexture(*MID, FName(*FString::Printf(TEXT("Base%d"), Index)), Maps, 0, true);
                const bool bNormal = SetTexture(*MID, FName(*FString::Printf(TEXT("Normal%d"), Index)), Maps, 1, false);
                MID->SetScalarParameterValue(FName(*FString::Printf(TEXT("HasNormal%d"), Index)), bNormal);
            };
            Layer(0, Json->GetArrayField(TEXT("base")));
            const auto& Layers = Json->GetArrayField(TEXT("layers"));
            if (Layers.Num() >= TerrainLayerLimit)
                UE_LOG(LogT5Data, Warning, TEXT("Terrain quadrant has more than %d layers"), TerrainLayerLimit);
            TArray<FColor> Weights[2];
            for (auto& W : Weights)
                W.Init(FColor(0, 0, 0, 0), 17 * 17);
            for (int32 Index = 0; Index < FMath::Min(Layers.Num(), TerrainLayerLimit - 1); ++Index)
            {
                const auto Obj = Layers[Index]->AsObject();
                Layer(Index + 1, Obj->GetArrayField(TEXT("maps")));
                const auto& Values = Obj->GetArrayField(TEXT("weights"));
                for (int32 Pixel = 0; Pixel < FMath::Min(Values.Num(), 17 * 17); ++Pixel)
                {
                    const auto Weight = static_cast<uint8>(FMath::RoundToInt(FMath::Clamp(Values[Pixel]->AsNumber(), 0.0, 1.0) * 255));
                    auto& Value = Weights[Index / 4][Pixel];
                    switch (Index % 4)
                    {
                    case 0: Value.R = Weight; break;
                    case 1: Value.G = Weight; break;
                    case 2: Value.B = Weight; break;
                    default: Value.A = Weight; break;
                    }
                }
            }
            for (int32 I = 0; I < 2; ++I)
            {
                auto* Texture = UTexture2D::CreateTransient(17, 17, PF_B8G8R8A8);
                if (Texture)
                {
                    FillMip(Texture->GetPlatformData()->Mips[0],
                        {reinterpret_cast<const uint8*>(Weights[I].GetData()), int64(Weights[I].Num()) * int64(sizeof(FColor))});
                    Texture->SRGB = false;
                    Texture->NeverStream = true;
                    Texture->AddressX = Texture->AddressY = TA_Clamp;
                    Texture->Filter = TF_Bilinear;
                    Texture->UpdateResource();
                    MID->SetTextureParameterValue(FName(*FString::Printf(TEXT("Weights%d"), I)), Texture);
                }
            }
            return MID;
        }
    };

    struct FMeshGroup
    {
        FMeshDescription Description;
        uint64 ContentHash = 0;
        TArray<TSharedPtr<FJsonObject>> Sections;
        TArray<FStaticMaterial> Materials;
        TMap<UMaterialInterface*, FPolygonGroupID> MaterialSlots;
        bool bCollision;
        bool bArchery = false;
        uint32 Reference = 0;
        FString MotionNode;

        explicit FMeshGroup(bool bInCollision) : bCollision(bInCollision)
        {
            FStaticMeshAttributes Attributes(Description);
            Attributes.Register();
            Attributes.GetVertexInstanceUVs().SetNumChannels(2);
        }
    };

    bool QueueSection(FMeshGroup& Group, const TSharedPtr<FJsonObject>& Section, TConstArrayView64<uint8> Buffer,
        const FT5LoadResult& Data, TMap<FString, uint64>& TextureHashes)
    {
        const int64 Offset = static_cast<int64>(Section->GetNumberField(TEXT("offset")));
        const int32 VertexCount = Section->GetIntegerField(TEXT("vertices"));
        const int32 IndexCount = Section->GetIntegerField(TEXT("indices"));
        const int64 VertexBytes = int64(VertexCount) * sizeof(FT5SourceVertex);
        if (Offset < 0 || VertexCount <= 0 || IndexCount <= 0 || IndexCount % 3 || Offset > Buffer.Num()
            || VertexBytes + int64(IndexCount) * int64(sizeof(uint32)) > Buffer.Num() - Offset) return false;
        const uint8* Vertices = Buffer.GetData() + Offset;
        const TSharedPtr<FJsonObject>* Terrain = nullptr;
        const bool bTerrain = Section->TryGetObjectField(TEXT("terrain"), Terrain);
        FString MaterialKey;
        FJsonSerializer::Serialize((bTerrain ? *Terrain : Section->GetObjectField(TEXT("material"))).ToSharedRef(),
            TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&MaterialKey));
        // Texture_N is an export-local name. Match materials across cell windows by
        // immutable texture content, hashing each DDS at most once during this build.
        static const FRegexPattern TexturePattern(TEXT("Texture_[0-9]+\\.[a-zA-Z0-9]+"));
        FRegexMatcher TextureMatcher(TexturePattern, MaterialKey);
        FString StableMaterialKey;
        int32 Copied = 0;
        while (TextureMatcher.FindNext())
        {
            const int32 Begin = TextureMatcher.GetMatchBeginning(), End = TextureMatcher.GetMatchEnding();
            const FString Name = MaterialKey.Mid(Begin, End - Begin);
            StableMaterialKey += MaterialKey.Mid(Copied, Begin - Copied);
            const auto Bytes = Data.Read(Name);
            if (Bytes.IsEmpty()) StableMaterialKey += Name;
            else
            {
                auto* Hash = TextureHashes.Find(Name);
                if (!Hash) Hash = &TextureHashes.Add(Name,
                    CityHash64(reinterpret_cast<const char*>(Bytes.GetData()), Bytes.Num()));
                StableMaterialKey += FString::Printf(TEXT("%016llX"), *Hash);
            }
            Copied = End;
        }
        StableMaterialKey += MaterialKey.Mid(Copied);
        MaterialKey = MoveTemp(StableMaterialKey);
        Group.ContentHash = CityHash64WithSeed(reinterpret_cast<const char*>(Vertices),
            VertexBytes + int64(IndexCount) * sizeof(uint32), Group.ContentHash);
        Group.ContentHash = CityHash64WithSeed(reinterpret_cast<const char*>(*MaterialKey),
            MaterialKey.Len() * sizeof(TCHAR), Group.ContentHash);

        Group.Sections.Add(Section);
        return true;
    }

    FT5FrameTask AddSection(FMeshGroup& Group, const TSharedPtr<FJsonObject>& Section, TConstArrayView64<uint8> Buffer,
        FMaterialFactory& MaterialFactory)
    {
        const int64 Offset = static_cast<int64>(Section->GetNumberField(TEXT("offset")));
        const int32 VertexCount = Section->GetIntegerField(TEXT("vertices"));
        const int32 IndexCount = Section->GetIntegerField(TEXT("indices"));
        const int64 VertexBytes = int64(VertexCount) * sizeof(FT5SourceVertex);
        if (Offset < 0 || VertexCount <= 0 || IndexCount <= 0 || IndexCount % 3
            || Offset > Buffer.Num() || VertexBytes + int64(IndexCount) * int64(sizeof(uint32)) > Buffer.Num() - Offset)
            co_return false;

        const uint8* Vertices = Buffer.GetData() + Offset;
        const uint8* Indices = Vertices + VertexBytes;
        for (int32 I = 0; I < IndexCount; ++I)
        {
            uint32 Index;
            FMemory::Memcpy(&Index, Indices + I * sizeof(Index), sizeof(Index));
            if (Index >= static_cast<uint32>(VertexCount))
                co_return false;
        }

        const TSharedPtr<FJsonObject>* Terrain = nullptr;
        const bool bTerrain = Section->TryGetObjectField(TEXT("terrain"), Terrain);
        const auto SourceMaterial = bTerrain ? TSharedPtr<FJsonObject>() : Section->GetObjectField(TEXT("material"));
        UMaterialInterface* Material = bTerrain ? MaterialFactory.Terrain(*Terrain)
            : MaterialFactory.Surface(SourceMaterial);
        const bool bBackFaces = SourceMaterial && SourceMaterial->GetIntegerField(TEXT("twoSided")) != 0
            && Material && !Material->IsTwoSided();
        if (!Material)
            co_return false;

        FVector2f MinXY(FLT_MAX, FLT_MAX);
        if (bTerrain)
            for (int32 I = 0; I < VertexCount; ++I)
            {
                FT5SourceVertex V;
                FMemory::Memcpy(&V, Vertices + I * sizeof(V), sizeof(V));
                MinXY.X = FMath::Min(MinXY.X, V.Position.X);
                MinXY.Y = FMath::Min(MinXY.Y, V.Position.Y);
            }

        auto& Mesh = Group.Description;
        FStaticMeshAttributes Attr(Mesh);
        auto Positions = Attr.GetVertexPositions();
        auto Normals = Attr.GetVertexInstanceNormals();
        auto Colors = Attr.GetVertexInstanceColors();
        auto UVs = Attr.GetVertexInstanceUVs();
        auto* Existing = Group.MaterialSlots.Find(Material);
        const FPolygonGroupID Slot = Existing ? *Existing : Mesh.CreatePolygonGroup();
        if (!Existing)
        {
            const FName SlotName(*FString::Printf(TEXT("Section%d"), Group.Materials.Num()));
            Attr.GetPolygonGroupMaterialSlotNames()[Slot] = SlotName;
            Group.MaterialSlots.Add(Material, Slot);
            Group.Materials.Emplace(Material, SlotName);
            // Runtime textures retain their full mip chain and are resident. The streaming system
            // still requires initialized mesh metadata for the material slots.
            Group.Materials.Last().UVChannelData = FMeshUVChannelInfo(1.0f);
        }
        TArray<FVertexInstanceID> Instances;
        Instances.Reserve(VertexCount);
        TArray<FVertexInstanceID> BackInstances;
        if (bBackFaces) BackInstances.Reserve(VertexCount);
        for (int32 I = 0; I < VertexCount; ++I)
        {
            if (I % 512 == 0) co_yield 0;
            FT5SourceVertex V;
            FMemory::Memcpy(&V, Vertices + I * sizeof(V), sizeof(V));
            const FVertexID Vertex = Mesh.CreateVertex();
            Positions[Vertex] = T5ToUnrealPosition(V.Position);
            const FVertexInstanceID Instance = Mesh.CreateVertexInstance(Vertex);
            Instances.Add(Instance);
            Normals[Instance] = T5ToUnrealDirection(V.Normal);
            Colors[Instance] = V.Color;
            UVs.Set(Instance, 0, V.UV);
            if (bBackFaces)
            {
                const auto Back = Mesh.CreateVertexInstance(Vertex);
                BackInstances.Add(Back);
                Normals[Back] = -Normals[Instance];
                Colors[Back] = V.Color;
                UVs.Set(Back, 0, V.UV);
            }
            if (bTerrain)
                UVs.Set(Instance, 1, ((FVector2f(V.Position.X, V.Position.Y) - MinXY) / 128.0f + FVector2f(.5f, .5f)) / 17.0f);
        }
        for (int32 I = 0; I < IndexCount; I += 3)
        {
            if (I % 1536 == 0) co_yield 0;
            uint32 Triangle[3];
            FMemory::Memcpy(Triangle, Indices + I * sizeof(uint32), sizeof(Triangle));
            const FVertexInstanceID Corners[] = {Instances[Triangle[0]], Instances[Triangle[1]], Instances[Triangle[2]]};
            Mesh.CreateTriangle(Slot, Corners);
            if (bBackFaces)
            {
                const FVertexInstanceID Back[] = {BackInstances[Triangle[0]], BackInstances[Triangle[2]], BackInstances[Triangle[1]]};
                Mesh.CreateTriangle(Slot, Back);
            }
        }
        co_return true;
    }
}

AT5Scene::AT5Scene()
{
    RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Origin"));
    PrimaryActorTick.bCanEverTick = true;
    PrimaryActorTick.TickInterval = 0;
}

void AT5Scene::BeginPlay()
{
    Super::BeginPlay();
    FString Data, Plugins, Bridge;
    if (ResolvePaths(Data, Plugins, Bridge))
        UIResources = MakeShared<FT5UIResources>(Data, Bridge);
    if (GEngine && !GIsEditor && !FParse::Param(FCommandLine::Get(), TEXT("T5Diagnostics")))
        GEngine->bEnableOnScreenDebugMessages = false;
    if (auto* Controller = GetWorld()->GetFirstPlayerController())
    {
        float Fov = 80;
        GConfig->GetFloat(TEXT("/Script/T5Data.T5Scene"), TEXT("WorldFieldOfView"), Fov, GGameIni);
        FParse::Value(FCommandLine::Get(), TEXT("T5FOV="), Fov);
        if (Controller->PlayerCameraManager && FMath::IsFinite(Fov))
            Controller->PlayerCameraManager->SetFOV(FMath::Clamp(Fov, 30.f, 120.f));
        EnableInput(Controller);
        if (InputComponent)
        {
            InputComponent->BindKey(EKeys::F6, IE_Pressed, this, &AT5Scene::OpenRaceMenu);
            InputComponent->BindKey(EKeys::F5, IE_Pressed, this, &AT5Scene::QuickSave);
            InputComponent->BindKey(EKeys::F9, IE_Pressed, this, &AT5Scene::OpenSaveBrowser);
            InputComponent->BindKey(EKeys::M, IE_Pressed, this, &AT5Scene::ToggleMapBrowser);
            InputComponent->BindKey(EKeys::E, IE_Pressed, this, &AT5Scene::InteractT5);
            InputComponent->BindKey(EKeys::Tab, IE_Pressed, this, &AT5Scene::OpenPlayerInventory);
            InputComponent->BindKey(EKeys::I, IE_Pressed, this, &AT5Scene::OpenPlayerInventory);
            InputComponent->BindKey(EKeys::R, IE_Pressed, this, &AT5Scene::ReadyPlayerWeapon);
            InputComponent->BindKey(EKeys::LeftMouseButton, IE_Pressed, this, &AT5Scene::AttackPlayerWeapon);
            InputComponent->BindKey(EKeys::F, IE_Pressed, this, &AT5Scene::TogglePlayerPerspective);
            InputComponent->BindKey(EKeys::Gamepad_RightThumbstick, IE_Pressed, this, &AT5Scene::TogglePlayerPerspective);
            InputComponent->BindKey(EKeys::MouseScrollUp, IE_Pressed, this, &AT5Scene::ZoomPlayerIn);
            InputComponent->BindKey(EKeys::MouseScrollDown, IE_Pressed, this, &AT5Scene::ZoomPlayerOut);
        }
    }
    if (FParse::Param(FCommandLine::Get(), TEXT("T5SelectSave")))
        OpenSaveBrowser();
    else if (FParse::Param(FCommandLine::Get(), TEXT("T5LoadGame")))
        QuickLoad();
    else if (bLoadAtStart)
        LoadFromT5();
}

void AT5Scene::EndPlay(const EEndPlayReason::Type Reason)
{
    ClearAnimationVisuals();
    DestroyFirstPersonView();
    DestroyPlayerBody();
    CloseInventory();
    if (CharacterCreator) { CharacterCreator->Close(); CharacterCreator.Reset(); }
    CloseSaveBrowser();
    CloseMapBrowser();
    if (PapyrusErrors)
        if (auto* Viewport = GetWorld()->GetGameViewport())
            Viewport->RemoveViewportWidgetContent(PapyrusErrors.ToSharedRef());
    PapyrusErrors.Reset();
    ++CatalogGeneration;
    ClearT5Preview();
    UIResources.Reset();
    Papyrus.Reset();
    Super::EndPlay(Reason);
}

void AT5Scene::ShowLoadingScreen()
{
    auto* Viewport = GetWorld() ? GetWorld()->GetGameViewport() : nullptr;
    if (!Viewport || LoadingScreen) return;
    const TWeakObjectPtr<AT5Scene> Weak(this);
    LoadingScreen = SNew(SOverlay)
        .Visibility_Lambda([Weak] {
            const auto* Scene = Weak.Get();
            return Scene && (Scene->bSceneLoading || Scene->bCatalogLoading || Scene->bStartupPreparing || Scene->bLoadingFailed)
                ? EVisibility::HitTestInvisible : EVisibility::Collapsed;
        })
        + SOverlay::Slot()
        [SNew(SBorder)
            .BorderImage(FCoreStyle::Get().GetBrush(TEXT("WhiteBrush")))
            .BorderBackgroundColor_Lambda([Weak] {
                const auto* Scene = Weak.Get();
                return FLinearColor(0.008f, .012f, .018f, Scene && Scene->bFullLoadingScreen ? 1.f : 0.f);
            })]
        + SOverlay::Slot().HAlign(HAlign_Center).VAlign(VAlign_Bottom).Padding(30, 45)
        [SNew(SBox).WidthOverride(1040).Visibility_Lambda([Weak] {
            const auto* S = Weak.Get();
            return S && (S->bFullLoadingScreen || S->bStartupPreparing || S->bCatalogLoading || S->bLoadingFailed
                || FParse::Param(FCommandLine::Get(), TEXT("T5Diagnostics"))) ? EVisibility::HitTestInvisible : EVisibility::Collapsed;
        })
            [SNew(SBorder).Padding(22)
                .BorderImage(FCoreStyle::Get().GetBrush(TEXT("WhiteBrush")))
                .BorderBackgroundColor(FLinearColor(.02f, .025f, .035f, .96f))
                [SNew(SVerticalBox)
                    + SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 12)
                    [SNew(STextBlock).Font(FCoreStyle::GetDefaultFontStyle("Regular", 24)).Text_Lambda([Weak] {
                        const auto* Scene = Weak.Get();
                        if (!Scene) return FText::GetEmpty();
                        if (Scene->bLoadingFailed) return FText::FromString(TEXT("Ошибка загрузки: ") + Scene->LoadStatus);
                        if (!Scene->bSceneLoading && Scene->bStartupPreparing)
                            return FText::FromString(TEXT("Подготовка персонажей, анимаций и камеры…"));
                        if (!Scene->LoadProgress) return FText::FromString(TEXT("Загрузка локации…"));
                        FScopeLock Lock(&Scene->LoadProgress->Mutex);
                        const auto& P = *Scene->LoadProgress;
                        FString Phase = P.Phase;
                        if (Phase == TEXT("Opening game files")) Phase = TEXT("Открытие игровых файлов");
                        else if (Phase == TEXT("Reading plugin bytes")) Phase = TEXT("Чтение ESP/ESM/ESL в память");
                        else if (Phase == TEXT("Scanning resident plugin records")) Phase = TEXT("Разбор записей ESP/ESM из памяти");
                        else if (Phase == TEXT("Reading converted resource cache")) Phase = TEXT("Загрузка преобразованных ресурсов из кеша");
                        else if (Phase == TEXT("Reading cached texture sources")) Phase = TEXT("Чтение текстур из архивов по кешу");
                        else if (Phase == TEXT("Indexing resource revisions")) Phase = TEXT("Индексирование изменений ресурсов");
                        else if (Phase == TEXT("Validating resource cache")) Phase = TEXT("Проверка кеша ресурсов");
                        else if (Phase == TEXT("Converting models and textures")) Phase = TEXT("Преобразование моделей и текстур (объекты)");
                        else if (Phase == TEXT("Building landscape tiles")) Phase = TEXT("Построение ячеек ландшафта");
                        else if (Phase == TEXT("Reading placed NIF models")) Phase = TEXT("Чтение моделей объектов из NIF");
                        else if (Phase == TEXT("Resolving actors and references")) Phase = TEXT("Подготовка персонажей и ссылок");
                        else if (Phase == TEXT("Indexing winning records")) Phase = TEXT("Индексирование записей с учётом порядка плагинов");
                        const FString Count = P.Total ? FString::Printf(TEXT("%llu / %llu"), P.Done, P.Total)
                            : TEXT("Общий объём этого этапа определяется…");
                        return FText::FromString(FString::Printf(TEXT("%s\n%s\n%s\nТекстуры: %llu · %.1f МиБ прочитано"),
                            *Phase, *Count, *P.Item, P.Textures, double(P.Bytes) / 1048576.));
                    }).WrapTextAt(990)]
                    + SVerticalBox::Slot().AutoHeight()
                    [SNew(SProgressBar).Percent_Lambda([Weak]() -> TOptional<float> {
                        const auto* Scene = Weak.Get();
                        if (!Scene || !Scene->LoadProgress || (!Scene->bSceneLoading && !Scene->bCatalogLoading)) return {};
                        FScopeLock Lock(&Scene->LoadProgress->Mutex);
                        const auto& P = *Scene->LoadProgress;
                        return P.Total ? TOptional<float>(FMath::Clamp(float(double(P.Done) / P.Total), 0.f, 1.f)) : TOptional<float>{};
                    })]
                ]
            ]
        ]
        + SOverlay::Slot().HAlign(HAlign_Right).VAlign(VAlign_Bottom).Padding(18)
        [SNew(STextBlock).Text(FText::FromString(TEXT("Подгрузка мира…")))
            .Visibility_Lambda([Weak] {
                const auto* S = Weak.Get();
                return S && S->bSceneLoading && !S->bFullLoadingScreen && !S->bStartupPreparing
                    && !FParse::Param(FCommandLine::Get(), TEXT("T5Diagnostics")) ? EVisibility::HitTestInvisible : EVisibility::Collapsed;
            })];
    Viewport->AddViewportWidgetContent(LoadingScreen.ToSharedRef(), 1000);
}

void AT5Scene::ShowPapyrusErrors()
{
    if (!FParse::Param(FCommandLine::Get(), TEXT("T5Diagnostics"))) return;
    auto* Viewport = GetWorld()->GetGameViewport();
    if (PapyrusErrors || !Viewport || !Papyrus)
        return;
    const TWeakPtr<FT5PapyrusRuntime> Runtime = Papyrus;
    PapyrusErrors = SNew(SOverlay)
        .Visibility(EVisibility::HitTestInvisible)
        + SOverlay::Slot()
        .HAlign(HAlign_Left)
        .VAlign(VAlign_Top)
        .Padding(FMargin(14))
        [
            SNew(SBorder)
            .Visibility_Lambda([Runtime]
            {
                const auto State = Runtime.Pin();
                return State && !State->RecentErrors().IsEmpty() ? EVisibility::HitTestInvisible : EVisibility::Collapsed;
            })
            .BorderBackgroundColor(FLinearColor(0.025f, 0.008f, 0.008f, 0.88f))
            .Padding(FMargin(9, 6))
            [
                SNew(STextBlock)
                .Text_Lambda([Runtime]
                {
                    const auto State = Runtime.Pin();
                    return State ? FText::FromString(FString(TEXT("Papyrus\n")) + State->RecentErrors()) : FText::GetEmpty();
                })
                .ColorAndOpacity(FLinearColor(1, 0.68f, 0.62f))
                .WrapTextAt(690)
            ]
        ];
    Viewport->AddViewportWidgetContent(PapyrusErrors.ToSharedRef(), 100);
}

void AT5Scene::Destroyed()
{
    ClearT5Preview();
    Super::Destroyed();
}

void AT5Scene::ClearT5Preview()
{
    CloseDialogue();
    if (CharacterCreator) { CharacterCreator->Close(); CharacterCreator.Reset(); }
    if (LoadingScreen && GetWorld())
        if (auto* Viewport = GetWorld()->GetGameViewport())
            Viewport->RemoveViewportWidgetContent(LoadingScreen.ToSharedRef());
    LoadingScreen.Reset(); LoadProgress.Reset();
    bStartupPreparing = bLoadingFailed = bFullLoadingScreen = bCatalogLoading = false;
    Faces.Reset();
    PendingNativeCheckpoint.Reset(); PendingHostCheckpoint.Reset(); PendingPhysicsCheckpoint.Reset(); PendingArcheryCheckpoint.Reset(); CheckpointPlayer.Reset();
    bRestoringCheckpoint = false;
    PendingBuild.Reset();
    RuntimeAudio.Reset();
    RuntimeArchery.Reset();
    ArcheryAssets.Reset();
    if (Titles && GetWorld())
        if (auto* Viewport = GetWorld()->GetGameViewport())
            Viewport->RemoveViewportWidgetContent(Titles.ToSharedRef());
    Titles.Reset();
    if (Compass && GetWorld())
        if (auto* Viewport = GetWorld()->GetGameViewport())
            Viewport->RemoveViewportWidgetContent(Compass.ToSharedRef());
    Compass.Reset();
    ++LoadGeneration;
    ActiveWorld = 0;
    ActiveLocation = 0;
    LastPlayerHeading.Reset();
    LastPlayerScriptPose.Reset();
    LastPlayerVehicle = 0;
    DestroyPlayerBody();
    PlayerCameraDistance = 0.f;
    bOpeningViewReady = false;
    Locomotion.Reset();
    RecentTextures.Reset();
    CachedStaticMeshes.Reset(); StaticMeshCacheOrder.Reset(); StaticMeshCacheBytes = 0;
    if (auto* Effects = VehiclePostProcess.Get())
        Effects->DestroyComponent();
    VehiclePostProcess.Reset();
    LastLoggedPlayerPose.Reset();
    PlayerCamera.Reset();
    ReferenceAnimations.Reset();
    ClearAnimationVisuals();
    PlayerAnimationSerial = 0;
    SpawnReloadDue = 0;
    LoadedInteriorArrivals.Reset();
    bSceneLoading = false;
    LoadStatus.Reset();
    if (IsValid(LoadedScene))
        LoadedScene->Destroy();
    LoadedScene = nullptr;
    Mechanics.Reset();
    ReferenceVisuals.Reset();
    InteractionComponents.Reset();
    SetStartupDaylight(GetWorld(), true, true);
}

bool AT5Scene::ResolvePaths(FString& Data, FString& Plugins, FString& Bridge) const
{
    const auto Plugin = IPluginManager::Get().FindPlugin(TEXT("T5Data"));
    if (!Plugin)
        return false;

    const FString ProjectDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
    const bool bOverrideData = FParse::Value(FCommandLine::Get(), TEXT("T5Data="), Data);
    if (!bOverrideData)
    {
        FString DataRoot = ProjectDir;
        if (FPlatformProperties::RequiresCookedData())
        {
            FPaths::NormalizeDirectoryName(DataRoot);
            DataRoot = FPaths::GetPath(DataRoot);
            if (!FPaths::DirectoryExists(DataRoot / TEXT("Data")))
            {
                // Run directly from the default Saved/StagedBuilds/Windows stage.
                const FString SourceRoot = FPaths::ConvertRelativePathToFull(DataRoot, TEXT("../../.."));
                const FString ProjectFile = FString(FApp::GetProjectName()) + TEXT(".uproject");
                if (FPaths::FileExists(SourceRoot / ProjectFile))
                    DataRoot = SourceRoot;
            }
        }
        Data = DataRoot / TEXT("Data");
    }
    if (FParse::Value(FCommandLine::Get(), TEXT("T5Plugins="), Plugins))
        Plugins = FPaths::ConvertRelativePathToFull(ProjectDir, Plugins);
    else
        Plugins = FPaths::ConvertRelativePathToFull(Plugin->GetBaseDir() / TEXT("Resources/SkyrimPlugins.txt"));
    Data = FPaths::ConvertRelativePathToFull(ProjectDir, Data);
    if (!FPaths::DirectoryExists(Data))
    {
        UE_LOG(LogT5Data, Error, TEXT("Skyrim Data directory not found: %s. See the T54UE README for setup."), *Data);
        return false;
    }
    if (!FPaths::FileExists(Plugins))
    {
        UE_LOG(LogT5Data, Error, TEXT("Skyrim plugin list not found: %s"), *Plugins);
        return false;
    }
    if (!bOverrideData)
        Data = ResolveDataJunction(MoveTemp(Data));

    Bridge = FPaths::ConvertRelativePathToFull(FPaths::Combine(Plugin->GetBaseDir(), TEXT("Binaries/ThirdParty/Win64")));
    FParse::Value(FCommandLine::Get(), TEXT("T5BridgeDirectory="), Bridge);
    Bridge = FPaths::ConvertRelativePathToFull(Bridge);
    return true;
}

void AT5Scene::RequestCatalog(TFunction<void(bool)> OnReady)
{
    FString Data, Plugins, Bridge;
    if (!ResolvePaths(Data, Plugins, Bridge))
    {
        CatalogStatus = TEXT("Set valid T5 Data and plugin list paths.");
        OnReady(false);
        return;
    }
    CatalogStatus = TEXT("Reading T5 plugin list...");
    const auto Progress = MakeShared<FT5LoadProgress>();
    if (!LoadedScene && !bSceneLoading)
    {
        LoadProgress = Progress;
        bCatalogLoading = bFullLoadingScreen = true;
        ShowLoadingScreen();
    }
    const uint64 Generation = ++CatalogGeneration;
    Async(EAsyncExecution::ThreadPool, [Weak = TWeakObjectPtr<AT5Scene>(this), Generation, Data, Plugins, Bridge, Progress, OnReady = MoveTemp(OnReady)]() mutable
    {
        FT5LoadRequest Request;
        Request.bCatalog = true;
        Request.Progress = Progress;
        auto Result = LoadT5Scene(Data, Plugins, Bridge, Request);
        for (const TCHAR* Name : {TEXT("meshes.json"), TEXT("location.json"), TEXT("collision.json"), TEXT("mechanics.json"), TEXT("catalog.json")})
            Result->PreparedJson.Add(Name, ReadJson(*Result, Name));
        AsyncTask(ENamedThreads::GameThread, [Weak, Generation, Result = MoveTemp(Result), OnReady = MoveTemp(OnReady)]
        {
            auto* Scene = Weak.Get();
            if (!Scene || Scene->CatalogGeneration != Generation)
                return;
            Scene->bCatalogLoading = false;
            auto Catalog = ReadJson(*Result, TEXT("catalog.json"));
            if (!Catalog || !Result->Error.IsEmpty())
            {
                Scene->CatalogStatus = Result->Error.IsEmpty() ? TEXT("Invalid T5 catalog.") : Result->Error;
                if (!Scene->LoadedScene && !Scene->bSceneLoading)
                { Scene->bLoadingFailed = true; Scene->LoadStatus = Scene->CatalogStatus; }
                UE_LOG(LogT5Data, Error, TEXT("%s"), *Scene->CatalogStatus);
                OnReady(false);
                return;
            }
            Scene->Locations.Reset();
            const auto& Worlds = Catalog->GetArrayField(TEXT("worlds"));
            const auto& Interiors = Catalog->GetArrayField(TEXT("interiors"));
            ReadLocations(Worlds, false, Scene->Locations);
            ReadLocations(Interiors, true, Scene->Locations);
            Scene->CatalogStatus = FString::Printf(TEXT("%d worlds, %d interiors"), Worlds.Num(), Interiors.Num());
            UE_LOG(LogT5Data, Display, TEXT("T5_CATALOG_READY %s"), *Scene->CatalogStatus);
            if (Scene->MapBrowser)
                Scene->MapBrowser->Refresh();
            OnReady(true);
        });
    });
}

void AT5Scene::RefreshMapCatalog()
{
    RequestCatalog([](bool) {});
}

TArray<FString> AT5Scene::GetMapOptions() const
{
    TArray<FString> Options;
    for (const auto& Pair : Locations)
    {
        const auto& Location = Pair.Value;
        Options.Add(Location->Label() + TEXT(" ") + Location->Key());
    }
    Options.Sort();
    return Options;
}

void AT5Scene::ToggleMapBrowser()
{
    if (CharacterCreator || SaveBrowser || DialogueMenu) return;
    if (MapBrowser)
    {
        CloseMapBrowser();
        return;
    }
    auto* World = GetWorld();
    auto* Controller = World ? World->GetFirstPlayerController() : nullptr;
    auto* Viewport = World ? World->GetGameViewport() : nullptr;
    if (!World || !World->IsGameWorld() || !Controller || !Viewport)
        return;
    if (Locations.IsEmpty() && !bSceneLoading && !CatalogStatus.StartsWith(TEXT("Reading")))
        RefreshMapCatalog();
    SAssignNew(MapBrowser, ST5MapBrowser, this);
    Viewport->AddViewportWidgetContent(MapBrowser.ToSharedRef(), 10);
    FInputModeGameAndUI Mode;
    Mode.SetWidgetToFocus(MapBrowser);
    Mode.SetHideCursorDuringCapture(false);
    Controller->SetInputMode(Mode);
    Controller->bShowMouseCursor = true;
    Controller->SetIgnoreMoveInput(true);
    Controller->SetIgnoreLookInput(true);
}

void AT5Scene::CloseMapBrowser()
{
    if (!MapBrowser)
        return;
    if (auto* World = GetWorld())
    {
        if (auto* Viewport = World->GetGameViewport())
            Viewport->RemoveViewportWidgetContent(MapBrowser.ToSharedRef());
        if (auto* Controller = World->GetFirstPlayerController())
        {
            Controller->SetInputMode(FInputModeGameOnly());
            Controller->bShowMouseCursor = false;
            Controller->SetIgnoreMoveInput(false);
            Controller->SetIgnoreLookInput(false);
        }
    }
    MapBrowser.Reset();
}

void AT5Scene::LoadFromT5()
{
    FString Name = SelectedMap;
    const bool bExplicitMap = FParse::Value(FCommandLine::Get(), TEXT("T5Map="), Name);
    if (FParse::Param(FCommandLine::Get(), TEXT("T5NewGame")) && GetWorld()->IsGameWorld())
    {
        OpeningQuest = 0x0003372B;
        SetActorTickInterval(0.f);
        FString QuestId;
        if (FParse::Value(FCommandLine::Get(), TEXT("T5Quest="), QuestId))
        {
            TCHAR* End = nullptr;
            const uint64 Parsed = FCString::Strtoui64(*QuestId, &End, 16);
            if (!Parsed || Parsed > MAX_uint32 || (End && *End))
            {
                UE_LOG(LogT5Data, Error, TEXT("T5Quest must be a hexadecimal FormID"));
                return;
            }
            OpeningQuest = static_cast<uint32>(Parsed);
        }
        if (!bExplicitMap)
        {
            FString Data, Plugins, Bridge;
            if (!ResolvePaths(Data, Plugins, Bridge))
                return;
            FT5LoadRequest Request;
            Request.Progress = LoadProgress = MakeShared<FT5LoadProgress>();
            bStartupPreparing = bFullLoadingScreen = true;
            bLoadingFailed = false;
            ShowLoadingScreen();
            Request.bNewGameStart = true;
            Request.OpeningQuest = OpeningQuest;
            const uint64 Generation = ++LoadGeneration;
            bSceneLoading = true;
            Async(EAsyncExecution::ThreadPool, [Weak = TWeakObjectPtr<AT5Scene>(this), Generation, Data, Plugins, Bridge, Request]
            {
                auto Result = LoadT5Scene(Data, Plugins, Bridge, Request);
                AsyncTask(ENamedThreads::GameThread, [Weak, Generation, Result = MoveTemp(Result)]
                {
                    auto* Scene = Weak.Get();
                    if (!Scene || Scene->LoadGeneration != Generation)
                        return;
                    Scene->bSceneLoading = false;
                    if (const auto Start = ReadJson(*Result, TEXT("start.json")))
                    {
                        const auto Form = static_cast<uint32>(Start->GetNumberField(TEXT("id")));
                        const FIntPoint Cell(Start->GetIntegerField(TEXT("x")), Start->GetIntegerField(TEXT("y")));
                        Scene->RequestCatalog([](bool) {});
                        Scene->LoadLocation(Form, Cell, true);
                    }
                    else
                    {
                        Scene->bLoadingFailed = true;
                        Scene->LoadStatus = Result->Error.IsEmpty() ? TEXT("Invalid new game start data") : Result->Error;
                        UE_LOG(LogT5Data, Error, TEXT("T5 new game start: %s"), *Scene->LoadStatus);
                    }
                });
            });
            return;
        }
    }
    if (Name.IsEmpty())
    {
        RequestScene(FT5LoadRequest{}, false);
        return;
    }
    TOptional<FIntPoint> Cell;
    if (bUseStartCell)
        Cell = StartCell;
    FIntPoint ArgCell;
    const bool bX = FParse::Value(FCommandLine::Get(), TEXT("T5CellX="), ArgCell.X);
    const bool bY = FParse::Value(FCommandLine::Get(), TEXT("T5CellY="), ArgCell.Y);
    if (bX != bY)
    {
        UE_LOG(LogT5Data, Error, TEXT("Specify both -T5CellX and -T5CellY"));
        return;
    }
    if (bX)
        Cell = ArgCell;
    RequestCatalog([this, Name, Cell, Generation = LoadGeneration](bool bReady)
    {
        if (bReady && Generation == LoadGeneration)
            LoadSelectedMap(Name, Cell);
    });
}

void AT5Scene::OpenT5Map(const FString& Name)
{
    if (!Locations.IsEmpty())
        LoadSelectedMap(Name, {});
    else
        RequestCatalog([this, Name, Generation = LoadGeneration](bool bReady)
        {
            if (bReady && Generation == LoadGeneration)
                LoadSelectedMap(Name, {});
        });
}

void AT5Scene::LoadSelectedMap(const FString& Name, TOptional<FIntPoint> Cell)
{
    FString Lookup = Name;
    if (const int32 IdStart = Lookup.Find(TEXT(" 0x"), ESearchCase::IgnoreCase, ESearchDir::FromEnd); IdStart != INDEX_NONE)
        Lookup.RightChopInline(IdStart + 1);
    TSharedPtr<FT5Location> Match;
    for (const auto& Pair : Locations)
    {
        const auto& Location = Pair.Value;
        if (Location->EditorId.Equals(Lookup, ESearchCase::IgnoreCase) || Location->Key().Equals(Lookup, ESearchCase::IgnoreCase))
        {
            if (Match)
            {
                UE_LOG(LogT5Data, Error, TEXT("Ambiguous T5 EditorID '%s'. Use its FormID."), *Name);
                return;
            }
            Match = Location;
        }
    }
    if (Match)
        LoadLocation(Match->Id, Cell.Get(Match->SuggestedCell), true);
    else
        UE_LOG(LogT5Data, Error, TEXT("T5 map '%s' is absent from the active plugin list"), *Name);
}

void AT5Scene::LoadLocation(uint32 FormId, FIntPoint Cell, bool bRecenter)
{
    FT5LoadRequest Request;
    Request.FormId = FormId;
    Request.Cell = Cell;
    Request.Radius = FMath::Clamp(CellRadius, 1, 3);
    RequestScene(Request, bRecenter);
}

void AT5Scene::Tick(float DeltaTime)
{
    if (UIResources)
        UIResources->Poll();
    Super::Tick(DeltaTime);
    TickSceneBuild();
    if (auto* PC = GetWorld()->GetFirstPlayerController())
        if (auto* Player = Cast<AT5PlayerPawn>(PC->GetPawn()))
            Player->SuspendMovement(!LoadedScene || bRestoringCheckpoint || (bSceneLoading && bFullLoadingScreen)
                || LastPlayerVehicle || HasExecutionCamera(AppliedControls, AppliedGameFlags));
    TickCharacterCreation();
    if (CharacterCreator)
    {
        CharacterCreator->Tick();
        if (RuntimeAudio && Papyrus) RuntimeAudio->Tick(*this, *Papyrus,
            [](uint32) -> TOptional<FVector> { return {}; }, SimulationTime, true);
        return;
    }
    if (Papyrus && !MapBrowser && !SaveBrowser)
    {
        // Deliver contacts from the previous frame before advancing Papyrus and PACK.
        if (RuntimeArchery && LoadedScene)
            RuntimeArchery->Tick(DeltaTime, *LoadedScene, *Papyrus, ArcheryAssets,
                [this](uint32 Id) { return FindReferenceSkeleton(Id); }, InteractionComponents);
        // Do not let PACK outrun the collision data during an asynchronous cell load.
        // Keep rendering and loading, but stop advancing script/route time until support exists.
        FTransform SurfaceAuthored;
        uint32 SurfaceVehicle = 0;
        Papyrus->ReferenceTransform(0x14, SurfaceAuthored, SurfaceVehicle);
        const bool bAwaitingSurface = bRestoringCheckpoint || (bSceneLoading && (bFullLoadingScreen ||
            (SurfaceVehicle && Mechanics && !Mechanics->HasVehicleSurface(SurfaceVehicle))));
        if (Mechanics) Mechanics->SetPaused(bAwaitingSurface);
        if (!bAwaitingSurface)
        {
            // Chaos clamps long frames. PACK and dialogue must advance by the
            // same simulated interval, otherwise the horse outruns its carriage.
            const float Step=FMath::Min(DeltaTime,UPhysicsSettings::Get()->MaxPhysicsDeltaTime);
            SimulationTime += Step;
            if (auto* PC = GetWorld()->GetFirstPlayerController(); PC && PC->PlayerInput)
            {
                bool bInput = false, bGamepad = false;
                float Latest = -1;
                static const TArray<FKey> Keys = [] { TArray<FKey> Result; EKeys::GetAllKeys(Result); return Result; }();
                for (const auto& Key : Keys)
                    if (const auto* State = PC->PlayerInput->GetKeyState(Key))
                        if (!State->EventCounts[IE_Pressed].IsEmpty() || (Key.IsAnalog() && State->RawValue.SizeSquared() > .04))
                            if (!bInput || State->LastUpDownTransitionTime > Latest)
                            { bInput = true; bGamepad = Key.IsGamepadKey(); Latest = State->LastUpDownTransitionTime; }
                if (bInput) Papyrus->SetGamepad(bGamepad);
            }
            if (!SurfaceVehicle && LastPlayerScriptPose
                && !(AppliedControls & 1u) && !(AppliedGameFlags & (4u | 64u | 128u)))
                if (const auto* PC = GetWorld()->GetFirstPlayerController())
                    if (const auto* Pawn = PC->GetPawnOrSpectator())
                    {
                        const FTransform Origin = GetActorTransform();
                        FTransform Player = *LastPlayerScriptPose;
                        const auto* Character = Cast<AT5PlayerPawn>(Pawn);
                        const auto Foot = Character ? Origin.InverseTransformPosition(Character->Feet())
                            : Origin.InverseTransformPosition(Pawn->GetActorLocation()) - FVector(0, 0, Papyrus->PlayerEyeHeight());
                        const auto Heading = Origin.InverseTransformRotation(FRotator(0, PC->GetControlRotation().Yaw, 0).Quaternion());
                        if (FVector::DistSquared(Foot, Player.GetLocation()) > .25 || !Player.GetRotation().Equals(Heading))
                        {
                            Player.SetLocation(Foot);
                            Player.SetRotation(Heading);
                            Papyrus->AnimationPose(0x14, Player, false);
                            LastPlayerScriptPose = Player;
                            LastPlayerHeading = Origin.TransformRotation(Player.GetRotation());
                        }
                    }
            Papyrus->Tick(Step);
        }
        for (const auto& [Id, Parts] : ReferenceVisuals)
            for (const auto& Part : Parts)
                if (auto* Mesh = Cast<USkeletalMeshComponent>(Part.Component.Get()))
                    Mesh->GlobalAnimRateScale = bAwaitingSurface ? 0.f
                        : FMath::Min(1.f,UPhysicsSettings::Get()->MaxPhysicsDeltaTime/FMath::Max(DeltaTime,UE_SMALL_NUMBER));
        ApplyPapyrusState();
        ApplyPapyrusWorld();
        TickPlayerBody();
        TickFirstPerson();
        // Bootstrap scripts may create new references. Keep the loading screen until
        // their scene rebuild and the authored camera have both been applied.
        if (bStartupPreparing && !bRestoringCheckpoint && !bSceneLoading && LoadedSpawnCount == Papyrus->SpawnedCount()
            && bOpeningViewReady)
        {
            Papyrus->BeginEnvironment(ActiveWorld);
            bStartupPreparing = false;
            UE_LOG(LogT5Data, Display, TEXT("T5_LOADING_READY refs=%u camera=1"), LoadedSpawnCount);
        }
        FinishCheckpointLoad();
        if (!bStartupPreparing) TickPresentation();
        TickDialogue();
        TickRequestedSave();
        if (!RuntimeAudio)
            RuntimeAudio = MakeShared<FT5AudioRuntime>();
        if (!bStartupPreparing) RuntimeAudio->Tick(*this, *Papyrus, [this](uint32 Source) -> TOptional<FVector>
        {
            if (Source == 0x14)
            {
                if (const auto* Controller = GetWorld()->GetFirstPlayerController())
                    if (const auto* Pawn = Controller->GetPawnOrSpectator())
                        return Pawn->GetPawnViewLocation();
            }
            if (const auto* Visuals = ReferenceVisuals.Find(Source))
                for (const auto& Visual : *Visuals)
                    if (auto* Mesh = Cast<USkeletalMeshComponent>(Visual.Component.Get()))
                    {
                        static const FName Head(TEXT("NPC Head [Head]"));
                        if (Mesh->GetBoneIndex(Head) != INDEX_NONE)
                            return Mesh->GetBoneLocation(Head);
                    }
            FTransform Authored;
            uint32 Parent;
            if (const auto Pose = Papyrus->ReferenceTransform(Source, Authored, Parent))
                return GetActorTransform().TransformPosition(
                    (Mechanics ? Mechanics->CorrectPose(Source, Parent, *Pose) : *Pose).GetLocation());
            return {};
        }, SimulationTime, bAwaitingSurface);
        TickFaces();
        const uint32 RuntimeRefCount = Papyrus->SpawnedCount();
        if (!bSceneLoading && ActiveLocation && (!ActiveWorld || RuntimeRefCount != LoadedSpawnCount))
        {
            const double Now = GetWorld()->GetTimeSeconds();
            if (SpawnReloadDue == 0)
                SpawnReloadDue = Now + (bStartupPreparing ? 0 : .35);
            if (Now >= SpawnReloadDue)
            {
                SpawnReloadDue = Now + .35;
                FT5LoadRequest Snapshot;
                Papyrus->Capture(Snapshot);
                const int32 Radius = FMath::Clamp(CellRadius, 1, 3);
                const auto* Arrival = Snapshot.SpawnedReferences.FindByPredicate([&](const auto& Ref)
                {
                    if (ReferenceVisuals.Contains(Ref.Id) || !(Ref.Flags & 4))
                        return false;
                    if (!ActiveWorld)
                        return Ref.Cell == ActiveLocation && !LoadedInteriorArrivals.Contains(Ref.Id);
                    return FMath::Abs(FMath::FloorToInt(Ref.Position[0] / T5CellSize) - RequestedCell.X) <= Radius
                        && FMath::Abs(FMath::FloorToInt(Ref.Position[1] / T5CellSize) - RequestedCell.Y) <= Radius;
                });
                if (Arrival)
                {
                    UE_LOG(LogT5Data, Verbose, TEXT("T5_REFERENCE_ARRIVAL ref=%08X base=%08X flags=%u cell=%08X"), Arrival->Id, Arrival->Base, Arrival->Flags, Arrival->Cell);
                    LoadLocation(ActiveLocation, RequestedCell, false);
                    return;
                }
                LoadedSpawnCount = RuntimeRefCount;
            }
        }
    }
    if (!ActiveWorld || bSceneLoading || MapBrowser || !GetWorld()->IsGameWorld())
        return;
    const auto* Location = Locations.Find(ActiveWorld);
    const auto* Controller = GetWorld()->GetFirstPlayerController();
    const auto* Pawn = Controller ? Controller->GetPawnOrSpectator() : nullptr;
    if (!Location || !Pawn)
        return;
    // Streaming follows the player's logical position, independent of seat and camera offsets.
    FTransform Authored;
    uint32 Vehicle = 0;
    const auto Player = Papyrus ? Papyrus->ReferenceTransform(0x14, Authored, Vehicle) : TOptional<FTransform>{};
    const FVector View = UnrealToT5Position(Player && Vehicle ? Player->GetLocation()
        : GetActorTransform().InverseTransformPosition(Pawn->GetActorLocation()));
    const FIntPoint Cell(FMath::FloorToInt(View.X / T5CellSize), FMath::FloorToInt(View.Y / T5CellSize));
    const float Margin = FMath::Clamp(StreamingHysteresis, 0.f, .45f);
    const FVector2D Local(View.X / T5CellSize - RequestedCell.X, View.Y / T5CellSize - RequestedCell.Y);
    if (Local.X >= -Margin && Local.X < 1 + Margin && Local.Y >= -Margin && Local.Y < 1 + Margin)
        return;
    const int32 Radius = FMath::Clamp(CellRadius, 1, 3);
    for (int32 X = -Radius; X <= Radius; ++X)
        for (int32 Y = -Radius; Y <= Radius; ++Y)
            if ((*Location)->Cells.Contains(Cell + FIntPoint(X, Y)))
            {
                LoadLocation(ActiveWorld, Cell, false);
                return;
            }
}

USkeletalMeshComponent* AT5Scene::FindReferenceSkeleton(uint32 Id) const
{
    if (const auto* Visuals = ReferenceVisuals.Find(Id))
        for (const auto& Visual : *Visuals)
            if (auto* Mesh = Cast<USkeletalMeshComponent>(Visual.Component.Get()))
                return Mesh;
    return nullptr;
}

void AT5Scene::TickPresentation()
{
    if (!RuntimeArchery)
        RuntimeArchery = MakeShared<FT5Archery>();
    const double Now = GetWorld()->GetTimeSeconds();
    auto* Controller = GetWorld()->GetFirstPlayerController();
    auto* Viewport = GetWorld()->GetGameViewport();
    uint32 Controls, Flags; float SittingRotation;
    Papyrus->GameState(Controls, Flags, SittingRotation);
    const bool bShowCompass = Controller && Viewport && LoadedScene && (!OpeningQuest || bOpeningViewReady)
        && !(Flags & (2u | 64u)) && !bStartupPreparing && !MapBrowser;
    bCompassVisible = bShowCompass;
    if (bShowCompass && !Compass)
    {
        Compass = SNew(ST5Compass).Resources(UIResources)
            .Visibility_Lambda([Weak = TWeakObjectPtr<AT5Scene>(this)]
            {
                const auto* Scene = Weak.Get();
                return Scene && Scene->bCompassVisible && !Scene->MapBrowser && !Scene->InventoryMenu
                    && !Scene->SaveBrowser && !Scene->CharacterCreator
                    && !(Scene->bSceneLoading && Scene->bFullLoadingScreen)
                    ? EVisibility::HitTestInvisible : EVisibility::Collapsed;
            });
        Viewport->AddViewportWidgetContent(Compass.ToSharedRef(), 80);
    }
    TArray<FT5CompassTarget> CompassTargets;
    FVector Eye;
    FRotator View = FRotator::ZeroRotator;
    int32 Width = 0, Height = 0;
    if (bShowCompass)
    {
        Controller->GetPlayerViewPoint(Eye, View);
        Controller->GetViewportSize(Width, Height);
    }
    for (const auto& Value : Papyrus->TakePresentation())
    {
        const auto& Args = Value->AsArray();
        if (Args.IsEmpty())
            continue;
        if (Args[0]->AsString() == TEXT("dialogue") && Args.Num() == 2)
        {
            OpenDialogue(uint32(Args[1]->AsNumber()));
            continue;
        }
        if (Args[0]->AsString() == TEXT("inventory") && Args.Num() == 2)
        {
            CloseDialogue();
            OpenInventory(uint32(Args[1]->AsNumber()));
            continue;
        }
        if (Args[0]->AsString() == TEXT("objectives"))
        {
            CompassTargets.Reset();
            if (bShowCompass)
                for (int32 I = 1; I < Args.Num(); ++I)
                {
                    const auto& Marker = Args[I]->AsArray();
                    if (Marker.Num() != 4) continue;
                    const uint32 Id = Marker[0]->AsNumber();
                    FVector Point = GetActorTransform().TransformPosition(T5ToUnrealPosition(FVector(
                        Marker[1]->AsNumber(), Marker[2]->AsNumber(), Marker[3]->AsNumber())));
                    if (const auto* Rig = FindReferenceSkeleton(Id); Rig && Rig->IsVisible())
                        Point = Rig->Bounds.Origin + FVector(0,0,Rig->Bounds.BoxExtent.Z);
                    const auto Direction = Point - Eye;
                    FVector2D Screen;
                    TOptional<FVector2D> UV;
                    if (Width > 0 && Height > 0 && Controller->ProjectWorldLocationToScreen(Point, Screen))
                        UV = FVector2D(Screen.X / Width, Screen.Y / Height);
                    const bool bDoor = Mechanics && Mechanics->IsAnimatedDoor(Id);
                    CompassTargets.Add({FMath::RadiansToDegrees(FMath::Atan2(Direction.Y, Direction.X)), UV, bDoor});
                }
            continue;
        }
        if (Args[0]->AsString() == TEXT("doorTravel") && Args.Num() == 5)
        {
            // Keep the loading screen visible while the in-flight exterior
            // build finishes. The native transition remains queued for attach.
            if (bSceneLoading) bFullLoadingScreen = true;
            if (!bSceneLoading)
            {
                UE_LOG(LogT5Data, Display, TEXT("T5_DOOR_TRAVEL ref=%08X destination=%08X"),
                    uint32(Args[1]->AsNumber()), uint32(Args[2]->AsNumber()));
                LoadLocation(Args[2]->AsNumber(), FIntPoint(Args[3]->AsNumber(), Args[4]->AsNumber()), true);
            }
            continue;
        }
        if (Args[0]->AsString() == TEXT("destruction") && Args.Num() == 5)
        {
            ApplyDestruction(Args[1]->AsNumber(), Args[2]->AsNumber(), Args[3]->AsBool(), Args[4]->AsBool());
            continue;
        }
        if (Args[0]->AsString() == TEXT("modelHkx") && (Args.Num() == 5 || Args.Num() == 7))
        {
            if (Mechanics) Mechanics->SetHkxSequence(Args[1]->AsNumber(), Args[2]->AsString(),
                Args[3]->AsString(), Args[4]->AsNumber(), *Papyrus,
                Args.Num() == 7 ? Args[5]->AsString() : FString(), Args.Num() == 7 ? Args[6]->AsNumber() : 0.);
            continue;
        }
        if (Args[0]->AsString() == TEXT("modelAnimation") && Args.Num() == 4)
        {
            if (Mechanics) Mechanics->SetSequence(Args[1]->AsNumber(), Args[2]->AsString(), Args[3]->AsNumber());
            continue;
        }
        if (Args[0]->AsString() == TEXT("radialImpulse") && Args.Num() == 6)
        {
            if (LoadedScene)
            {
                const auto Origin = GetActorTransform().TransformPosition(T5ToUnrealPosition(FVector(
                    Args[1]->AsNumber(), Args[2]->AsNumber(), Args[3]->AsNumber())));
                TInlineComponentArray<UPrimitiveComponent*> Parts(LoadedScene);
                for (auto* Part : Parts)
                    if (Part->IsSimulatingPhysics())
                        Part->AddRadialImpulse(Origin, Args[4]->AsNumber() * T5CentimetersPerUnit,
                            Args[5]->AsNumber() * 100., RIF_Linear, true);
            }
            continue;
        }
        if (Args[0]->AsString() == TEXT("imagespace") && Args.Num() == 10)
        {
            if (!ScriptPostProcess.IsValid())
            {
                auto* Component = NewObject<UPostProcessComponent>(this, NAME_None, RF_Transient);
                Component->SetupAttachment(GetRootComponent());
                Component->bUnbound = true; Component->Priority = 1100;
                Component->RegisterComponent(); ScriptPostProcess = Component;
            }
            auto& S = ScriptPostProcess->Settings;
            ScriptPostProcess->BlendWeight = Args[9]->AsNumber() != 0 ? 1.f : 0.f;
            const auto Global = [&](int32 I) { return FVector4(1,1,1,FMath::Clamp(Args[I]->AsNumber(),0.,8.)); };
            S.bOverride_ColorSaturation = S.bOverride_ColorGain = S.bOverride_ColorContrast = true;
            S.ColorSaturation = Global(1); S.ColorGain = Global(2); S.ColorContrast = Global(3);
            S.bOverride_BloomIntensity = !FMath::IsNearlyEqual(Args[4]->AsNumber(),1.) || !FMath::IsNearlyZero(Args[5]->AsNumber());
            S.BloomIntensity = FMath::Clamp(.675 * Args[4]->AsNumber() + Args[5]->AsNumber(),0.,8.);
            // Map the original full-screen blur envelope to UE's cinematic DOF.
            const float Blur = FMath::Max(0.,Args[6]->AsNumber());
            S.bOverride_DepthOfFieldFstop = S.bOverride_DepthOfFieldFocalDistance = Blur > .001f;
            S.DepthOfFieldFocalDistance = 10; S.DepthOfFieldFstop = FMath::Max(.2f, 4.f / FMath::Max(.01f,Blur));
            S.bOverride_VignetteIntensity = Args[7]->AsNumber() > .001;
            S.VignetteIntensity = FMath::Clamp(Args[7]->AsNumber(),0.,1.);
            S.bOverride_MotionBlurAmount = Args[8]->AsNumber() > .001;
            S.MotionBlurAmount = FMath::Clamp(Args[8]->AsNumber(),0.,1.);
            continue;
        }
        if (Args[0]->AsString() == TEXT("requestsave") || Args[0]->AsString() == TEXT("requestautosave"))
        {
            const bool bManual = Args[0]->AsString() == TEXT("requestsave");
            if (bManual || PendingScriptSave.IsEmpty()) PendingScriptSave = bManual ? TEXT("Manual") : TEXT("Auto");
            continue;
        }
        if (Args[0]->AsString() == TEXT("face") && Args.Num() == 5)
        {
            auto& Face = Faces.FindOrAdd(static_cast<uint32>(Args[2]->AsNumber()));
            switch (static_cast<int32>(Args[1]->AsNumber()))
            {
                case 0: Face.Target = static_cast<uint32>(Args[3]->AsNumber()); break;
                case 1: Face.bTracking = Args[4]->AsNumber() != 0; break;
                case 2: Face.Expression = static_cast<int32>(static_cast<uint32>(Args[3]->AsNumber()));
                    Face.Strength = Args[4]->AsNumber(); break;
            }
            continue;
        }
        if (Args[0]->AsString() == TEXT("arrow"))
        {
            if (LoadedScene)
                RuntimeArchery->Command(*LoadedScene, *Papyrus, Args, ArcheryAssets,
                    [this](uint32 Id) { return FindReferenceSkeleton(Id); }, InteractionComponents);
            continue;
        }
        if (Args[0]->AsString() == TEXT("spellProjectile"))
        {
            if (LoadedScene)
                RuntimeArchery->LaunchSpell(*LoadedScene, Args, ArcheryAssets,
                    [this](uint32 Id) { return FindReferenceSkeleton(Id); });
            continue;
        }
        if (Args[0]->AsString() == TEXT("actorFall") && Args.Num() == 6)
        {
            SweepActorFall(Args[1]->AsNumber(), T5ToUnrealPosition(FVector(Args[2]->AsNumber(), Args[3]->AsNumber(), Args[4]->AsNumber())),
                Args[5]->AsNumber());
            continue;
        }
        if (Args[0]->AsString() == TEXT("firingPosition") && Args.Num() == 6)
        {
            FindFiringPosition(Args[1]->AsNumber(), Args[2]->AsNumber(), Args[3]->AsNumber(),
                Args[4]->AsNumber() * T5CentimetersPerUnit, Args[5]->AsNumber() * T5CentimetersPerUnit);
            continue;
        }
        if (!Titles)
            if (Viewport)
            {
                Titles = SNew(ST5Titles).Visibility(EVisibility::HitTestInvisible);
                Viewport->AddViewportWidgetContent(Titles.ToSharedRef(), 90);
            }
        if (Titles)
            Titles->Command(Args, Now, *Papyrus);
    }
    if (Compass && bShowCompass)
    {
        if (Now >= NextPoolRefresh)
        {
            bool bDrawn = false;
            Papyrus->PlayerViewKey(bDrawn);
            Compass->SetResourcePools(Papyrus->ResourcePoolRatios(), bDrawn);
            NextPoolRefresh = Now + .2;
        }
        Compass->Update(View.Yaw, MoveTemp(CompassTargets), !DialogueMenu && !(Controls & (1u << 6)));
    }
    if (Titles)
    {
        Titles->SetWorldReady(!OpeningQuest || bOpeningViewReady);
        Titles->Advance(Now);
    }
}

void AT5Scene::TickFaces()
{
    // INFO TRDT / SCEN emotion enumeration. Papyrus expression indices use a separate table.
    static const FName Moods[] = { NAME_None, TEXT("DialogueAnger"), TEXT("DialogueDisgusted"), TEXT("DialogueFear"),
        TEXT("DialogueSad"), TEXT("DialogueHappy"), TEXT("DialogueSurprise"), TEXT("DialoguePuzzled") };
    static const FName Overrides[] = { TEXT("DialogueAnger"), TEXT("DialogueFear"), TEXT("DialogueHappy"), TEXT("DialogueSad"),
        TEXT("DialogueSurprise"), TEXT("DialoguePuzzled"), TEXT("DialogueDisgusted"), TEXT("MoodNeutral"), TEXT("MoodAnger"),
        TEXT("MoodFear"), TEXT("MoodHappy"), TEXT("MoodSad"), TEXT("MoodSurprise"), TEXT("MoodPuzzled"), TEXT("MoodDisgusted"),
        TEXT("CombatAnger"), TEXT("CombatShout") };
    for (const auto& [Id, Visuals] : ReferenceVisuals)
    {
        const FFaceState* Face = Faces.Find(Id);
        TOptional<FVector> Target;
        if (Face && Face->bTracking && Face->Target)
        {
            if (Face->Target == 0x14)
            {
                if (const auto* Controller = GetWorld()->GetFirstPlayerController())
                { FVector Location; FRotator Rotation; Controller->GetPlayerViewPoint(Location, Rotation); Target = Location; }
            }
            else if (const auto* Mesh = FindReferenceSkeleton(Face->Target))
                Target = Mesh->GetBoneIndex(TEXT("NPC Head [Head]")) != INDEX_NONE
                    ? Mesh->GetBoneLocation(TEXT("NPC Head [Head]")) : Mesh->GetComponentLocation();
            else
            {
                FTransform Original; uint32 Parent;
                if (const auto Transform = Papyrus->ReferenceTransform(Face->Target, Original, Parent))
                    Target = GetActorTransform().TransformPosition(Transform->GetLocation());
            }
        }
        const auto Emotion = RuntimeAudio->Expression(Id);
        FName Expression = Emotion.Key > 0 && Emotion.Key < UE_ARRAY_COUNT(Moods) ? Moods[Emotion.Key] : NAME_None;
        float Strength = Emotion.Value;
        if (Face && Face->Expression >= 0 && Face->Expression < UE_ARRAY_COUNT(Overrides))
        { Expression = Overrides[Face->Expression]; Strength = Face->Strength; }
        TArray<float> LipWeights;
        RuntimeAudio->FacialWeights(Id, SimulationTime, LipWeights);
        for (const auto& Visual : Visuals)
            if (auto* Mesh = Cast<USkeletalMeshComponent>(Visual.Component.Get()))
            {
                if (auto* Animation = Cast<UT5HkxAnimInstance>(Mesh->GetAnimInstance()))
                    Animation->SetLookTarget(Target ? TOptional<FVector>(Mesh->GetComponentTransform().InverseTransformPosition(*Target)) : TOptional<FVector>{});
                const float Blend = 1.f - FMath::Exp(-8.f * GetWorld()->GetDeltaSeconds());
                for (const auto& Morph : Overrides)
                    Mesh->SetMorphTarget(Morph, FMath::Lerp(Mesh->GetMorphTarget(Morph), Morph == Expression ? Strength : 0.f, Blend));
                for (int32 Channel = 0; Channel < LipWeights.Num(); ++Channel)
                    Mesh->SetMorphTarget(T5LipMorphNames()[Channel], LipWeights[Channel]);
            }
    }
}

void AT5Scene::InteractT5()
{
    if (!Papyrus || MapBrowser || SaveBrowser || InventoryMenu || DialogueMenu || CharacterCreator || bRestoringCheckpoint
        || (bSceneLoading && bFullLoadingScreen) || !GetWorld() || (AppliedControls & (1u << 6)))
        return;
    const auto* Controller = GetWorld()->GetFirstPlayerController();
    if (!Controller)
        return;
    FVector Eye;
    FRotator View;
    Controller->GetPlayerViewPoint(Eye, View);
    if (const auto* Pawn = Cast<AT5PlayerPawn>(Controller->GetPawn()))
        Eye = Pawn->GetPawnViewLocation();
    TArray<FHitResult> Hits;
    FCollisionQueryParams Query(SCENE_QUERY_STAT(T5Activate), true, Controller->GetPawnOrSpectator());
    GetWorld()->LineTraceMultiByChannel(Hits, Eye, Eye + View.Vector() * 450, ECC_Visibility, Query);
    for (const auto& Hit : Hits)
        if (const auto* Id = InteractionComponents.Find(Hit.GetComponent()); Id && Papyrus->Activate(*Id))
        {
            ApplyPapyrusState();
            TickPresentation();
            return;
        }
    const auto End = !Hits.IsEmpty() && Hits.Last().bBlockingHit ? Hits.Last().ImpactPoint : Eye + View.Vector() * 450;
    if (const auto Id = FindInteractionReference(Eye, End, true); Id && Papyrus->Activate(Id))
    {
        ApplyPapyrusState();
        TickPresentation();
    }
}

void AT5Scene::ApplyDestruction(uint32 Id, int32 Stage, bool bNoCollision, bool bHasModel)
{
    if (!LoadedScene || !ReferenceVisuals.Contains(Id)) return;
    auto& Damage = DamageVisuals.FindOrAdd(Id);
    if (Damage.Stage == Stage) return;
    if (auto* Previous = Damage.Replacement.Component.Get()) Previous->DestroyComponent();
    Damage = {Stage, bNoCollision, {}};
    if (bHasModel && !Papyrus->IsDisabled(Id).Get(false))
    {
        FTransform Authored;
        uint32 Parent = 0;
        if (const auto Pose = Papyrus->ReferenceTransform(Id, Authored, Parent))
            if (const auto Data = Papyrus->DestructionModel(Id))
            {
                Damage.Replacement = BuildAnimationProp(*Data);
                if (auto* Component = Damage.Replacement.Component.Get())
                {
                    Component->AttachToComponent(LoadedScene->GetRootComponent(),
                        FAttachmentTransformRules::KeepRelativeTransform);
                    Component->SetRelativeTransform(*Pose);
                    Component->SetVisibility(true);
                }
            }
    }
}

void AT5Scene::ApplyPapyrusState()
{
    for (const auto& [Id, Visuals] : ReferenceVisuals)
        if (const auto Disabled = Papyrus->IsDisabled(Id))
            for (const auto& Visual : Visuals)
                if (auto* Component = Visual.Component.Get())
                {
                    const auto* Damage = DamageVisuals.Find(Id);
                    const bool bShowOriginal = !*Disabled && !(Damage && Damage->Replacement.Component.IsValid());
                    if (!Visual.bColliderOnly && Component->IsVisible() != bShowOriginal)
                        Component->SetVisibility(bShowOriginal);
                    if (auto* Primitive = Cast<UPrimitiveComponent>(Component))
                    {
                        const auto Collision = *Disabled || (Damage && Damage->bNoCollision)
                            ? ECollisionEnabled::NoCollision : Visual.Collision;
                        if (Primitive->GetCollisionEnabled() != Collision)
                            Primitive->SetCollisionEnabled(Collision);
                    }
                }
}

bool AT5Scene::HasExecutionCamera(uint32 Controls, uint32 Flags) const
{
    if (!Papyrus) return false;
    const auto* Request = Papyrus->AnimationRequests().Find(0x14);
    return Request && ((Flags & (4u | 128u)) || (Controls & 1u))
        && (Request->Event == TEXT("IdleExecutionerChop_Player")
            || Request->Event == TEXT("IdleExecutionerAlduinReactionDeath")
            || Request->Event.StartsWith(TEXT("Behavior37/")));
}

void AT5Scene::ApplyPapyrusWorld()
{
    if (bRestoringCheckpoint) return;
    TickAnimationVisuals();
    TickRagdolls();
    if (Mechanics && IsValid(LoadedScene))
        Mechanics->Tick(*LoadedScene, *Papyrus, ActiveWorld != 0);
    TickAnimations();
    auto* Controller = GetWorld()->GetFirstPlayerController();
    uint32 Controls, Flags;
    float SittingRotation;
    Papyrus->GameState(Controls, Flags, SittingRotation);
    if (Controller)
    {
        const auto MoveLocked = [](uint32 Mask, uint32 State) { return (Mask & 1u) || (State & (4u | 128u)); };
        if (MoveLocked(Controls, Flags) != MoveLocked(AppliedControls, AppliedGameFlags))
            Controller->SetIgnoreMoveInput(MoveLocked(Controls, Flags));
        if ((Controls ^ AppliedControls) & (1u << 3))
            Controller->SetIgnoreLookInput((Controls & (1u << 3)) != 0);
        if (auto* Hud = Controller->GetHUD(); Hud && ((Flags ^ AppliedGameFlags) & 2u))
            Hud->bShowHUD = (Flags & 2u) == 0;
        FTransform Authored;
        uint32 Vehicle = 0;
        const bool bExecutionCamera = HasExecutionCamera(Controls, Flags);
        auto* Character = Cast<AT5PlayerPawn>(Controller->GetPawn());
        if (Character && !bExecutionCamera)
            Character->SetScriptedEye({});
        if (auto* Pawn = Controller->GetPawnOrSpectator())
            if (const auto Player = Papyrus->ReferenceTransform(0x14, Authored, Vehicle);
                Player && (Vehicle || bExecutionCamera || LastPlayerVehicle || !LastPlayerScriptPose || !Player->Equals(*LastPlayerScriptPose)))
            {
                LastPlayerScriptPose = *Player;
                LastPlayerVehicle = Vehicle;
                const FTransform Origin = GetActorTransform();
                const auto PlayerPose = Mechanics ? Mechanics->CorrectPose(0x14, Vehicle, *Player) : *Player;
                FVector Eye = PlayerPose.GetLocation() + FVector(0, 0, Vehicle ? 92*CentimetersPerT5Unit : Papyrus->PlayerEyeHeight());
                FQuat LocalHeading = PlayerPose.GetRotation();
                if (Vehicle || bExecutionCamera)
                {
                    const auto* Request = Papyrus->AnimationRequests().Find(0x14);
                    if (Request && Request->Serial != PlayerAnimationSerial)
                    {
                        PlayerAnimationSerial = Request->Serial;
                        PlayerAnimationStart = SimulationTime;
                        PlayerCamera = MakeShared<FT5HkxCameraClip>();
                        FString Error;
                        if (!LoadT5HkxPlayerCamera(Request->Behavior, Request->Event,
                            [this](const FString& Path, TArray<uint8>& Bytes) { return Papyrus->ReadResource(Path, Bytes); },
                            *PlayerCamera, Error, Request->Clip))
                        {
                            PlayerCamera.Reset();
                            UE_LOG(LogT5Data, Warning, TEXT("T5_HKX_CAMERA_ERROR idle=%08X %s"), Request->Idle, *Error);
                        }
                        else
                        {
                            LastPlayerHeading.Reset();
                            UE_LOG(LogT5Data, Display, TEXT("T5_HKX_CAMERA idle=%08X event=%s"), Request->Idle, *Request->Event);
                        }
                    }
                    FTransform AuthoredVehicle;
                    uint32 Ancestor = 0;
                    if (const auto Cart = Vehicle ? Papyrus->ReferenceTransform(Vehicle, AuthoredVehicle, Ancestor) : Player; Cart && PlayerCamera)
                    {
                        const float Elapsed = SimulationTime - PlayerAnimationStart;
                        // The third-person actor clock dispatches gameplay events.
                        // Camera clips are presentation and can have different durations.
                        const auto CartPose = Vehicle && Mechanics ? Mechanics->CorrectPose(Vehicle, Ancestor, *Cart) : *Cart;
                        const auto* Animation = ReferenceAnimations.Find(0x14);
                        const FTransform Root = Animation && (*Animation)->bLoaded ? (*Animation)->Pose : CartPose;
                        const FTransform Camera = SampleT5HkxPlayerCamera(*PlayerCamera, Elapsed) * Root;
                        Eye = Camera.GetLocation();
                        LocalHeading = Camera.GetRotation();
                        if (!bOpeningViewReady)
                        {
                            bOpeningViewReady = true;
                            UE_LOG(LogT5Data, Display, TEXT("T5_OPENING_CAMERA_READY vehicle=%08X"), Vehicle);
                        }
                    }
                }
                if (!VehiclePostProcess.IsValid() && Vehicle)
                {
                    auto* Effects = NewObject<UPostProcessComponent>(this, NAME_None, RF_Transient);
                    Effects->bUnbound = true;
                    Effects->Priority = 1000;
                    Effects->Settings.bOverride_MotionBlurAmount = true;
                    Effects->Settings.MotionBlurAmount = 0;
                    Effects->RegisterComponent();
                    VehiclePostProcess = Effects;
                }
                if (auto* Effects = VehiclePostProcess.Get())
                    Effects->bEnabled = Vehicle != 0;
                if (Character)
                {
                    Character->SetEyeHeight(Papyrus->PlayerEyeHeight() * Origin.GetScale3D().Z);
                    if (Vehicle || bExecutionCamera)
                        Character->SetScriptedEye(Origin.TransformPosition(Eye));
                    Character->PlaceFeet(Origin.TransformPosition(PlayerPose.GetLocation()));
                }
                else
                    Pawn->SetActorLocation(Origin.TransformPosition(Eye), false, nullptr, ETeleportType::TeleportPhysics);
                if (OpeningQuest && (!LastLoggedPlayerPose || FVector::DistSquared(*LastLoggedPlayerPose, Eye) > FMath::Square(600.)))
                {
                    LastLoggedPlayerPose = Eye;
                    UE_LOG(LogT5Data, Display, TEXT("T5_PLAYER_POSE vehicle=%08X %s"), Vehicle, *Eye.ToCompactString());
                }
                // Roll in the terrain fit belongs to the chassis. The scripted camera
                // still drives its heading while the local view keeps a stable horizon.
                auto ViewBase = (Origin.GetRotation() * LocalHeading).Rotator();
                if (Vehicle || !bExecutionCamera) ViewBase.Roll = 0;
                const FQuat Heading = ViewBase.Quaternion();
                if (!LastPlayerHeading)
                {
                    auto View = ViewBase;
                    View.Yaw += Vehicle ? SittingRotation : 0;
                    Controller->SetControlRotation(View);
                    AppliedSittingRotation = Vehicle ? SittingRotation : 0;
                }
                else if (!Heading.Equals(*LastPlayerHeading))
                {
                    auto View = (Heading * LastPlayerHeading->Inverse()
                        * Controller->GetControlRotation().Quaternion()).Rotator();
                    if (Vehicle || !bExecutionCamera) View.Roll = 0;
                    Controller->SetControlRotation(View);
                }
                if (Vehicle && SittingRotation != AppliedSittingRotation)
                {
                    auto View = Controller->GetControlRotation();
                    View.Yaw += SittingRotation - AppliedSittingRotation;
                    Controller->SetControlRotation(View);
                    AppliedSittingRotation = SittingRotation;
                }
                LastPlayerHeading = Heading;
            }
        // The player can regain control without a change in the actor transform.
        if (!Vehicle && !bExecutionCamera)
        {
            auto View = Controller->GetControlRotation();
            if (!FMath::IsNearlyZero(FRotator::NormalizeAxis(View.Roll)))
            {
                View.Roll = 0;
                Controller->SetControlRotation(View);
            }
        }
    }
    AppliedControls = Controls;
    AppliedGameFlags = Flags;

    for (auto& [Id, Visuals] : ReferenceVisuals)
    {
        if (Ragdolled.Contains(Id)) continue;
        FTransform Authored;
        uint32 Parent;
        const auto Current = Papyrus->ReferenceTransform(Id, Authored, Parent);
        const auto* Request = Papyrus->AnimationRequests().Find(Id);
        auto* Mount = Parent ? FindReferenceSkeleton(Parent) : nullptr;
        const bool bRider = Mount && Mount->GetBoneIndex(TEXT("SaddleBone")) != INDEX_NONE;
        if (bRider)
            Request = Papyrus->AnimationRequests().Find(Parent);
        bool bAnnotationOwner = false;
        for (auto& Visual : Visuals)
        {
            auto* Component = Visual.Component.Get();
            if (!Component || Component->GetAttachParent() != LoadedScene->GetRootComponent())
                continue;
            auto* Mesh = Cast<USkeletalMeshComponent>(Component);
            const uint64 Serial = Request ? Request->Serial : MAX_uint64;
            const auto* Scripted=ReferenceAnimations.Find(Id);
            const bool bReturnToLocomotion=Scripted && (*Scripted)->Serial==Serial
                && (*Scripted)->Clock.Playback.bReturnToLocomotion;
            const FString Gait = bReturnToLocomotion ? TEXT("T5Idle") : Request ? Request->Event : TEXT("T5Idle");
            const bool bLocomotion = Gait == TEXT("T5Idle") || Gait == TEXT("T5Walk")
                || Gait == TEXT("T5FastWalk") || Gait == TEXT("T5Jog") || Gait == TEXT("T5Run");
            if (Mesh && (Visual.AnimationSerial != Serial || Visual.AnimationMount != (bRider ? Parent : 0)))
            {
                Visual.AnimationSerial = Serial;
                Visual.AnimationMount = bRider ? Parent : 0;
                const auto ResumeAt = Visual.ResumeSerial == Serial ? Visual.ResumeAt : TOptional<float>{};
                Visual.ResumeAt.Reset();
                FString Error;
                const TWeakPtr<FT5PapyrusRuntime> Runtime = Papyrus;
                const bool bBow = Gait == TEXT("T5Bow");
                TFunction<void(const FString&)> Annotation;
                Visual.bPlayedIdle = bLocomotion ? PlayLocomotion(*Mesh, Visual.AnimationProject,
                        bRider ? TEXT("Rider") + Gait : Gait, Request ? Request->Speed : 1.f, ResumeAt, Error)
                    : PlayT5Hkx(*Mesh, Request->Behavior, Request->Event, Request->Clip,
                        [this](const FString& Path, TArray<uint8>& Bytes) { return Papyrus->ReadResource(Path, Bytes); },
                        Error, Visual.SkeletonPath, MoveTemp(Annotation), Request->Speed, bBow || Gait == TEXT("T5Execution"), false, ResumeAt);
                if (!Visual.bPlayedIdle && !Error.IsEmpty())
                {
                    UE_LOG(LogT5Data, Warning, TEXT("T5_HKX_ERROR ref=%08X event=%s %s"), Id, *Gait, *Error);
                }
                else if (bBow && !bAnnotationOwner && RuntimeArchery)
                    RuntimeArchery->AnimateBow(Id, Request->Clip, Request->Speed, *Papyrus);
            }
            if (Mesh && Visual.bPlayedIdle)
            {
                bAnnotationOwner = true;
                if (const auto* Animation = ReferenceAnimations.Find(Id); Animation && (*Animation)->bLoaded && !bRider)
                    if (auto* Instance = Cast<UT5HkxAnimInstance>(Mesh->GetAnimInstance()))
                        Instance->Synchronize(T5HkxPosition((*Animation)->Clock, SimulationTime - (*Animation)->Start),
                            !(*Animation)->Clock.Playback.BoneWeights.IsEmpty());
            }
            if (Current)
            {
                FTransform Pose = Mechanics ? Mechanics->CorrectPose(Id, Parent, *Current) : *Current;
                if (const auto* Animation = ReferenceAnimations.Find(Id);
                    Animation && (*Animation)->bLoaded && !bRider && Visual.bPlayedIdle)
                    Pose = (*Animation)->Pose;
                if (bRider && Visual.bPlayedIdle)
                {
                    FTransform MountAuthored;
                    uint32 Ancestor;
                    if (const auto MountPose = Papyrus->ReferenceTransform(Parent, MountAuthored, Ancestor))
                    {
                        const FTransform Grounded = Mechanics ? Mechanics->CorrectPose(Parent, Ancestor, *MountPose) : *MountPose;
                        // The horse NIF/HKX defines the saddle origin and rotation. Rider clips
                        // are authored about that origin, not about the placed ACHR beside it.
                        FTransform LocalMount = FTransform::Identity;
                        for (const auto& Part : ReferenceVisuals.FindChecked(Parent))
                            if (Part.Component.Get() == Mount)
                                LocalMount = Part.Original * MountAuthored.Inverse();
                        const auto Saddle = Mount->GetSocketTransform(TEXT("SaddleBone"), RTS_Component);
                        Pose = Saddle * LocalMount * Grounded;
                    }
                }
                Pose.SetScale3D(Pose.GetScale3D() * Papyrus->VisualScale(Id));
                const auto Delta = Mechanics ? Mechanics->DoorDelta(Id, Visual.MotionNode, Authored, Pose) : Authored.Inverse() * Pose;
                Component->SetRelativeTransform(Visual.Original * Delta);
            }
        }
    }
}

bool AT5Scene::PlayLocomotion(USkeletalMeshComponent& Mesh, const FString& Project, const FString& Gait,
    float PlaybackSpeed, TOptional<float> ResumeAt, FString& Error)
{
    if (Project.IsEmpty())
        return false;
    const auto Read = [this](const FString& Path, TArray<uint8>& Bytes) { return Papyrus->ReadResource(Path, Bytes); };
    const bool bRider = Gait.StartsWith(TEXT("Rider"));
    const FString Key = Project + (bRider ? TEXT("|rider") : TEXT(""));
    const FString Movement = bRider ? Gait.Mid(5) : Gait;
    auto* Set = Locomotion.Find(Key);
    if (!Set)
    {
        Set = &Locomotion.Add(Key);
        TArray<FString> Clips;
        if (!LoadT5HkxCharacterProject(Project, Read, Set->Rig, Clips))
            return false;
        const auto Find = [&](const TCHAR* Name)
        {
            const auto FindFile = [&](const FString& File) {
                return Clips.FindByPredicate([&](const auto& Path) {
                    return FPaths::GetBaseFilename(Path).Equals(File, ESearchCase::IgnoreCase)
                        && Path.Contains(TEXT("/horse_rider/"), ESearchCase::IgnoreCase) == bRider;
                });
            };
            if (const auto* Human = FindFile(FString(TEXT("MT_")) + Name))
                return *Human;
            if (const auto* Clip = FindFile(Name))
                return *Clip;
            return FString{};
        };
        Set->Idle = Find(TEXT("Idle"));
        Set->Walk = Find(TEXT("WalkForward"));
        Set->Jog = Find(TEXT("TrotForward"));
        Set->Run = Find(TEXT("RunForward"));
    }
    const bool bTrot = Movement == TEXT("T5Jog") && !Set->Jog.IsEmpty();
    const bool bRun = (Movement == TEXT("T5Run") || Movement == TEXT("T5Jog")) && !Set->Run.IsEmpty();
    const auto& File = bTrot ? Set->Jog : bRun ? Set->Run
        : Movement != TEXT("T5Idle") && !Set->Walk.IsEmpty() ? Set->Walk : Set->Idle;
    if (File.IsEmpty())
        return false;
    const float Rate = Movement == TEXT("T5FastWalk") || (bRun && !bTrot) ? PlaybackSpeed : 1.f;
    if (PlayT5Hkx(Mesh, {}, {}, File, Read, Error, Set->Rig, {}, Rate, false, true, ResumeAt))
        return true;
    const auto ReadBase = [this](const FString& Path, TArray<uint8>& Bytes) {
        return Papyrus->ReadResource(TEXT("@base-animation/") + Path, Bytes);
    };
    FString FallbackError;
    if (!PlayT5Hkx(Mesh, {}, {}, File, ReadBase, FallbackError, Set->Rig, {}, Rate, false, true, ResumeAt))
        return false;
    UE_LOG(LogT5Data, Display, TEXT("T5_HKX_BASE_FALLBACK %s (%s)"), *File, *Error);
    Error.Reset();
    return true;
}

void AT5Scene::RequestScene(const FT5LoadRequest& InRequest, bool bRecenter)
{
    FT5LoadRequest Request = InRequest;
    Request.bPlayable = Request.FormId != 0 && GetWorld() && GetWorld()->IsGameWorld();
    Request.OpeningQuest = OpeningQuest;
    if (Request.bPlayable && Papyrus)
        Papyrus->Capture(Request);
    if (!FApp::CanEverRender() && FPlatformProperties::RequiresCookedData())
    {
        LoadStatus = TEXT("A rendering device is required to load T5 geometry in a packaged build.");
        UE_LOG(LogT5Data, Warning, TEXT("%s"), *LoadStatus);
        return;
    }
    FString Data, Plugins, Bridge;
    if (!ResolvePaths(Data, Plugins, Bridge))
        return;
    PendingBuild.Reset();
    Request.Progress = LoadProgress = MakeShared<FT5LoadProgress>();
    bFullLoadingScreen = bStartupPreparing || bRecenter || !LoadedScene;
    if (bFullLoadingScreen) CloseDialogue();
    bLoadingFailed = false;
    ShowLoadingScreen();
    bSceneLoading = true;
    LoadStatus = FString::Printf(TEXT("Loading 0x%08X at %d, %d..."), Request.FormId, Request.Cell.X, Request.Cell.Y);
    if (!bRecenter)
        RequestedCell = Request.Cell;
    const uint64 Generation = ++LoadGeneration;
    UE_LOG(LogT5Data, Display, TEXT("T5_LOAD id=%08X cell=%d,%d radius=%d recenter=%d"),
        Request.FormId, Request.Cell.X, Request.Cell.Y, Request.Radius, bRecenter);
    Async(EAsyncExecution::ThreadPool, [Weak = TWeakObjectPtr<AT5Scene>(this), Generation, Data, Plugins, Bridge, Request, bRecenter]
    {
        auto Result = LoadT5Scene(Data, Plugins, Bridge, Request);
        for (const TCHAR* Name : {TEXT("meshes.json"), TEXT("location.json"), TEXT("collision.json"), TEXT("mechanics.json")})
            Result->PreparedJson.Add(Name, ReadJson(*Result, Name));
        AsyncTask(ENamedThreads::GameThread, [Weak, Generation, Request, bRecenter, Result = MoveTemp(Result)]() mutable
        {
            if (auto* Scene = Weak.Get(); Scene && Scene->LoadGeneration == Generation)
            {
                Scene->BuildStep = TEXT("begin");
                Scene->BuildStarted = FPlatformTime::Seconds();
                Scene->BuildMaxStepMs = 0;
                Scene->PendingBuild = MakeShared<FT5FrameTask>(Scene->BuildScene(MoveTemp(Result), Request, bRecenter));
            }
        });
    });
}

void AT5Scene::TickSceneBuild()
{
    const double RetireStart = FPlatformTime::Seconds();
    while (!RetiredScenes.IsEmpty() && FPlatformTime::Seconds() - RetireStart < .001)
    {
        auto* Actor = RetiredScenes.Last().Get();
        if (!Actor) { RetiredScenes.Pop(); continue; }
        TInlineComponentArray<UActorComponent*> Components(Actor);
        if (Components.Num() <= 1) { Actor->Destroy(); RetiredScenes.Pop(); }
        else Components.Last()->DestroyComponent();
    }
    if (!PendingBuild)
        return;
    const double Start = FPlatformTime::Seconds();
    do
    {
        const double Before = FPlatformTime::Seconds();
        const FString Step = BuildStep;
        PendingBuild->Resume();
        if (FPlatformTime::Seconds() - Before > .025)
            UE_LOG(LogT5Data, Display, TEXT("T5_BUILD_STEP ms=%.2f stage=%s"), (FPlatformTime::Seconds() - Before) * 1000., *Step);
        BuildMaxStepMs = FMath::Max(BuildMaxStepMs, (FPlatformTime::Seconds() - Before) * 1000.);
    } while (!PendingBuild->Done() && FPlatformTime::Seconds() - Start < .004);
    if (PendingBuild->Done())
    {
        const bool bSuccess = PendingBuild->Succeeded();
        UE_LOG(LogT5Data, Display, TEXT("T5_BUILD_FINISHED success=%d seconds=%.3f maxStepMs=%.2f"),
            bSuccess, FPlatformTime::Seconds() - BuildStarted, BuildMaxStepMs);
        PendingBuild.Reset();
        bSceneLoading = false;
        bLoadingFailed = !bSuccess;
        LoadStatus = bSuccess ? FString{} : TEXT("T5 scene build failed; previous scene retained. See log.");
    }
}

FT5FrameTask AT5Scene::BuildScene(TUniquePtr<FT5LoadResult> Data, FT5LoadRequest Request, bool bRecenter)
{
    const auto& Result = *Data;
    if (!Result.Error.IsEmpty())
    {
        UE_LOG(LogT5Data, Error, TEXT("T5 Data load failed: %s"), *Result.Error);
        co_return false;
    }
    const auto Geometry = Result.Read(TEXT("meshes.bin"));
    const auto Manifest = ReadJson(Result, TEXT("meshes.json"));
    const auto Metadata = ReadJson(Result, TEXT("location.json"));
    const auto Collisions = ReadJson(Result, TEXT("collision.json"));
    const bool bInterior = Metadata && Metadata->GetIntegerField(TEXT("interior")) != 0;
    if (!Manifest || Manifest->GetIntegerField(TEXT("version")) != 2 || !Collisions
        || (Request.FormId && !Metadata) || !GetWorld())
    {
        UE_LOG(LogT5Data, Error, TEXT("Invalid T5 Data scene manifest"));
        co_return false;
    }

    FActorSpawnParameters Spawn;
    Spawn.ObjectFlags = RF_Transient | RF_DuplicateTransient;
    AActor* NewScene = GetWorld()->SpawnActor<AActor>(AActor::StaticClass(), GetActorTransform(), Spawn);
    if (!NewScene)
        co_return false;
    struct FPendingSceneGuard
    {
        AActor* Actor;
        ~FPendingSceneGuard() { if (IsValid(Actor)) Actor->Destroy(); }
    } Guard{NewScene};
    NewScene->SetActorHiddenInGame(true);
    NewScene->SetActorEnableCollision(false);
    auto* Root = NewObject<USceneComponent>(NewScene, NAME_None, RF_Transient | RF_DuplicateTransient);
    Root->SetRelativeTransform(GetActorTransform());
    Root->SetMobility(EComponentMobility::Static);
    NewScene->SetRootComponent(Root);
    Root->RegisterComponent();
    auto NewMechanics = MakeShared<FT5Mechanics>();
    { FString DataPath,Plugins,Bridge; if (ResolvePaths(DataPath,Plugins,Bridge)) NewMechanics->Configure(DataPath,Plugins); }
    if (Request.bPlayable)
        if (const auto Models = ReadJson(Result, TEXT("mechanics.json")))
            NewMechanics->Load(*NewScene, *Models);
    NewMechanics->SetActive(false);
    BuildStep = TEXT("materials");
    co_yield 0;
    for (auto It = RecentTextures.CreateIterator(); It; ++It)
        if (!It.Value().IsValid())
            It.RemoveCurrent();
    FMaterialFactory Factory{this, Result, RecentTextures};
    TMap<FString, TUniquePtr<FMeshGroup>> Groups;
    TMap<FString, TSharedPtr<FT5ArcheryAsset>> NewArcheryAssets;
    TMap<uint32, TArray<FReferenceVisual>> NewReferences;
    TMap<TWeakObjectPtr<UPrimitiveComponent>, uint32> NewInteraction;
    const auto AddReference = [&](uint32 Id, USceneComponent* Component, FString SkeletonPath = {},
                                  FString MotionNode = {}, bool bColliderOnly = false) {
        if (!Id)
            return;
        auto* Primitive = Cast<UPrimitiveComponent>(Component);
        NewReferences.FindOrAdd(Id).Add({Component, Primitive ? Primitive->GetCollisionEnabled() : ECollisionEnabled::NoCollision,
            Component->GetRelativeTransform(), MoveTemp(SkeletonPath), MoveTemp(MotionNode), bColliderOnly});
        if (Primitive)
            NewInteraction.Add(Primitive, Id);
    };
    struct FRigGroup
    {
        FString Rig;
        TArray<FT5SkinSection> Sections;
        uint32 Reference = 0;
        FString ArcheryPath, AnimationProject;
    };
    TMap<FString, FRigGroup> Skinned;
    TSet<FString> Actors;
    const auto& Sections = Manifest->GetArrayField(TEXT("sections"));
    int32 Count = 0;
    int32 LodSections = 0;
    uint64 SectionDone = 0;
    TMap<FString, uint64> TextureHashes;
    for (const auto& Value : Sections)
    {
        LoadProgress->Set(TEXT("Подготовка секций и материалов"), Value->AsObject()->GetStringField(TEXT("object")), SectionDone++, Sections.Num());
        BuildStep = TEXT("section:") + Value->AsObject()->GetStringField(TEXT("object"));
        co_yield 0;
        const auto Section = Value->AsObject();
        const FString Group = Section->GetStringField(TEXT("group"));
        const bool bNPC = Group == TEXT("NPC");
        const FString Object = Section->GetStringField(TEXT("object"));
        double ReferenceNumber = 0;
        Section->TryGetNumberField(TEXT("reference"), ReferenceNumber);
        const uint32 Reference = static_cast<uint32>(ReferenceNumber);
        if (bNPC)
            Actors.Add(Object);
        FString Rig;
        if (Section->TryGetStringField(TEXT("rig"), Rig))
        {
            auto& Batch = Skinned.FindOrAdd(Object + TEXT("|") + Rig);
            Batch.Rig = MoveTemp(Rig);
            Batch.Reference = Reference;
            Section->TryGetStringField(TEXT("animationProject"), Batch.AnimationProject);
            if (Group == TEXT("Archery"))
            {
                Batch.ArcheryPath = Object;
            }
            Batch.Sections.Add({Section, Factory.Surface(Section->GetObjectField(TEXT("material")))});
            ++Count;
            continue;
        }
        const bool bLOD = Group == TEXT("LOD");
        LodSections += bLOD;
        FString MotionNode;
        Section->TryGetStringField(TEXT("motionNode"), MotionNode);
        auto& Batch = Groups.FindOrAdd(Group == TEXT("Archery") ? Object
            : Reference ? TEXT("REFR_") + LexToString(Reference) + TEXT("|") + MotionNode : bNPC ? Object : Group + TEXT("|") + Object);
        if (!Batch)
        {
            Batch = MakeUnique<FMeshGroup>(Group == TEXT("Terrain"));
            Batch->bArchery = Group == TEXT("Archery");
            Batch->Reference = Reference;
            Batch->MotionNode = MoveTemp(MotionNode);
        }
        if (!QueueSection(*Batch, Section, Geometry, Result, TextureHashes))
        {
            NewScene->Destroy();
            UE_LOG(LogT5Data, Error, TEXT("Cannot build T5 mesh section %d"), Count);
            co_return false;
        }
        ++Count;
    }

    // The visible scene already owns all of its meshes. Reuse those even when
    // their total size exceeds the separate budget for retaining unused meshes.
    TMap<FName, UStaticMesh*> ActiveStaticMeshes;
    if (IsValid(LoadedScene) && StaticMeshCacheMB > 0)
    {
        TInlineComponentArray<UStaticMeshComponent*> Components(LoadedScene);
        for (auto* Component : Components)
            for (const FName Tag : Component->ComponentTags)
                if (Tag.ToString().StartsWith(TEXT("T5Cache:")))
                    ActiveStaticMeshes.Add(Tag, Component->GetStaticMesh());
    }
    int32 MeshCacheHits = 0, MeshCacheMisses = 0;
    uint64 MeshDone = 0;
    for (auto& Pair : Groups)
    {
        LoadProgress->Set(TEXT("Создание статических мешей"), Pair.Key, MeshDone++, Groups.Num());
        BuildStep = TEXT("mesh:") + Pair.Key;
        co_yield 0;
        auto& Group = *Pair.Value;
        const auto SourceRevision = Result.Read(TEXT("resource-revision"));
        const uint64 RevisionHash = CityHash64(reinterpret_cast<const char*>(SourceRevision.GetData()), SourceRevision.Num());
        const FString CacheKey = FString::Printf(TEXT("%016llX-%016llX-%d"), RevisionHash, Group.ContentHash, Group.bCollision);
        const FName CacheTag(*(TEXT("T5Cache:") + CacheKey));
        UStaticMesh* Mesh = nullptr;
        if (StaticMeshCacheMB > 0 && !SourceRevision.IsEmpty())
        {
            if (const auto* Cached = CachedStaticMeshes.Find(CacheKey)) Mesh = Cached->Get();
            if (!Mesh)
                if (auto* const* Active = ActiveStaticMeshes.Find(CacheTag)) Mesh = *Active;
        }
        if (Mesh)
        {
            if (CachedStaticMeshes.Contains(CacheKey))
            {
                StaticMeshCacheOrder.Remove(CacheKey);
                StaticMeshCacheOrder.Add(CacheKey);
            }
            ++MeshCacheHits;
        }
        else
        {
            for (const auto& Section : Group.Sections)
            {
                auto SectionTask = AddSection(Group, Section, Geometry, Factory);
                while (!SectionTask.Done()) { SectionTask.Resume(); co_yield 0; }
                if (!SectionTask.Succeeded())
                {
                    UE_LOG(LogT5Data, Error, TEXT("Cannot expand T5 mesh group %s"), *Pair.Key);
                    co_return false;
                }
            }
            // The shared description remains owned by the coroutine until the worker ends.
            // Cancellation must not invalidate worker data, so transfer ownership temporarily.
            auto Description = MakeShared<FMeshDescription>(MoveTemp(Group.Description));
            auto Tangents = Async(EAsyncExecution::ThreadPool, [Description] {
                FStaticMeshOperations::ComputeTriangleTangentsAndNormals(*Description);
                FStaticMeshOperations::ComputeTangentsAndNormals(*Description, EComputeNTBsFlags::Tangents | EComputeNTBsFlags::UseMikkTSpace);
            });
            while (!Tangents.IsReady())
                co_yield 0;
            Tangents.Get();
            Group.Description = MoveTemp(*Description);
            Mesh = NewObject<UStaticMesh>(this, NAME_None, RF_Transient | RF_DuplicateTransient);
            Mesh->bAllowCPUAccess = Group.bCollision;
            Mesh->SetStaticMaterials(Group.Materials);
            UStaticMesh::FBuildMeshDescriptionsParams Build;
            Build.bFastBuild = true;
            Build.bCommitMeshDescription = false;
            Build.bMarkPackageDirty = false;
            Build.bAllowCpuAccess = Group.bCollision;
            if (!Mesh->BuildFromMeshDescriptions({&Group.Description}, Build))
            {
                NewScene->Destroy();
                UE_LOG(LogT5Data, Error, TEXT("Cannot build T5 mesh group %s"), *Pair.Key);
                co_return false;
            }
            ++MeshCacheMisses;
            const uint64 Bytes = Mesh->GetResourceSizeBytes(EResourceSizeMode::EstimatedTotal);
            const uint64 Budget = uint64(FMath::Clamp(StaticMeshCacheMB, 0, 2048)) * 1024 * 1024;
            if (Bytes <= Budget && Budget > 0 && !SourceRevision.IsEmpty())
            {
                while (StaticMeshCacheBytes + Bytes > Budget && !StaticMeshCacheOrder.IsEmpty())
                {
                    const FString OldKey = StaticMeshCacheOrder[0];
                    StaticMeshCacheOrder.RemoveAt(0);
                    if (const auto* Old = CachedStaticMeshes.Find(OldKey))
                        StaticMeshCacheBytes -= FMath::Min(StaticMeshCacheBytes,
                            uint64((*Old)->GetResourceSizeBytes(EResourceSizeMode::EstimatedTotal)));
                    CachedStaticMeshes.Remove(OldKey);
                }
                CachedStaticMeshes.Add(CacheKey, Mesh);
                StaticMeshCacheOrder.Add(CacheKey); StaticMeshCacheBytes += Bytes;
            }
        }
        auto* Component = NewObject<UStaticMeshComponent>(NewScene, NAME_None, RF_Transient | RF_DuplicateTransient);
        Component->SetupAttachment(Root);
        Component->SetStaticMesh(Mesh);
        Component->bCastShadowAsTwoSided = bInterior;
        if (!SourceRevision.IsEmpty()) Component->ComponentTags.Add(CacheTag);
        if (Pair.Key.StartsWith(TEXT("LOD|")))
        {
            Component->SetCastShadow(false);
            Component->SetVisibleInRayTracing(false);
        }
        if (Group.bArchery)
        {
            Component->SetCollisionEnabled(ECollisionEnabled::NoCollision);
            Component->SetVisibility(false);
            Component->RegisterComponent();
            NewArcheryAssets.Add(Pair.Key, MakeShared<FT5ArcheryAsset>(FT5ArcheryAsset{Mesh, {}}));
            continue;
        }
        if (Group.bCollision)
        {
            Component->ComponentTags.Add(TEXT("T5Terrain"));
            Mesh->GetBodySetup()->CollisionTraceFlag = CTF_UseComplexAsSimple;
            Component->SetCollisionProfileName(TEXT("BlockAll"));
        }
        else
        {
            Component->SetCollisionEnabled(ECollisionEnabled::NoCollision);
        }
        Component->RegisterComponent();
        AddReference(Group.Reference, Component, {}, Group.MotionNode);
    }
    LastMeshCacheHits = MeshCacheHits;
    UE_LOG(LogT5Data, Display, TEXT("T5_MESH_CACHE hits=%d misses=%d retained=%d meshMB=%.1f"),
        MeshCacheHits, MeshCacheMisses, CachedStaticMeshes.Num(), double(StaticMeshCacheBytes) / (1024 * 1024));
    TMultiMap<uint32, USkeletalMeshComponent*> Skeletons;
    if (!Skinned.IsEmpty())
    {
        const auto& Rigs = Manifest->GetObjectField(TEXT("rigs"));
        const auto Skin = Result.Read(TEXT("skins.bin"));
        uint64 SkinDone = 0;
        for (const auto& Pair : Skinned)
        {
            LoadProgress->Set(TEXT("Создание скелетных мешей и лицевых морфов"), Pair.Key, SkinDone++, Skinned.Num());
            BuildStep = TEXT("skin:") + Pair.Key;
            co_yield 0;
            const auto& Batch = Pair.Value;
            const auto& Joints = Rigs->GetArrayField(Batch.Rig);
            auto* Component = BuildT5SkeletalMesh(*NewScene, *Root, Joints, Batch.Sections, Geometry, Skin, Result.Read(TEXT("morphs.bin")));
            if (!Component)
            {
                NewScene->Destroy();
                UE_LOG(LogT5Data, Error, TEXT("Cannot build T5 skeletal mesh %s"), *Pair.Key);
                co_return false;
            }
            if (!Batch.ArcheryPath.IsEmpty())
            {
                Component->SetVisibility(false);
                NewArcheryAssets.Add(Batch.ArcheryPath, MakeShared<FT5ArcheryAsset>(
                    FT5ArcheryAsset{Component->GetSkeletalMeshAsset(), Batch.AnimationProject}));
                continue;
            }
            AddReference(Batch.Reference, Component, FPaths::ChangeExtension(Batch.Rig, TEXT("hkx")));
            if (Batch.Reference)
            {
                Skeletons.Add(Batch.Reference, Component);
                auto& Added = NewReferences.FindChecked(Batch.Reference).Last();
                Added.AnimationProject = Batch.AnimationProject;

            }
        }
    }
    // Group instances by model, preserving collision sharing without building
    // the entire region in one frame.
    TMap<FString, TArray<TSharedPtr<FJsonValue>>> CollisionInstances;
    for (const auto& Value : Collisions->GetArrayField(TEXT("instances")))
        CollisionInstances.FindOrAdd(Value->AsObject()->GetStringField(TEXT("model"))).Add(Value);
    uint64 CollisionDone = 0;
    for (const auto& [Path, Model] : Collisions->GetObjectField(TEXT("models"))->Values)
    {
        LoadProgress->Set(TEXT("Создание коллизий из NIF"), FString(Path), CollisionDone++, Collisions->GetObjectField(TEXT("models"))->Values.Num());
        BuildStep = TEXT("collision:") + FString(Path);
        co_yield 0;
        auto Part = MakeShared<FJsonObject>();
        auto Models = MakeShared<FJsonObject>();
        Models->SetField(Path, Model);
        Part->SetObjectField(TEXT("models"), Models);
        const TSharedPtr<FJsonObject>* Ragdolls = nullptr;
        if (Collisions->TryGetObjectField(TEXT("ragdolls"), Ragdolls) && (*Ragdolls)->HasField(Path))
        {
            auto Rig = MakeShared<FJsonObject>();
            Rig->SetField(Path, (*Ragdolls)->TryGetField(Path));
            Part->SetObjectField(TEXT("ragdolls"), Rig);
        }
        Part->SetArrayField(TEXT("instances"), CollisionInstances.FindRef(FString(Path)));
        NewMechanics->LoadCollisions(*NewScene, *Part, Skeletons,
            [&](uint32 Id, UStaticMeshComponent* Component, const FString& Node) { AddReference(Id, Component, {}, Node, true); });
    }
    LoadProgress->Set(TEXT("Подключение локации, скриптов и анимаций"), FString{}, 0, 1);
    BuildStep = TEXT("commit");
    co_yield 0;
    if (IsValid(LoadedScene))
    {
        if (!bRestoringCheckpoint) CaptureRagdollPoses();
        LoadedScene->SetActorHiddenInGame(true);
        LoadedScene->SetActorEnableCollision(false);
        RetiredScenes.Add(LoadedScene);
    }
    for (auto& [Id, Parts] : NewReferences)
        if (const auto* Previous = ReferenceVisuals.Find(Id))
            for (auto& Added : Parts)
                for (const auto& Visual : *Previous)
                    if (Visual.SkeletonPath == Added.SkeletonPath)
                        if (auto* Old = Cast<USkeletalMeshComponent>(Visual.Component.Get()))
                            if (const auto* Anim = Cast<UT5HkxAnimInstance>(Old->GetAnimInstance()))
                            {
                                Added.ResumeSerial = Visual.AnimationSerial;
                                Added.ResumeAt = Anim->PlaybackPosition();
                                break;
                            }
    LoadedScene = NewScene;
    Guard.Actor = nullptr;
    NewScene->SetActorHiddenInGame(false);
    NewScene->SetActorEnableCollision(true);
    NewMechanics->SetActive(true);
    ArcheryAssets = MoveTemp(NewArcheryAssets);
    if (!bRecenter && Mechanics)
        NewMechanics->ContinueMotionFrom(*Mechanics);
    Mechanics = MoveTemp(NewMechanics);
    const bool bGame = GetWorld()->IsGameWorld();
    ApplyEnvironment(NewScene, Root, Metadata, bInterior);
    FVector Start = GetActorLocation();
    if (Metadata)
    {
        Start = GetActorTransform().TransformPosition(T5Position(Metadata->GetArrayField(TEXT("spawn"))));
        const TSharedPtr<FJsonObject>* Environment = nullptr;
        Metadata->TryGetObjectField(TEXT("environment"), Environment);
        double FadeBegin = 0, FadeEnd = 0;
        if (Environment)
        {
            (*Environment)->TryGetNumberField(TEXT("lightFadeBegin"), FadeBegin);
            (*Environment)->TryGetNumberField(TEXT("lightFadeEnd"), FadeEnd);
        }
        for (const auto& Value : Metadata->GetArrayField(TEXT("lights")))
        {
            const auto& Light = Value->AsArray();
            auto* Component = NewObject<UPointLightComponent>(NewScene, NAME_None, RF_Transient | RF_DuplicateTransient);
            Component->SetupAttachment(Root);
            Component->SetRelativeLocation(T5Position(Light));
            Component->SetAttenuationRadius(Light[3]->AsNumber() * CentimetersPerT5Unit);
            Component->SetLightFColor(FColor(Light[4]->AsNumber(), Light[5]->AsNumber(), Light[6]->AsNumber()));
            Component->SetIntensityUnits(ELightUnits::Lumens);
            const double Fade = Light.IsValidIndex(7) ? Light[7]->AsNumber() : 1;
            const uint32 Flags = Light.IsValidIndex(8) ? static_cast<uint32>(Light[8]->AsNumber()) : 0;
            Component->SetIntensity(1500 * FMath::Clamp(Fade, 0.0, 8.0));
            constexpr uint32 ShadowLight = 0x1c00, PortalStrict = 0x2000;
            // Use the room geometry to contain portal-strict lights.
            Component->SetCastShadows((Flags & ShadowLight) != 0 || (bInterior && (Flags & PortalStrict)));
            if (FadeEnd > 0 && FadeEnd > FadeBegin)
            {
                Component->SetMaxDrawDistance(FadeEnd * CentimetersPerT5Unit);
                Component->SetMaxDistanceFadeRange((FadeEnd - FMath::Max(0., FadeBegin)) * CentimetersPerT5Unit);
            }
            Component->RegisterComponent();
            if (Light.IsValidIndex(9))
                AddReference(static_cast<uint32>(Light[9]->AsNumber()), Component);
        }
        if (bRecenter && bGame)
            if (auto* Controller = GetWorld()->GetFirstPlayerController())
            {
                const FRotator Rotation(0, GetActorRotation().Yaw - Metadata->GetNumberField(TEXT("yaw")), 0);
                const bool bGrounded = Request.bPlayable && !FParse::Param(FCommandLine::Get(), TEXT("T5FreeCamera"));
                APawn* Previous = Controller->GetPawn();
                APawn* Pawn = bGrounded ? static_cast<APawn*>(Cast<AT5PlayerPawn>(Previous))
                    : static_cast<APawn*>(Cast<ASpectatorPawn>(Controller->GetPawnOrSpectator()));
                if (!Pawn)
                {
                    FActorSpawnParameters PlayerSpawn = Spawn;
                    PlayerSpawn.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
                    Pawn = bGrounded ? static_cast<APawn*>(GetWorld()->SpawnActor<AT5PlayerPawn>(Start, Rotation, PlayerSpawn))
                        : static_cast<APawn*>(GetWorld()->SpawnActor<ASpectatorPawn>(Start, Rotation, PlayerSpawn));
                    if (Pawn)
                    {
                        Controller->Possess(Pawn);
                        if (Previous && Controller->GetPawn() == Pawn)
                            Previous->Destroy();
                    }
                }
                if (Pawn)
                {
                    if (auto* Character = Cast<AT5PlayerPawn>(Pawn))
                    {
                        Character->SuspendMovement(true);
                        Character->SetScriptedEye({});
                        Character->PlaceFeet(Start - FVector(0, 0, (Papyrus ? Papyrus->PlayerEyeHeight() : 164.f) * GetActorScale3D().Z));
                    }
                    else
                    {
                        if (auto* Movement = Pawn->GetMovementComponent())
                            Movement->StopMovementImmediately();
                        Pawn->SetActorLocationAndRotation(Start, Rotation, false, nullptr, ETeleportType::TeleportPhysics);
                    }
                    Controller->SetControlRotation(Rotation);
                    Controller->SetViewTarget(Pawn);
                }
            }
    }
    if (Request.bPlayable)
    {
        if (!Papyrus)
            Papyrus = MakeShared<FT5PapyrusRuntime>();
        ShowPapyrusErrors();
        const TArray<uint8>* Checkpoint = PendingNativeCheckpoint.IsEmpty() ? nullptr : &PendingNativeCheckpoint;
        if (Papyrus->Attach(Result, Checkpoint))
        {
            ReferenceVisuals = MoveTemp(NewReferences);
            InteractionComponents = MoveTemp(NewInteraction);
            LootableReferences.Reset();
            AuthoredCorpses.Reset();
            Ragdolled.Reset();
            const TSharedPtr<FJsonObject>* Corpses = nullptr;
            if (Metadata && Metadata->TryGetObjectField(TEXT("corpses"), Corpses))
                for (const auto& [Id, Pose] : (*Corpses)->Values)
                    AuthoredCorpses.Add(FCString::Strtoui64(*Id, nullptr, 10), Pose);
            const TArray<TSharedPtr<FJsonValue>>* Loot = nullptr;
            if (Metadata && Metadata->TryGetArrayField(TEXT("loot"), Loot))
                for (const auto& Id : *Loot)
                    LootableReferences.Add(uint32(Id->AsNumber()));
            Papyrus->SetLineOfSight([](void* Context, uint32 Source, uint32 Target) -> int
            {
                const auto& Scene = *static_cast<AT5Scene*>(Context);
                FCollisionQueryParams Query(SCENE_QUERY_STAT(T5PapyrusSight), true);
                const auto Position = [&](uint32 Id) -> TOptional<FVector>
                {
                    if (Id == 0x14)
                        if (const auto* PC = Scene.GetWorld()->GetFirstPlayerController())
                            if (const auto* Pawn = PC->GetPawnOrSpectator())
                            {
                                Query.AddIgnoredActor(Pawn);
                                return Pawn->GetPawnViewLocation();
                            }
                    if (const auto* Visuals = Scene.ReferenceVisuals.Find(Id))
                        for (const auto& Visual : *Visuals)
                            if (const auto* Component = Cast<UPrimitiveComponent>(Visual.Component.Get()))
                                Query.AddIgnoredComponent(Component);
                    static const FName Head(TEXT("NPC Head [Head]"));
                    if (const auto* Mesh = Scene.FindReferenceSkeleton(Id); Mesh && Mesh->GetBoneIndex(Head) != INDEX_NONE)
                        return Mesh->GetBoneLocation(Head);
                    FTransform Authored; uint32 Parent;
                    if (const auto Pose = Scene.Papyrus->ReferenceTransform(Id, Authored, Parent))
                        return Scene.GetActorTransform().TransformPosition(Pose->GetLocation());
                    return {};
                };
                const auto From = Position(Source), To = Position(Target);
                return From && To && !Scene.GetWorld()->LineTraceTestByChannel(*From, *To, ECC_Visibility, Query);
            }, this);
            if (Checkpoint) { RestoreCheckpointHost(); if (bLoadingFailed) { Papyrus.Reset(); bRestoringCheckpoint = false; co_return false; } }
            ApplyPapyrusState();
            ApplyPapyrusWorld();
        }
        else
        {
            ReferenceVisuals.Reset();
            InteractionComponents.Reset();
            UE_LOG(LogT5Data, Error, TEXT("T5 Papyrus could not attach the loaded location"));
            if (bRestoringCheckpoint) { bLoadingFailed = true; Papyrus.Reset(); bRestoringCheckpoint = false; SaveMessage(TEXT("Сохранение несовместимо с данными игры. См. T5Papyrus.log"), true); co_return false; }
        }
    }
    else
    {
        ReferenceVisuals.Reset();
        InteractionComponents.Reset();
    }
    ActiveWorld = bInterior ? 0 : Request.FormId;
    ActiveLocation = Request.FormId;
    LoadedSpawnCount = Request.SpawnedReferences.Num();
    LoadedInteriorArrivals.Reset();
    if (bInterior)
        for (const auto& Ref : Request.SpawnedReferences)
            if ((Ref.Flags & 4) && Ref.Cell == ActiveLocation)
                LoadedInteriorArrivals.Add(Ref.Id);
    RequestedCell = Request.Cell;
    const FString WorldMode = bGame ? TEXT("game") : TEXT("editor");
    FHitResult Ground;
    const bool bGround = GetWorld()->LineTraceSingleByChannel(Ground, Start + FVector(0, 0, 3000), Start - FVector(0, 0, 3000), ECC_Visibility);
    UE_LOG(LogT5Data, Display, TEXT("T5_DATA_READY mode=%s id=%08X cell=%d,%d interior=%d lod=%d groups=%d sections=%d npcs=%d skeletal=%d textures=%d unsupported=%d ground=%d"),
        *WorldMode, Request.FormId, Request.Cell.X, Request.Cell.Y, bInterior, LodSections, Groups.Num(), Count, Actors.Num(),
        Skinned.Num(), Factory.Textures.Num(), Factory.MissingTextures, bGround);
    // Releasing the native bundle and mesh descriptions can free hundreds of MB.
    // None of these buffers own UObjects: do that work outside the game thread.
    Async(EAsyncExecution::ThreadPool, [Buffers = MoveTemp(Data), Descriptions = MoveTemp(Groups)]() mutable
    { Descriptions.Reset(); Buffers.Reset(); });
    co_return true;
}

void AT5Scene::OpenRaceMenu()
{
    if (CharacterCreator || !Papyrus || bSceneLoading || bRestoringCheckpoint || bStartupPreparing) return;
    CloseDialogue();
    CloseMapBrowser();
    uint32 Controls, Flags; float Rotation; Papyrus->GameState(Controls, Flags, Rotation);
    auto Creator = MakeShared<FT5CharacterCreator>();
    if (Creator->Open(*this, (Flags & FT5PapyrusRuntime::RaceMenuPending) != 0))
    { CharacterCreator = MoveTemp(Creator); bAutoRaceMenuUsed = true; }
}
void AT5Scene::TickCharacterCreation()
{
    if (CharacterCreator && CharacterCreator->IsClosed()) CharacterCreator.Reset();
    // Existing cart/checkpoint probes inspect the original camera at this boundary.
    // The dedicated RaceMenu regression exercises the interactive host separately.
    if (FParse::Param(FCommandLine::Get(), TEXT("T5NoRaceMenu"))
        || (!FParse::Param(FCommandLine::Get(), TEXT("T5RaceMenuSmoke"))
            && (FParse::Param(FCommandLine::Get(), TEXT("T5MQ101RaceMenuSmoke"))
                || FParse::Param(FCommandLine::Get(), TEXT("T5CheckpointSmoke"))))) return;
    if (!Papyrus || CharacterCreator || bSceneLoading || bRestoringCheckpoint || bStartupPreparing) return;
    uint32 Controls, Flags; float Rotation; Papyrus->GameState(Controls, Flags, Rotation);
    if (Flags & FT5PapyrusRuntime::RaceMenuPending) OpenRaceMenu();
    else if (!bAutoRaceMenuUsed && FParse::Param(FCommandLine::Get(), TEXT("T5RaceMenu")))
    { bAutoRaceMenuUsed = true; OpenRaceMenu(); }
}
bool AT5Scene::CanChangePlayerPerspective() const
{
    const auto* PC = GetWorld()->GetFirstPlayerController();
    return PC && Cast<AT5PlayerPawn>(PC->GetPawn()) && PC->GetViewTarget() == PC->GetPawn()
        && Papyrus && !(AppliedControls & (1u << 2)) && !LastPlayerVehicle
        && !HasExecutionCamera(AppliedControls, AppliedGameFlags) && !bRestoringCheckpoint
        && !(bSceneLoading && bFullLoadingScreen) && !bStartupPreparing
        && !MapBrowser && !SaveBrowser && !InventoryMenu && !DialogueMenu && !CharacterCreator;
}

void AT5Scene::TogglePlayerPerspective()
{
    if (CanChangePlayerPerspective())
        PlayerCameraDistance = PlayerCameraDistance > 0 ? 0.f : LastThirdPersonDistance;
}

void AT5Scene::ZoomPlayerIn()
{
    if (CanChangePlayerPerspective() && PlayerCameraDistance > 0)
    {
        PlayerCameraDistance = PlayerCameraDistance <= 120.f ? 0.f : FMath::Max(120.f, PlayerCameraDistance - 48.f);
        if (PlayerCameraDistance > 0) LastThirdPersonDistance = PlayerCameraDistance;
    }
}

void AT5Scene::ZoomPlayerOut()
{
    if (CanChangePlayerPerspective())
        LastThirdPersonDistance = PlayerCameraDistance = PlayerCameraDistance <= 0
            ? 120.f : FMath::Min(480.f, PlayerCameraDistance + 48.f);
}

void AT5Scene::DestroyPlayerBody()
{
    if (auto* Body = PlayerBody.Get()) Body->Destroy();
    PlayerBody.Reset();
    PlayerBodyKey = 0;
    PlayerBodyGait.Reset();
    PlayerBodyProject.Reset();
}

void AT5Scene::TickPlayerBody()
{
    const auto* PC = GetWorld()->GetFirstPlayerController();
    auto* Player = PC ? Cast<AT5PlayerPawn>(PC->GetPawn()) : nullptr;
    if (Player)
        Player->SetCameraDistance(PlayerCameraDistance);
    if (!Player || PC->GetViewTarget() != Player || !Player->IsThirdPerson() || bStartupPreparing
        || bRestoringCheckpoint || (bSceneLoading && bFullLoadingScreen))
    {
        if (auto* Body = PlayerBody.Get()) Body->SetActorHiddenInGame(true);
        return;
    }
    bool bDrawn = false;
    const auto Key = Papyrus->PlayerViewKey(bDrawn);
    if (Key && Key != PlayerBodyKey && !bSceneLoading)
    {
        DestroyPlayerBody();
        PlayerBodyKey = Key;
        const auto Data = Papyrus->PlayerBody();
        FString Error = Data ? Data->Error : TEXT("Bridge unavailable");
        if (Data && Error.IsEmpty())
            PlayerBody = BuildCharacterPreview(*Data, Error, false);
        if (auto* Body = PlayerBody.Get())
        {
            const auto Model = ReadJson(*Data, TEXT("body.json"));
            PlayerBodyScale = Model ? Model->GetNumberField(TEXT("scale")) : 1.f;
            UE_LOG(LogT5Data, Display, TEXT("T5_PLAYER_BODY ready scale=%.3f"), PlayerBodyScale);
        }
        else
            UE_LOG(LogT5Data, Warning, TEXT("T5_PLAYER_BODY_ERROR %s"), *Error);
    }
    auto* Body = PlayerBody.Get();
    if (!Body) return;
    const auto Velocity = Player->GetVelocity();
    const auto Speed = Velocity.Size2D();
    const bool bMoving = Speed > 12.f;
    const float Yaw = !bDrawn && bMoving ? Velocity.Rotation().Yaw : PC->GetControlRotation().Yaw;
    Body->SetActorTransform(FTransform(FRotator(0, Yaw, 0), Player->Feet(), GetActorScale3D() * PlayerBodyScale));
    Body->SetActorHiddenInGame(false);
    const FString Gait = !bMoving ? TEXT("T5Idle") : Speed < 180.f ? TEXT("T5Walk") : TEXT("T5Run");
    if (PlayerBodyGait != Gait)
    {
        PlayerBodyGait = Gait;
        TInlineComponentArray<USkeletalMeshComponent*> Meshes(Body);
        for (auto* Mesh : Meshes)
        {
            FString Error;
            if (!PlayLocomotion(*Mesh, PlayerBodyProject, Gait, 1.f, {}, Error) && !Error.IsEmpty())
                UE_LOG(LogT5Data, Warning, TEXT("T5_PLAYER_BODY_ANIMATION %s"), *Error);
        }
    }
}

AActor* AT5Scene::BuildCharacterPreview(const FT5LoadResult& Data, FString& Error, bool bPreview)
{
    auto Manifest = ReadJson(Data, TEXT("meshes.json"));
    if (!Manifest) { Error = TEXT("Нет геометрии персонажа"); return nullptr; }
    FActorSpawnParameters Spawn; Spawn.ObjectFlags = RF_Transient;
    auto* Actor = GetWorld()->SpawnActor<AActor>(GetActorLocation(), FRotator::ZeroRotator, Spawn);
    if (!Actor) return nullptr;
    auto* Root = NewObject<USceneComponent>(Actor); Actor->SetRootComponent(Root); Root->RegisterComponent();
    Actor->SetActorHiddenInGame(true);
    Actor->Tags.Add(bPreview ? TEXT("T5PlayerCustomization") : TEXT("T5PlayerBody"));
    FMaterialFactory Factory{Actor, Data, RecentTextures};
    TMap<FString, TArray<FT5SkinSection>> Groups;
    const auto& Rigs = Manifest->GetObjectField(TEXT("rigs"));
    const auto Morphs = bPreview ? nullptr : ReadJson(Data, TEXT("morphs.json"));
    for (const auto& Value : Manifest->GetArrayField(TEXT("sections")))
    {
        auto Section = Value->AsObject(); FString Rig;
        if (Section->GetStringField(TEXT("group")) == TEXT("NPC") && Section->TryGetStringField(TEXT("rig"), Rig))
            Groups.FindOrAdd(Rig).Add({Section, Factory.Surface(Section->GetObjectField(TEXT("material")))});
    }
    int32 Count = 0;
    for (const auto& [Rig, Sections] : Groups)
        if (auto* Component = BuildT5SkeletalMesh(*Actor, *Root, Rigs->GetArrayField(Rig), Sections,
            Data.Read(TEXT("meshes.bin")), Data.Read(TEXT("skins.bin")), Data.Read(TEXT("morphs.bin"))))
        {
            Component->SetCastShadow(true); ++Count;
            Component->SetLightingChannels(true, false, false);
            if (!bPreview)
            {
                Sections[0].Data->TryGetStringField(TEXT("animationProject"), PlayerBodyProject);
                if (Morphs)
                    for (const auto& [Name, Value] : Morphs->Values)
                        Component->SetMorphTarget(FName(*Name), Value->AsNumber());
            }
            else
            {
                FString Project, MotionError;
                if (Sections[0].Data->TryGetStringField(TEXT("animationProject"), Project))
                    PlayLocomotion(*Component, Project, TEXT("T5Idle"), 1.f, {}, MotionError);
                Component->GlobalAnimRateScale=0;
                Component->SetComponentTickEnabled(false);
                Component->TickAnimation(0,false);
                Component->RefreshBoneTransforms();
            }
        }
    if (!Count) { Actor->Destroy(); Error = TEXT("Не удалось построить модель персонажа"); return nullptr; }
    return Actor;
}

namespace
{
    UStaticMesh* BuildSectionMesh(AActor& Owner, FMaterialFactory& Factory, TConstArrayView64<uint8> Geometry,
        const TArray<TSharedPtr<FJsonObject>>& Sections)
    {
        FMeshGroup Group(false);
        for (const auto& Section : Sections)
        {
            auto Task = AddSection(Group, Section, Geometry, Factory);
            while (!Task.Done()) Task.Resume();
            if (!Task.Succeeded()) return nullptr;
        }
        if (Group.Materials.IsEmpty()) return nullptr;
        FStaticMeshOperations::ComputeTriangleTangentsAndNormals(Group.Description);
        FStaticMeshOperations::ComputeTangentsAndNormals(Group.Description, EComputeNTBsFlags::UseMikkTSpace);
        auto* Mesh = NewObject<UStaticMesh>(&Owner, NAME_None, RF_Transient);
        Mesh->SetStaticMaterials(Group.Materials);
        UStaticMesh::FBuildMeshDescriptionsParams Build;
        Build.bFastBuild = true;
        Build.bCommitMeshDescription = false;
        Build.bMarkPackageDirty = false;
        return Mesh->BuildFromMeshDescriptions({&Group.Description}, Build) ? Mesh : nullptr;
    }
}

AT5Scene::FAnimationProp AT5Scene::BuildAnimationProp(const FT5LoadResult& Data)
{
    FAnimationProp Result;
    Result.bAttempted = true;
    if (!Data.Error.IsEmpty()) return Result;
    const auto Manifest = ReadJson(Data, TEXT("meshes.json"));
    const auto Prop = ReadJson(Data, TEXT("prop.json"));
    if (!Manifest || !Prop) return Result;
    Result.Socket = FName(*Prop->GetStringField(TEXT("socket")));
    Result.Unload = Prop->GetStringField(TEXT("unload"));
    TArray<TSharedPtr<FJsonObject>> Sections;
    for (const auto& Value : Manifest->GetArrayField(TEXT("sections"))) Sections.Add(Value->AsObject());
    FMaterialFactory Factory{this, Data, RecentTextures};
    if (auto* Mesh = BuildSectionMesh(*this, Factory, Data.Read(TEXT("meshes.bin")), Sections))
    {
        auto* Component = NewObject<UStaticMeshComponent>(this, NAME_None, RF_Transient);
        Component->SetStaticMesh(Mesh);
        Component->SetCollisionEnabled(ECollisionEnabled::NoCollision);
        Component->SetVisibility(false);
        Component->RegisterComponent();
        Result.Component = Component;
    }
    return Result;
}

bool AT5Scene::BuildFirstPersonView(const FT5LoadResult& Data)
{
    const auto Manifest = ReadJson(Data, TEXT("meshes.json"));
    const auto Settings = ReadJson(Data, TEXT("view.json"));
    if (!Manifest || !Settings || !Data.Error.IsEmpty()) return false;
    const auto& Rigs = Manifest->GetObjectField(TEXT("rigs"));
    FString Rig;
    TArray<TSharedPtr<FJsonObject>> Arms, Weapon;
    for (const auto& Value : Manifest->GetArrayField(TEXT("sections")))
    {
        const auto Section = Value->AsObject();
        const auto Group = Section->GetStringField(TEXT("group"));
        if (Group == TEXT("NPC") && Section->TryGetStringField(TEXT("rig"), Rig)) Arms.Add(Section);
        else if (Group == TEXT("ViewWeapon")) Weapon.Add(Section);
    }
    if (Arms.IsEmpty()) return false;
    FActorSpawnParameters Spawn;
    Spawn.ObjectFlags = RF_Transient;
    auto* Actor = GetWorld()->SpawnActor<AActor>(FVector::ZeroVector, FRotator::ZeroRotator, Spawn);
    if (!Actor) return false;
    auto* Root = NewObject<USceneComponent>(Actor);
    Actor->SetRootComponent(Root);
    Root->RegisterComponent();
    FMaterialFactory Factory{Actor, Data, RecentTextures};
    TArray<FT5SkinSection> Sections;
    for (const auto& Section : Arms) Sections.Add({Section, Factory.Surface(Section->GetObjectField(TEXT("material")))});
    const auto Geometry = Data.Read(TEXT("meshes.bin"));
    auto* Mesh = BuildT5SkeletalMesh(*Actor, *Root, Rigs->GetArrayField(Rig), Sections,
        Geometry, Data.Read(TEXT("skins.bin")), Data.Read(TEXT("morphs.bin")));
    if (!Mesh) { Actor->Destroy(); return false; }
    Mesh->SetCastShadow(false);
    Mesh->SetFirstPersonPrimitiveType(EFirstPersonPrimitiveType::FirstPerson);
    Mesh->SetVisibleInRayTracing(false);
    Mesh->VisibilityBasedAnimTickOption = EVisibilityBasedAnimTickOption::AlwaysTickPoseAndRefreshBones;
    FirstPersonActor = Actor;
    FirstPersonArms = Mesh;
    FirstPersonWeaponType = Settings->GetIntegerField(TEXT("type"));
    if (auto* Asset = BuildSectionMesh(*Actor, Factory, Geometry, Weapon))
    {
        auto* Part = NewObject<UStaticMeshComponent>(Actor);
        Part->SetupAttachment(Mesh, FirstPersonWeaponType == 7 ? FName(TEXT("SHIELD")) : FName(TEXT("Weapon")));
        Part->SetStaticMesh(Asset);
        Part->SetCollisionEnabled(ECollisionEnabled::NoCollision);
        Part->SetCastShadow(false);
        Part->SetVisibleInRayTracing(false);
        Part->SetFirstPersonPrimitiveType(EFirstPersonPrimitiveType::FirstPerson);
        Part->SetVisibility(false);
        Part->RegisterComponent();
        FirstPersonWeapon = Part;
    }
    UE_LOG(LogT5Data, Display, TEXT("T5_VIEW_MODEL armSections=%d weapon=%d type=%d"),
        Sections.Num(), FirstPersonWeapon.IsValid(), FirstPersonWeaponType);
    return true;
}
