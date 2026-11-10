using PlasmaBuild.Core.Configuration;
using PlasmaBuild.Core.Rules;

public class JoltTestTarget : TargetRules
{
    public JoltTestTarget(BuildContext context) : base(context)
    {
        Type = TargetType.Executable;
        OutputDirectory = PlasmaPackageSdk.PackageBinaryDirectory(context);
        ExtraModules.Add("JoltTestModule");
        TargetDependencies.Add("JoltPlugin");
    }
}
