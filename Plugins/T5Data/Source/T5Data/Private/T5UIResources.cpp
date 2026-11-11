#include "T5UIResources.h"

#include "Algo/AllOf.h"
#include "Async/Async.h"
#include "Dom/JsonObject.h"
#include "Engine/FontFace.h"
#include "Engine/Texture2D.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/Base64.h"
#include "Misc/CommandLine.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonSerializer.h"
#include "Styling/CoreStyle.h"
#if PLATFORM_WINDOWS
#include "Windows/WindowsHWrapper.h"
#endif

DEFINE_LOG_CATEGORY_STATIC(LogT5UI, Log, All);

namespace
{
    constexpr int32 CacheVersion = 9;
    constexpr uint32 CompressionToggle = 0x40000000;
    constexpr int32 MaxMovieBytes = 8 * 1024 * 1024;
    constexpr int32 MaxFontMovieBytes = 32 * 1024 * 1024;
    const TMap<FString, FName> FontAliases = {
        {TEXT("$EverywhereFont"), TEXT("Regular")},
        {TEXT("$EverywhereMediumFont"), TEXT("Medium")},
        {TEXT("$EverywhereBoldFont"), TEXT("Bold")},
        {TEXT("$DialogueFont"), TEXT("Dialogue")},
    };
    const FT5UIArtSpec HudArt[] = {
        {TEXT("CompassFrame"), TEXT("HUDMovie/CompassShoutMeterHolder/Compass/CompassFrame")},
        {TEXT("QuestMarker"), TEXT("QuestMarker")},
        {TEXT("QuestDoor"), TEXT("quest_door")},
        {TEXT("PlayerMarker"), TEXT("PlayerSet")},
        {TEXT("EnemyMarker"), TEXT("Enemy")},
        {TEXT("DoorMarker"), TEXT("Door")},
        {TEXT("Crosshair"), TEXT("HUDMovie/Crosshair")},
        {TEXT("ResourceMeter"), TEXT("HUDMovie/Health/HealthMeter_mc/#1")},
        {TEXT("HealthFill"), TEXT("HUDMovie/Health/HealthMeter_mc/HealthLeft/#4")},
        {TEXT("MagickaFill"), TEXT("HUDMovie/Magica/MagickaMeter_mc/#3")},
        {TEXT("StaminaFill"), TEXT("HUDMovie/Stamina/StaminaMeter_mc/#3")},
    };
    const FT5UIArtSpec ItemListArt[] = {
        {TEXT("ItemCategoryPanel"), TEXT("InventoryLists/CategoriesListHolder/ListBackground")},
        {TEXT("ItemListPanel"), TEXT("InventoryLists/ItemsListHolder/ListBackground")},
        {TEXT("ItemEquipped"), TEXT("EquipIcon")},
    };
    const FT5UIArtSpec ItemCardArt[] = {{TEXT("ItemCard"), TEXT("ITEM CARD BASE")}};
    const FT5UIArtSpec ItemBottomArt[] = {{TEXT("ItemBottom"), TEXT("BottomBar")}};
    const FT5UIArtSpec DialogueArt[] = {
        {TEXT("DialoguePanel"), TEXT("DialogueMenuObj/TopicListHolder/ListPanel")},
        {TEXT("DialogueNext"), TEXT("DialogueMenuObj/TopicListHolder/ScrollIndicators/Down")},
    };
    const FT5UIArtSpec MapArt[] = {
        {TEXT("MapTown"), TEXT("TownMarker/IconClip")},
        {TEXT("MapDoor"), TEXT("DoorMarker/IconClip")},
        {TEXT("MapSelection"), TEXT("YouAreHereMarker/Icon")},
    };
    const FT5UIArtSpec JournalArt[] = {
        {TEXT("JournalPanel"), TEXT("QuestJournalBase")},
        {TEXT("JournalDivider"), TEXT("SystemPage/SystemDivider")},
        {TEXT("JournalBottom"), TEXT("JournalBottomBar")},
    };
    const FT5UIArtSpec RaceArt[] = {
        {TEXT("RacePanel"), TEXT("RaceSexPanels/PanelTwoWideInstance")},
        {TEXT("RaceHeader"), TEXT("Base/CagetoryLockBaseInstance/CategoryInstance")},
        {TEXT("RaceBottom"), TEXT("Base/BottomBarInstance")},
        {TEXT("RaceSliderTrack"), TEXT("slider_track")},
        {TEXT("RaceSliderThumb"), TEXT("slider_thumb")},
    };
    const struct FMovieArt
    {
        const TCHAR* Path;
        TConstArrayView<FT5UIArtSpec> Specs;
    } Movies[] = {
        {TEXT("interface/hudmenu.swf"), HudArt},
        {TEXT("interface/inventory components/inventorylists.swf"), ItemListArt},
        {TEXT("interface/inventory components/itemcard.swf"), ItemCardArt},
        {TEXT("interface/inventory components/bottombar.swf"), ItemBottomArt},
        {TEXT("interface/dialoguemenu.swf"), DialogueArt},
        {TEXT("interface/map.swf"), MapArt},
        {TEXT("interface/quest_journal.swf"), JournalArt},
        {TEXT("interface/racesex_menu.swf"), RaceArt},
    };

