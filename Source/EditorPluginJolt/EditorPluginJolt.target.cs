using PlasmaBuild.Core.Configuration;
using PlasmaBuild.Core.Rules;

public class EditorPluginJoltTarget : TargetRules
{
    public EditorPluginJoltTarget(BuildContext context) : base(context)
    {
        Type = TargetType.SharedLibrary;
        OutputName = "plEditorPluginJolt";
        OutputDirectory = PlasmaPackageSdk.PackageBinaryDirectory(context);
        UsePCHFiles = true;
        UseUnityBuild = true;
        UseAdaptiveUnityBuild = true;
        UseIncrementalLinking = true;
        TargetDependencies.Add("JoltPlugin");
        TargetDependencies.Add("EnginePluginJolt");
        ExtraModules.Add("EditorPluginJoltModule");
        ExtraModules.Add("JoltCookingModule");
        ExtraModules.Add("JoltModule");
    }
}
