#include "BlueprintMCPEditorSubsystem.h"
#include "BlueprintMCPServer.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Misc/Parse.h"
#include "HAL/PlatformMisc.h"

namespace
{
	// Resolve port from (in order):
	//   1) -BlueprintMCPPort=NNNN command line arg
	//   2) UE_BLUEPRINT_MCP_PORT env var (project-specific override)
	//   3) UE_PORT env var (matches the BlueprintMCP TS server's convention)
	//   4) Default 9847
	// Lets PBW + SchoolsOut editors run side-by-side on different ports.
	int32 ResolveBlueprintMCPPort()
	{
		int32 CmdPort = 0;
		if (FParse::Value(FCommandLine::Get(), TEXT("BlueprintMCPPort="), CmdPort) && CmdPort > 0)
		{
			return CmdPort;
		}
		const FString EnvSpecific = FPlatformMisc::GetEnvironmentVariable(TEXT("UE_BLUEPRINT_MCP_PORT"));
		if (!EnvSpecific.IsEmpty())
		{
			const int32 EnvPort = FCString::Atoi(*EnvSpecific);
			if (EnvPort > 0) return EnvPort;
		}
		const FString EnvGeneric = FPlatformMisc::GetEnvironmentVariable(TEXT("UE_PORT"));
		if (!EnvGeneric.IsEmpty())
		{
			const int32 EnvPort = FCString::Atoi(*EnvGeneric);
			if (EnvPort > 0) return EnvPort;
		}
		return 9847;
	}
}

void UBlueprintMCPEditorSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	// Don't start in commandlet mode — the commandlet has its own server instance.
	if (IsRunningCommandlet())
	{
		return;
	}

	const int32 ResolvedPort = ResolveBlueprintMCPPort();

	Server = MakeUnique<FBlueprintMCPServer>();
	if (Server->Start(ResolvedPort, /*bEditorMode=*/true))
	{
		UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: Editor subsystem started — MCP server on port %d"), Server->GetPort());

		// Asset Registry loads asynchronously during editor startup.
		// The initial scan in Start() only sees engine assets.
		// Defer a full rescan until the registry finishes gathering.
		FAssetRegistryModule& ARM = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
		IAssetRegistry& AR = ARM.Get();

		if (AR.IsGathering())
		{
			OnFilesLoadedHandle = AR.OnFilesLoaded().AddUObject(
				this, &UBlueprintMCPEditorSubsystem::HandleAssetRegistryReady);
		}
	}
	else
	{
		UE_LOG(LogTemp, Warning, TEXT("BlueprintMCP: Editor subsystem failed to start MCP server on port %d (port may be in use)"), ResolvedPort);
		Server.Reset();
	}
}

void UBlueprintMCPEditorSubsystem::HandleAssetRegistryReady()
{
	if (OnFilesLoadedHandle.IsValid())
	{
		FAssetRegistryModule& ARM = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
		ARM.Get().OnFilesLoaded().Remove(OnFilesLoadedHandle);
		OnFilesLoadedHandle.Reset();
	}

	if (Server && Server->IsRunning())
	{
		Server->HandleRescan();
		UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: Deferred rescan complete after Asset Registry finished gathering."));
	}
}

void UBlueprintMCPEditorSubsystem::Deinitialize()
{
	if (OnFilesLoadedHandle.IsValid() && FModuleManager::Get().IsModuleLoaded("AssetRegistry"))
	{
		FAssetRegistryModule& ARM = FModuleManager::GetModuleChecked<FAssetRegistryModule>("AssetRegistry");
		ARM.Get().OnFilesLoaded().Remove(OnFilesLoadedHandle);
		OnFilesLoadedHandle.Reset();
	}

	if (Server)
	{
		Server->Stop();
		Server.Reset();
		UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: Editor subsystem stopped."));
	}

	Super::Deinitialize();
}

void UBlueprintMCPEditorSubsystem::Tick(float DeltaTime)
{
	if (Server)
	{
		Server->ProcessOneRequest();
	}
}

bool UBlueprintMCPEditorSubsystem::IsTickable() const
{
	return Server.IsValid() && Server->IsRunning();
}

TStatId UBlueprintMCPEditorSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UBlueprintMCPEditorSubsystem, STATGROUP_Tickables);
}