    struct FEntry
    {
        FString Path;
        uint32 Offset = 0, Size = 0;
        bool bCompressed = false;
    };

    FString InterfaceLanguage(const FString& Data)
    {
        FString Language;
        if (!FParse::Value(FCommandLine::Get(), TEXT("T5Language="), Language))
        {
            const FString Game = FPaths::GetPath(Data);
            const FString Documents = FPlatformProcess::UserDir();
            for (const FString& Path : {Game / TEXT("Skyrim.ini"),
                    Documents / TEXT("My Games/Skyrim Special Edition/Skyrim.ini"),
                    Game / TEXT("Skyrim_Default.ini")})
            {
                if (!IFileManager::Get().FileExists(*Path))
                    continue;
                FConfigFile Ini;
                Ini.Read(Path);
                if (Ini.GetString(TEXT("General"), TEXT("sLanguage"), Language) && !Language.IsEmpty())
                    break;
            }
        }
        Language.TrimStartAndEndInline();
        Language.ToLowerInline();
        Language.ReplaceInline(TEXT("_"), TEXT("-"));
        static const TMap<FString, FString> Aliases = {
            {TEXT("english"), TEXT("en")}, {TEXT("russian"), TEXT("ru")}, {TEXT("polish"), TEXT("pl")},
            {TEXT("japanese"), TEXT("ja")}, {TEXT("chinese"), TEXT("cn")}, {TEXT("tchinese"), TEXT("cn")},
            {TEXT("schinese"), TEXT("cn")}, {TEXT("zh"), TEXT("cn")}, {TEXT("jp"), TEXT("ja")},
            {TEXT("french"), TEXT("fr")}, {TEXT("german"), TEXT("de")}, {TEXT("italian"), TEXT("it")},
            {TEXT("spanish"), TEXT("es")}, {TEXT("portuguese"), TEXT("pt")}, {TEXT("korean"), TEXT("ko")},
        };
        if (const FString* Code = Aliases.Find(Language))
            return *Code;
        FString BaseLanguage;
        if (Language.Split(TEXT("-"), &BaseLanguage, nullptr))
            Language = MoveTemp(BaseLanguage);
        if (Language == TEXT("zh"))
            return TEXT("cn");
        if (Language.Len() < 2 || Language.Len() > 3)
            return TEXT("en");
        for (const TCHAR Character : Language)
            if (Character < TEXT('a') || Character > TEXT('z'))
                return TEXT("en");
        return Language;
    }

