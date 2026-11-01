using PlasmaBuild.Core.Configuration;
using PlasmaBuild.Core.Rules;

public class JoltPluginModule : ModuleRules
{
    public JoltPluginModule(BuildContext context) : base(context)
    {
        PlasmaPackageSdk.ConfigurePackageModule(this, context, "Source/JoltPlugin", "JoltPluginPCH.h",
            "BUILDSYSTEM_BUILDING_JOLTPLUGIN_LIB");

        // Jolt itself, vendored in this package. It exports no symbols, so it is compiled into
        // every consumer rather than imported across a DLL boundary.
        PublicDependencies.Add("JoltModule");

        // Jolt is folded into the plugin, and a host that also contains Jolt (the headless tests) would otherwise take over the
        // plugin's calls into it on Linux and give them its own copy of Jolt's global state.
        if (context.Platform == TargetPlatform.Linux)
        {
            AdditionalLinkerFlags.Add("-Wl,-Bsymbolic");
        }
    }
}
