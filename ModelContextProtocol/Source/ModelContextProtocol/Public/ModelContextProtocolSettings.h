#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"

#include "ModelContextProtocolSettings.generated.h"

UCLASS(Config=ModelContextProtocol, DefaultConfig, meta=(DisplayName="MCP for Unreal Editor"))
class MODELCONTEXTPROTOCOL_API UModelContextProtocolSettings final : public UDeveloperSettings
{
    GENERATED_BODY()

public:
    virtual FName GetCategoryName() const override
    {
        return TEXT("Plugins");
    }

    /** TCP port used by the local Streamable HTTP server. The MCP client URL must use the same port. */
    UPROPERTY(
        Config,
        EditAnywhere,
        Category="Server",
        meta=(ClampMin="1", ClampMax="65535", UIMin="1", UIMax="65535", ConfigRestartRequired=true))
    int32 Port = 18777;
};
