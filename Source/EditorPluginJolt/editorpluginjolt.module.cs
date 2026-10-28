using PlasmaBuild.Core.Configuration;
using PlasmaBuild.Core.Rules;

public class EditorPluginJoltModule : ModuleRules
{
    public EditorPluginJoltModule(BuildContext context) : base(context)
    {
        PlasmaPackageSdk.ConfigurePackageModule(this, context, "Source/EditorPluginJolt",
            "EditorPluginJoltPCH.h", "BUILDSYSTEM_BUILDING_EDITORPLUGINJOLT_LIB");

        PlasmaPackageSdk.ConfigureEditorSdkConsumer(this, context);
        PlasmaPackageSdk.AddPackagePluginImport(this, context, "Source/JoltPlugin", "plJoltPlugin");
        PlasmaPackageSdk.AddPackagePluginImport(this, context, "Source/EnginePluginJolt", "plEnginePluginJolt");

        PublicDependencies.Add("JoltCookingModule");

        // The project settings content is engine-drawn: EditorUi and the retained UI runtime.
        PlasmaPackageSdk.AddImportLibrary(this, context, "plEditorUi");
        PlasmaPackageSdk.AddImportLibrary(this, context, "plGameUiPlugin");
    }
}
