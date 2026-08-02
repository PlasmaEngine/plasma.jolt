using PlasmaBuild.Core.Configuration;
using PlasmaBuild.Core.Rules;

public class JoltCookingModule : ModuleRules
{
    public JoltCookingModule(BuildContext context) : base(context)
    {
        PlasmaPackageSdk.ConfigurePackageModule(this, context, "Source/JoltCooking", "JoltCookingPCH.h",
            "BUILDSYSTEM_BUILDING_JOLTCOOKING_LIB");

        PublicDependencies.Add("JoltModule");
    }
}
