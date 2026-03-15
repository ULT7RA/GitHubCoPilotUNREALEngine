// Copyright GitHub, Inc. All Rights Reserved.

using UnrealBuildTool;

public class GitHubCopilotUERuntime : ModuleRules
{
	public GitHubCopilotUERuntime(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"Json",
			"JsonUtilities"
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
		});
	}
}
