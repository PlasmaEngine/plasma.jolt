using PlasmaBuild.Core.Configuration;
using PlasmaBuild.Core.Rules;

public class JoltPluginTarget : TargetRules
{
    public JoltPluginTarget(BuildContext context) : base(context)
    {
        Type = TargetType.SharedLibrary;
        OutputName = "plJoltPlugin";
        OutputDirectory = PlasmaPackageSdk.PackageBinaryDirectory(context);
        UsePCHFiles = true;
        UseUnityBuild = true;
        UseAdaptiveUnityBuild = true;
        UseIncrementalLinking = true;
        ExtraModules.Add("JoltPluginModule");
        ExtraModules.Add("JoltModule");
    }
}
