#pragma once

#include "CoreMinimal.h"

class FJsonObject;

namespace UnrealMCPBlueprintGraph
{
    TSharedRef<FJsonObject> Execute(
        const TSharedRef<FJsonObject>& Command,
        bool& bOutSucceeded,
        bool& bOutSideEffectsPossible);
}