    bool IndexArchive(FArchive& Archive, TArray<FEntry>& Entries, uint32& Flags)
    {
        uint32 Magic = 0, Version = 0, Offset = 0, Folders = 0, Files = 0;
        uint32 FolderBytes = 0, NameBytes = 0, FileTypes = 0;
        Archive << Magic << Version << Offset << Flags << Folders << Files << FolderBytes << NameBytes << FileTypes;
        const int64 IndexEnd = int64(Offset) + int64(Folders) * 25 + FolderBytes + int64(Files) * 16 + NameBytes;
        if (Archive.IsError() || Magic != 0x00415342 || Version != 105 || Offset < 36 || (Flags & 3) != 3
            || !Folders || !Files || Folders > 16384 || Files > 200000 || IndexEnd > Archive.TotalSize()
            || FolderBytes > 4 * 1024 * 1024 || NameBytes > 16 * 1024 * 1024)
            return false;

        Archive.Seek(Offset);
        TArray<uint32> Counts;
        Counts.Reserve(Folders);
        int64 Counted = 0;
        for (uint32 Index = 0; Index < Folders; ++Index)
        {
            uint64 Hash = 0, FolderOffset = 0;
            uint32 Count = 0, Reserved = 0;
            Archive << Hash << Count << Reserved << FolderOffset;
            Counts.Add(Count);
            Counted += Count;
        }
        if (Archive.IsError() || Counted != Files)
            return false;

        Entries.Reserve(Files);
        uint32 ReadFolderBytes = 0;
        for (uint32 Count : Counts)
        {
            uint8 Length = 0;
            Archive << Length;
            ANSICHAR Name[256]{};
            ReadFolderBytes += Length;
            if (!Length || ReadFolderBytes > FolderBytes)
                return false;
            Archive.Serialize(Name, Length);
            if (Name[Length - 1] != '\0')
                return false;
            const FString Folder = FString(ANSI_TO_TCHAR(Name)).Replace(TEXT("\\"), TEXT("/")).ToLower();
            for (uint32 Index = 0; Index < Count; ++Index)
            {
                uint64 Hash = 0;
                uint32 PackedSize = 0;
                FEntry& Entry = Entries.AddDefaulted_GetRef();
                Archive << Hash << PackedSize << Entry.Offset;
                Entry.Path = Folder;
                Entry.Size = PackedSize & 0x3fffffff;
                Entry.bCompressed = !!(Flags & 4) != !!(PackedSize & CompressionToggle);
                if (Entry.Offset < IndexEnd || int64(Entry.Offset) + Entry.Size > Archive.TotalSize())
                    return false;
            }
        }
        if (Archive.IsError() || Archive.Tell() != IndexEnd - NameBytes)
            return false;

        TArray<ANSICHAR> Names;
        Names.SetNumUninitialized(NameBytes);
        Archive.Serialize(Names.GetData(), NameBytes);
        int32 Cursor = 0;
        for (FEntry& Entry : Entries)
        {
            const int32 Start = Cursor;
            while (Cursor < Names.Num() && Names[Cursor])
                ++Cursor;
            if (Cursor == Start || Cursor == Names.Num())
                return false;
            Entry.Path = (Entry.Path / ANSI_TO_TCHAR(&Names[Start])).ToLower();
            ++Cursor;
        }
        return !Archive.IsError() && Cursor == Names.Num();
    }

