// #pl-version 1

using PlasmaBuild.Core.Configuration;
using PlasmaBuild.Core.Rules;

public class PlasmaJoltWorkspace : WorkspaceRules
{
    public PlasmaJoltWorkspace(BuildContext context) : base(context)
    {
        TargetNames.Add("JoltPlugin");
        TargetNames.Add("EnginePluginJolt");
        TargetNames.Add("EditorPluginJolt");
        TargetNames.Add("JoltTests");
    }
}
