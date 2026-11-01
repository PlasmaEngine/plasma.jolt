using System.IO;
using PlasmaBuild.Core.Configuration;
using PlasmaBuild.Core.Rules;

public class JoltTestModule : ModuleRules
{
    public JoltTestModule(BuildContext context) : base(context)
    {
        PlasmaPackageSdk.ConfigurePackageModule(this, context, "Tests", "TestsPCH.h",
            "BUILDSYSTEM_BUILDING_JOLTTEST_LIB");
        PlasmaPackageSdk.AddPackagePluginImport(this, context, "Source/JoltPlugin", "plJoltPlugin");
        PublicDependencies.Add("JoltModule");

        // The engine's test framework: headers, its platform entry point and the library the SDK builds next to the engine.
        var unitTests = Path.Combine(PlasmaPackageSdk.Root, "Code", "UnitTests");
        PublicIncludePaths.Add(unitTests);
        PublicIncludePaths.Add(Path.Combine(unitTests, "TestFramework", "Platform",
            context.Platform == TargetPlatform.Windows ? "Win" : "Linux"));
        PlasmaPackageSdk.AddImportLibrary(this, context, "plTestFramework");
    }
}