    bool ReadMovie(FArchive& Archive, const FEntry& Entry, uint32 Flags, const FString& Bridge, TArray<uint8>& Movie,
        uint32 MaxBytes = MaxMovieBytes)
    {
        if (!Entry.Size || Entry.Size > MaxBytes)
            return false;
        Archive.Seek(Entry.Offset);
        uint32 Remaining = Entry.Size;
        if (Flags & 0x100)
        {
            uint8 Length = 0;
            Archive << Length;
            if (Length + 1u >= Remaining)
                return false;
            Archive.Seek(Archive.Tell() + Length);
            Remaining -= Length + 1u;
        }
        if (!Entry.bCompressed)
        {
            Movie.SetNumUninitialized(Remaining);
            Archive.Serialize(Movie.GetData(), Remaining);
            return !Archive.IsError();
        }
        if (Remaining <= sizeof(uint32))
            return false;
        uint32 UnpackedSize = 0;
        Archive << UnpackedSize;
        if (!UnpackedSize || UnpackedSize > MaxBytes)
            return false;
        TArray<uint8> Packed;
        Packed.SetNumUninitialized(Remaining - sizeof(uint32));
        Archive.Serialize(Packed.GetData(), Packed.Num());
        if (Archive.IsError())
            return false;

        // Skyrim SE/AE BSA v105 uses LZ4 frames. The bridge already ships this library.
        void* Library = FPlatformProcess::GetDllHandle(*(Bridge / TEXT("lz4.dll")));
        if (!Library)
            return false;
        const auto Create = reinterpret_cast<SIZE_T (*)(void**, unsigned)>(
            FPlatformProcess::GetDllExport(Library, TEXT("LZ4F_createDecompressionContext")));
        const auto Decompress = reinterpret_cast<SIZE_T (*)(void*, void*, SIZE_T*, const void*, SIZE_T*, const void*)>(
            FPlatformProcess::GetDllExport(Library, TEXT("LZ4F_decompress")));
        const auto Destroy = reinterpret_cast<SIZE_T (*)(void*)>(
            FPlatformProcess::GetDllExport(Library, TEXT("LZ4F_freeDecompressionContext")));
        bool bOk = false;
        void* Context = nullptr;
        if (Create && Decompress && Destroy && Create(&Context, 100) == 0)
        {
            Movie.SetNumUninitialized(UnpackedSize);
            SIZE_T Dst = UnpackedSize, Src = Packed.Num();
            bOk = Decompress(Context, Movie.GetData(), &Dst, Packed.GetData(), &Src, nullptr) == 0
                && Dst == UnpackedSize && Src == SIZE_T(Packed.Num());
            Destroy(Context);
        }
        FPlatformProcess::FreeDllHandle(Library);
        return bOk;
    }

    FString ArchiveIdentity(const FString& Path)
    {
        const FFileStatData Stat = IFileManager::Get().GetStatData(*Path);
        return Stat.bIsValid && !Stat.bIsDirectory
            ? FString::Printf(TEXT("%s|%lld|%lld"), *Path, Stat.FileSize, Stat.ModificationTime.GetTicks())
            : FString();
    }

    bool ReadFontConfig(FArchive& Archive, const TArray<FEntry>& Entries, uint32 Flags, const FString& Bridge,
        const FString& Config, FT5UIFontSet& Out)
    {
        const FEntry* Entry = Entries.FindByPredicate([&](const FEntry& Item) { return Item.Path == Config; });
        TArray<uint8> Bytes;
        FString Text;
        if (!Entry || !ReadMovie(Archive, *Entry, Flags, Bridge, Bytes, 256 * 1024))
            return false;
        FFileHelper::BufferToString(Text, Bytes.GetData(), Bytes.Num());

        TArray<FString> Lines, Libraries;
        Text.ParseIntoArrayLines(Lines);
        FT5UIFontSet Fonts;
        for (const FString& Line : Lines)
        {
            TArray<FString> Fields;
            Line.ParseIntoArray(Fields, TEXT("\""), false);
            if (Fields.Num() < 3)
                continue;
            const FString Command = Fields[0].TrimStartAndEnd();
            if (Command.Equals(TEXT("fontlib"), ESearchCase::IgnoreCase))
            {
                FString Library = Fields[1].Replace(TEXT("\\"), TEXT("/")).ToLower();
                Libraries.AddUnique(MoveTemp(Library));
            }
            else if (Command.Equals(TEXT("map"), ESearchCase::IgnoreCase) && Fields.Num() >= 5)
            {
                if (const FName* Role = FontAliases.Find(Fields[1]))
                {
                    TArray<FString> Modifiers;
                    Fields[4].ParseIntoArrayWS(Modifiers);
                    uint8 Style = 0;
                    for (const FString& Modifier : Modifiers)
                    {
                        if (Modifier.Equals(TEXT("Bold"), ESearchCase::IgnoreCase)) Style |= 1;
                        if (Modifier.Equals(TEXT("Italic"), ESearchCase::IgnoreCase)) Style |= 2;
                    }
                    Fonts.Roles.Add(*Role, T5FontKey(Fields[3], Style));
                }
            }
        }
        for (const FString& Library : Libraries)
        {
            const FEntry* Movie = Entries.FindByPredicate([&](const FEntry& Item) { return Item.Path == Library; });
            if (Movie && ReadMovie(Archive, *Movie, Flags, Bridge, Bytes, MaxFontMovieBytes))
                ImportT5UIFonts(Bytes, Fonts);
            if (Algo::AllOf(Fonts.Roles, [&](const auto& Role) { return Fonts.Faces.Contains(Role.Value); }))
                break;
        }
        for (auto Role = Fonts.Roles.CreateIterator(); Role; ++Role)
            if (!Fonts.Faces.Contains(Role.Value()))
                Role.RemoveCurrent();
        if (Fonts.Roles.IsEmpty())
            return false;
        Out = MoveTemp(Fonts);
        return true;
    }

