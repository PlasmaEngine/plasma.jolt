using PlasmaBuild.Core.Configuration;
using PlasmaBuild.Core.Rules;

public class JoltModule : ModuleRules
{
    public JoltModule(BuildContext context) : base(context)
    {
        Type = ModuleType.ThirdParty;
        SourceDirectory = "ThirdParty/Jolt";

        // Jolt has no dllexport annotations, so its JPH symbols are never exported
        // from a DLL. Keep it folded (compiled) into every consumer rather than
        // importing it across a DLL boundary.
        ExportsSymbols = false;

        PublicIncludePaths.Add("..");
        PublicIncludePaths.Add(".");
        PublicDefinitions.Add("BUILDSYSTEM_ENABLE_JOLT_SUPPORT");
        PublicDefinitions.Add("JPH_DEBUG_RENDERER");

        CppStandard = CppStandard.Cpp20;
        TreatWarningsAsErrors = false;
    }
}