// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class DestructionGame : ModuleRules
{
	public DestructionGame(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[] {
			"Core",
			"CoreUObject",
			"Engine",
			"InputCore",
			"EnhancedInput",
			"UMG",

			// physics and destruction
			"PhysicsCore",
			"Chaos",
			"ChaosCore",
			"GeometryCollectionEngine",
			"FieldSystemEngine",

			// debris / dust effects
			"Niagara"
		});

		PrivateDependencyModuleNames.AddRange(new string[] { });

		PublicIncludePaths.AddRange(new string[] {
			"DestructionGame"
		});
	}
}