    bool ReadCache(const FString& Path, const FString& Identity, int32& EntryCount, TMap<FName, FT5UIBitmap>& Art,
        const FString& Language, FString& FontConfig, FT5UIFontSet& Fonts)
    {
        auto& Files = IFileManager::Get();
        const int64 Size = Files.FileSize(*Path);
        FString Json;
        TSharedPtr<FJsonObject> Root;
        int32 Version = 0;
        FString Source, CachedLanguage, CachedFontConfig;
        const TArray<TSharedPtr<FJsonValue>>* Entries = nullptr;
        const TSharedPtr<FJsonObject>* Assets = nullptr;
        const TSharedPtr<FJsonObject>* FontAssets = nullptr;
        const TSharedPtr<FJsonObject>* FontRoles = nullptr;
        if (Size <= 0 || Size > 80 * 1024 * 1024 || !FFileHelper::LoadFileToString(Json, *Path)
            || !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Root) || !Root
            || !Root->TryGetNumberField(TEXT("version"), Version) || Version != CacheVersion
            || !Root->TryGetStringField(TEXT("source"), Source) || Source != Identity
            || !Root->TryGetStringField(TEXT("language"), CachedLanguage) || CachedLanguage != Language
            || !Root->TryGetStringField(TEXT("fontConfig"), CachedFontConfig)
            || !Root->TryGetArrayField(TEXT("entries"), Entries) || Entries->IsEmpty()
            || !Root->TryGetObjectField(TEXT("art"), Assets) || (*Assets)->Values.Num() > 64
            || !Root->TryGetObjectField(TEXT("fonts"), FontAssets) || (*FontAssets)->Values.Num() > FontAliases.Num()
            || !Root->TryGetObjectField(TEXT("fontRoles"), FontRoles) || (*FontRoles)->Values.Num() > FontAliases.Num())
            return false;

