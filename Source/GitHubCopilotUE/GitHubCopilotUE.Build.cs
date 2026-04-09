// Copyright GitHub, Inc. All Rights Reserved.

using UnrealBuildTool;

public class GitHubCopilotUE : ModuleRules
{
public GitHubCopilotUE(ReadOnlyTargetRules Target) : base(Target)
{
PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;
PrecompileForTargets = PrecompileTargetsType.Editor;

PublicDependencyModuleNames.AddRange(new string[]
{
"Core",
"CoreUObject",
"Engine",
"Slate",
"SlateCore",
"InputCore",
"Json",
"JsonUtilities",
"HTTP",
"Projects",
"ToolMenus",
"LiveCoding"
});

PrivateDependencyModuleNames.AddRange(new string[]
{
"UnrealEd",
"LevelEditor",
"AssetRegistry",
"ContentBrowser",
"WorkspaceMenuStructure",
"DeveloperSettings",
"EditorSubsystem",
"DesktopPlatform",
"SourceControl",
"GameProjectGeneration"
});

// UE 5.1+ uses EditorFramework for some toolbar APIs.
// If targeting < 5.1, you may need to remove or guard this.
PrivateDependencyModuleNames.Add("EditorFramework");
PrivateDependencyModuleNames.Add("ApplicationCore");
PrivateDependencyModuleNames.Add("OutputLog");
PrivateDependencyModuleNames.Add("ImageWrapper");
}
}
