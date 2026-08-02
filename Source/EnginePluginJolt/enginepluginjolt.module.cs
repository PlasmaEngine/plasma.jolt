using PlasmaBuild.Core.Configuration;
using PlasmaBuild.Core.Rules;

public class EnginePluginJoltModule : ModuleRules
{
    public EnginePluginJoltModule(BuildContext context) : base(context)
    {
        PlasmaPackageSdk.ConfigurePackageModule(this, context, "Source/EnginePluginJolt",
            "EnginePluginJoltPCH.h", "BUILDSYSTEM_BUILDING_ENGINEPLUGINJOLT_LIB");

        PlasmaPackageSdk.ConfigureEditorSdkConsumer(this, context);
        PlasmaPackageSdk.AddPackagePluginImport(this, context, "Source/JoltPlugin", "plJoltPlugin");

        PublicDependencies.Add("JoltCookingModule");
    }
}
