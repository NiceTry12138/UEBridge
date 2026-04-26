// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class UAssetRead : ModuleRules
{
	public UAssetRead(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"Json",
			"JsonUtilities",
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"Slate",
			"SlateCore",
			"ApplicationCore",    // FPlatformApplicationMisc::ClipboardCopy
			"UnrealEd",           // UCommandlet, UBlueprint, asset registry
			"BlueprintGraph",     // K2Node types, UEdGraph
			"KismetCompiler",     // Blueprint compilation helpers
			"UMG",                // UWidget, UWidgetTree
			"UMGEditor",          // UWidgetBlueprint
			"RenderCore",         // FStaticMeshRenderData
			"RHI",                // EPixelFormat
			"AssetRegistry",      // IAssetRegistry
			"AssetTools",         // IAssetTypeActions
			"ContentBrowser",     // UContentBrowserAssetContextMenuContext
			"ContentBrowserData",
			"ToolMenus",          // UToolMenus
			"PhysicsCore",        // UBodySetup
			"HTTPServer",         // FHttpServerModule, IHttpRouter
			// New dependencies for extended commands
			"Niagara",            // UNiagaraSystem, UNiagaraEmitterHandle
			"NiagaraCore",        // Niagara core types
			"NiagaraEditor",      // UNiagaraScriptSource, UNiagaraGraph, NiagaraNodeFunctionCall
			"LevelSequence",      // ULevelSequence
			"MovieScene",         // UMovieScene, FMovieSceneBinding
			"MovieSceneTracks",   // UMovieSceneTrack subclasses
			"AnimGraph",          // Animation graph nodes
			"AnimGraphRuntime",   // Animation blueprint runtime
		});
	}
}