        TMap<FName, FT5UIBitmap> Loaded;
        for (const auto& [Name, Value] : (*Assets)->Values)
        {
            const TSharedPtr<FJsonObject>* Asset = nullptr;
            FT5UIBitmap Bitmap;
            FString Encoded;
            TArray<uint8> Bytes;
            if (!Value->TryGetObject(Asset)
                || !(*Asset)->TryGetNumberField(TEXT("width"), Bitmap.Size.X)
                || !(*Asset)->TryGetNumberField(TEXT("height"), Bitmap.Size.Y)
                || Bitmap.Size.GetMin() <= 0 || Bitmap.Size.GetMax() > 2048
                || !(*Asset)->TryGetNumberField(TEXT("shapes"), Bitmap.Shapes) || Bitmap.Shapes < 1 || Bitmap.Shapes > 32
                || !(*Asset)->TryGetStringField(TEXT("source"), Bitmap.Source)
                || !(*Asset)->TryGetStringField(TEXT("symbol"), Bitmap.Symbol)
                || !(*Asset)->TryGetStringField(TEXT("bgra"), Encoded)
                || !FBase64::Decode(Encoded, Bytes) || Bytes.Num() != Bitmap.Size.X * Bitmap.Size.Y * sizeof(FColor))
                return false;
            Bitmap.Pixels.SetNumUninitialized(Bytes.Num() / sizeof(FColor));
            FMemory::Memcpy(Bitmap.Pixels.GetData(), Bytes.GetData(), Bytes.Num());
            Loaded.Add(FName(Name), MoveTemp(Bitmap));
        }
        if (!Loaded.Contains(TEXT("QuestMarker")))
            return false;
        FT5UIFontSet LoadedFonts;
        for (const auto& [Name, Value] : (*FontAssets)->Values)
        {
            FString Encoded;
            TArray<uint8> Bytes;
            if (Name.Len() > 256 || !Value->TryGetString(Encoded) || Encoded.Len() > 48 * 1024 * 1024
                || !FBase64::Decode(Encoded, Bytes) || Bytes.Num() < 12
                || Bytes[0] != 0 || Bytes[1] != 1 || Bytes[2] != 0 || Bytes[3] != 0)
                return false;
            LoadedFonts.Faces.Add(FName(Name), MoveTemp(Bytes));
        }
        for (const auto& [Name, Value] : (*FontRoles)->Values)
        {
            const FName Role(Name);
            FString Key;
            if (!FontAliases.FindKey(Role) || !Value->TryGetString(Key) || Key.Len() > 256
                || !LoadedFonts.Faces.Contains(FName(Key)))
                return false;
            LoadedFonts.Roles.Add(Role, FName(Key));
        }
        EntryCount = Entries->Num();
        Art = MoveTemp(Loaded);
        FontConfig = MoveTemp(CachedFontConfig);
        Fonts = MoveTemp(LoadedFonts);
        return true;
    }

    bool WriteCache(const FString& Path, const FString& Identity, const TArray<FEntry>& Entries,
        const TMap<FName, FT5UIBitmap>& Art, const FString& Language, const FString& FontConfig, const FT5UIFontSet& Fonts)
    {
        auto Root = MakeShared<FJsonObject>();
        Root->SetNumberField(TEXT("version"), CacheVersion);
        Root->SetStringField(TEXT("source"), Identity);
        Root->SetStringField(TEXT("language"), Language);
        Root->SetStringField(TEXT("fontConfig"), FontConfig);
        TArray<TSharedPtr<FJsonValue>> Files;
        Files.Reserve(Entries.Num());
        for (const FEntry& Entry : Entries)
        {
            auto File = MakeShared<FJsonObject>();
            File->SetStringField(TEXT("path"), Entry.Path);
            File->SetNumberField(TEXT("storedBytes"), Entry.Size);
            File->SetBoolField(TEXT("compressed"), Entry.bCompressed);
            Files.Add(MakeShared<FJsonValueObject>(File));
        }
        Root->SetArrayField(TEXT("entries"), MoveTemp(Files));
        auto Assets = MakeShared<FJsonObject>();
        for (const auto& [Name, Bitmap] : Art)
        {
            auto Asset = MakeShared<FJsonObject>();
            Asset->SetNumberField(TEXT("width"), Bitmap.Size.X);
            Asset->SetNumberField(TEXT("height"), Bitmap.Size.Y);
            Asset->SetNumberField(TEXT("shapes"), Bitmap.Shapes);
            Asset->SetStringField(TEXT("source"), Bitmap.Source);
            Asset->SetStringField(TEXT("symbol"), Bitmap.Symbol);
            Asset->SetStringField(TEXT("bgra"), FBase64::Encode(reinterpret_cast<const uint8*>(Bitmap.Pixels.GetData()),
                Bitmap.Pixels.Num() * sizeof(FColor)));
            Assets->SetObjectField(Name.ToString(), Asset);
        }
        Root->SetObjectField(TEXT("art"), Assets);
        auto FontAssets = MakeShared<FJsonObject>();
        for (const auto& [Face, Bytes] : Fonts.Faces)
            FontAssets->SetStringField(Face.ToString(), FBase64::Encode(Bytes));
        Root->SetObjectField(TEXT("fonts"), FontAssets);
        auto FontRoles = MakeShared<FJsonObject>();
        for (const auto& [Role, Face] : Fonts.Roles)
            FontRoles->SetStringField(Role.ToString(), Face.ToString());
        Root->SetObjectField(TEXT("fontRoles"), FontRoles);

        FString Json;
        const auto Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Json);
        if (!FJsonSerializer::Serialize(Root, Writer))
            return false;
        auto& FileManager = IFileManager::Get();
        FileManager.MakeDirectory(*FPaths::GetPath(Path), true);
        const FString Temporary = Path + TEXT(".") + FGuid::NewGuid().ToString(EGuidFormats::Digits) + TEXT(".tmp");
        bool bSaved = FFileHelper::SaveStringToFile(Json, *Temporary, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
#if PLATFORM_WINDOWS
        bSaved = bSaved && ::MoveFileExW(*Temporary, *Path, MOVEFILE_REPLACE_EXISTING);
#else
        bSaved = bSaved && FileManager.Move(*Path, *Temporary, true, false, false, true);
#endif
        if (!bSaved)
            FileManager.Delete(*Temporary, false, true);
        return bSaved;
    }
}

