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

			// the piece menu is Slate in C++, not a WidgetBlueprint
			"UMG",
			"Slate",
			"SlateCore",

			// physics and destruction
			"PhysicsCore",
			"Chaos",
			"ChaosCore",
			"GeometryCollectionEngine",
			"FieldSystemEngine",

			// debris / dust effects
			"Niagara"
		});

		PrivateDependencyModuleNames.AddRange(new string[] {
			// Content tests read package headers through the asset registry, loading nothing
			"AssetRegistry"
		});

		PublicIncludePaths.AddRange(new string[] {
			"DestructionGame"
		});
	}
}
