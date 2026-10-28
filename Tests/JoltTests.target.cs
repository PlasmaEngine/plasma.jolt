using PlasmaBuild.Core.Configuration;
using PlasmaBuild.Core.Rules;

public class JoltTestsTarget : TargetRules
{
    public JoltTestsTarget(BuildContext context) : base(context)
    {
        Type = TargetType.Executable;
        OutputDirectory = PlasmaPackageSdk.PackageBinaryDirectory(context);
        ExtraModules.Add("JoltTestsModule");
        TargetDependencies.Add("JoltPlugin");
    }
}