FT5UIResources::FT5UIResources(const FString& Data, const FString& Bridge)
{
    const FString Archive = Data / TEXT("Skyrim - Interface.bsa");
    const FString Cache = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("T5UICache/catalog.json"));
    const FString Language = InterfaceLanguage(Data);
    Pending = Async(EAsyncExecution::ThreadPool,
        [Archive, Bridge, Cache, Language] { return Build(Archive, Bridge, Cache, Language); });
}

FT5UIResources::FCatalog FT5UIResources::Build(const FString& Path, const FString& Bridge, const FString& Cache,
    const FString& Language)
{
    const FString Identity = ArchiveIdentity(Path);
    FCatalog Result;
    if (!Identity.IsEmpty() && ReadCache(Cache, Identity, Result.EntryCount, Result.Art, Language,
            Result.FontConfig, Result.Fonts))
    {
        UE_LOG(LogT5UI, Display, TEXT("T5_UI_CACHE hit entries=%d art=%d fonts=%d language=%s config=%s"),
            Result.EntryCount, Result.Art.Num(), Result.Fonts.Faces.Num(), *Language, *Result.FontConfig);
        return Result;
    }

    TArray<FEntry> Entries;
    uint32 Flags = 0;
    TUniquePtr<FArchive> Archive(IFileManager::Get().CreateFileReader(*Path, FILEREAD_Silent));
    if (Identity.IsEmpty() || !Archive || !IndexArchive(*Archive, Entries, Flags))
    {
        UE_LOG(LogT5UI, Warning, TEXT("Cannot index Skyrim SE/AE interface archive: %s"), *Path);
        return {};
    }

    Result.EntryCount = Entries.Num();
    for (const auto& [MoviePath, Specs] : Movies)
    {
        const FEntry* Entry = Entries.FindByPredicate([MoviePath](const FEntry& Item) { return Item.Path == MoviePath; });
        TArray<uint8> Movie;
        if (Entry && ReadMovie(*Archive, *Entry, Flags, Bridge, Movie))
            Result.Art.Append(ImportT5UIArt(Movie, MoviePath, Specs));
        for (const auto& Spec : Specs)
            if (!Result.Art.Contains(Spec.Name))
                UE_LOG(LogT5UI, Warning, TEXT("UI symbol unavailable in %s: %s"), MoviePath, Spec.Symbol);
    }
    for (const FString& Config : {FString::Printf(TEXT("interface/fontconfig_%s.txt"), *Language),
            FString(TEXT("interface/fontconfig.txt"))})
    {
        if (ReadFontConfig(*Archive, Entries, Flags, Bridge, Config, Result.Fonts))
        {
            Result.FontConfig = Config;
            break;
        }
    }
    for (const auto& [Alias, Role] : FontAliases)
        if (!Result.Fonts.Roles.Contains(Role))
            UE_LOG(LogT5UI, Warning, TEXT("UI font alias unavailable for %s: %s"), *Language, *Alias);
    Archive.Reset();
    if (ArchiveIdentity(Path) != Identity)
        return {};
    const bool bSaved = WriteCache(Cache, Identity, Entries, Result.Art, Language, Result.FontConfig, Result.Fonts);
    UE_LOG(LogT5UI, Display, TEXT("T5_UI_CACHE built entries=%d art=%d fonts=%d language=%s config=%s saved=%d"),
        Result.EntryCount, Result.Art.Num(), Result.Fonts.Faces.Num(), *Language, *Result.FontConfig, bSaved);
    return Result;
}

