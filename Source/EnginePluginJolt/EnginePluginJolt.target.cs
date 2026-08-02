using PlasmaBuild.Core.Configuration;
using PlasmaBuild.Core.Rules;

public class EnginePluginJoltTarget : TargetRules
{
    public EnginePluginJoltTarget(BuildContext context) : base(context)
    {
        Type = TargetType.SharedLibrary;
        OutputName = "plEnginePluginJolt";
        OutputDirectory = PlasmaPackageSdk.PackageBinaryDirectory(context);
        UsePCHFiles = true;
        UseUnityBuild = true;
        UseAdaptiveUnityBuild = true;
        UseIncrementalLinking = true;
        TargetDependencies.Add("JoltPlugin");
        ExtraModules.Add("EnginePluginJoltModule");
        ExtraModules.Add("JoltCookingModule");
        ExtraModules.Add("JoltModule");
    }
}
