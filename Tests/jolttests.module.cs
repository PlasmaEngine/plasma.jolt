using PlasmaBuild.Core.Configuration;
using PlasmaBuild.Core.Rules;

public class JoltTestsModule : ModuleRules
{
    public JoltTestsModule(BuildContext context) : base(context)
    {
        PlasmaPackageSdk.ConfigurePackageModule(this, context, "Tests", "TestsPCH.h",
            "BUILDSYSTEM_BUILDING_JOLTTESTS_LIB");
        PlasmaPackageSdk.AddPackagePluginImport(this, context, "Source/JoltPlugin", "plJoltPlugin");
        PublicDependencies.Add("JoltModule");
    }
}