void FT5UIResources::Poll()
{
    if (Pending.IsValid() && Pending.IsReady())
    {
        Catalog = Pending.Get();
        Pending = {};
        if (!Catalog.Fonts.Faces.IsEmpty())
        {
            Typeface = MakeShared<FStandaloneCompositeFont>();
            const auto Fallback = FCoreStyle::GetDefaultFontStyle(TEXT("Regular"), 16);
            if (const FCompositeFont* Font = Fallback.GetCompositeFont())
                Typeface->FallbackTypeface.Typeface = Font->DefaultTypeface;
            TMap<FName, UFontFace*> Faces;
            for (auto& [Key, Bytes] : Catalog.Fonts.Faces)
            {
                UFontFace* Face = NewObject<UFontFace>();
                Face->SourceFilename = Key.ToString() + TEXT(".ttf");
                Face->LoadingPolicy = EFontLoadingPolicy::Inline;
                Face->FontFaceData->SetData(MoveTemp(Bytes));
                FontFaces.Emplace(Face);
                Faces.Add(Key, Face);
            }
            for (const auto& [Role, Key] : Catalog.Fonts.Roles)
                Typeface->DefaultTypeface.Fonts.Emplace_GetRef(Role).Font = FFontData(Faces.FindChecked(Key));
            Catalog.Fonts.Faces.Reset();
        }
    }
}

FSlateFontInfo FT5UIResources::GetFont(FName Role, int32 Size)
{
    Poll();
    if (Typeface && Typeface->DefaultTypeface.Fonts.ContainsByPredicate(
        [Role](const FTypefaceEntry& Face) { return Face.Name == Role; }))
        return FSlateFontInfo(Typeface, Size, Role);
    return FCoreStyle::GetDefaultFontStyle(TEXT("BoldCondensed"), Size);
}

const FSlateBrush* FT5UIResources::GetBrush(FName Name)
{
    Poll();
#if WITH_DEV_AUTOMATION_TESTS
    if (bTrackArt && Catalog.Art.Contains(Name))
        AccessedArt.Add(Name);
#endif
    if (const auto* Image = Images.Find(Name))
        return &(*Image)->Brush;
    const FT5UIBitmap* Bitmap = Catalog.Art.Find(Name);
    if (!Bitmap)
        return nullptr;
    UTexture2D* Texture = UTexture2D::CreateTransient(Bitmap->Size.X, Bitmap->Size.Y, PF_B8G8R8A8, NAME_None,
        MakeArrayView(reinterpret_cast<const uint8*>(Bitmap->Pixels.GetData()), Bitmap->Pixels.Num() * sizeof(FColor)));
    if (!Texture)
        return nullptr;
    Texture->SRGB = true;
    Texture->NeverStream = true;
    Texture->UpdateResource();
    auto Image = MakeUnique<FImage>();
    Image->Texture.Reset(Texture);
    Image->Brush.SetResourceObject(Texture);
    Image->Brush.ImageSize = FVector2D(Bitmap->Size);
    Image->Brush.DrawAs = ESlateBrushDrawType::Image;
    return &Images.Add(Name, MoveTemp(Image))->Brush;
}

const FSliderStyle* FT5UIResources::GetRaceSliderStyle()
{
    const auto& Default = FCoreStyle::Get().GetWidgetStyle<FSliderStyle>(TEXT("Slider"));
    if (!RaceSlider)
    {
        const auto* Track = GetBrush(TEXT("RaceSliderTrack"));
        const auto* Thumb = GetBrush(TEXT("RaceSliderThumb"));
        if (!Track || !Thumb)
            return &Default;
        RaceSlider = MakeUnique<FSliderStyle>(Default);
        RaceSlider->SetNormalBarImage(*Track).SetHoveredBarImage(*Track).SetDisabledBarImage(*Track)
            .SetNormalThumbImage(*Thumb).SetHoveredThumbImage(*Thumb).SetDisabledThumbImage(*Thumb)
            .SetBarThickness(7);
        for (FSlateBrush* Brush : {&RaceSlider->NormalThumbImage, &RaceSlider->HoveredThumbImage, &RaceSlider->DisabledThumbImage})
            Brush->ImageSize = FVector2D(14, 17);
    }
    return RaceSlider.Get();
}
