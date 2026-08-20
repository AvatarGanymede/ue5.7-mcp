using UnrealBuildTool;

public class ModelContextProtocol : ModuleRules
{
    public ModelContextProtocol(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

        PublicDependencyModuleNames.AddRange(new[]
        {
            "Core",
            "DeveloperSettings"
        });

        PrivateDependencyModuleNames.AddRange(new[]
        {
            "BlueprintGraph",
            "CoreUObject",
            "Engine",
            "HTTPServer",
            "Json",
            "Projects",
            "PythonScriptPlugin",
            "UnrealEd"
        });
    }
}
