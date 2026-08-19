using UnrealBuildTool;

public class ModelContextProtocol : ModuleRules
{
    public ModelContextProtocol(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

        PublicDependencyModuleNames.AddRange(new[]
        {
            "Core"
        });

        PrivateDependencyModuleNames.AddRange(new[]
        {
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
