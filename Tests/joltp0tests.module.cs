using PlasmaBuild.Core.Configuration;
using PlasmaBuild.Core.Rules;

public class JoltP0TestsModule : ModuleRules
{
    public JoltP0TestsModule(BuildContext context) : base(context)
    {
        PlasmaPackageSdk.ConfigurePackageModule(this, context, "Tests", "TestsPCH.h",
            "BUILDSYSTEM_BUILDING_JOLTP0TESTS_LIB");
        PlasmaPackageSdk.AddPackagePluginImport(this, context, "Source/JoltPlugin", "plJoltPlugin");
        PublicDependencies.Add("JoltModule");
    }
}
