using PlasmaBuild.Core.Configuration;
using PlasmaBuild.Core.Rules;

public class JoltP0TestsTarget : TargetRules
{
    public JoltP0TestsTarget(BuildContext context) : base(context)
    {
        Type = TargetType.Executable;
        OutputDirectory = PlasmaPackageSdk.PackageBinaryDirectory(context);
        ExtraModules.Add("JoltP0TestsModule");
        TargetDependencies.Add("JoltPlugin");
    }
}
