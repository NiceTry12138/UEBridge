// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

class UEdGraph;
class UEdGraphNode;
class UEdGraphPin;

/**
 * Utility helpers for converting Blueprint UEdGraph objects into
 * the unified Graph Schema JSON described in the README.
 */
class FBlueprintGraphUtils
{
public:
	/** Convert a full UEdGraph (function, event, macro) to a graph JSON object. */
	static TSharedPtr<FJsonObject> ExportGraph(UEdGraph* Graph, const FString& GraphType);

private:
	static TSharedPtr<FJsonObject> ExportNode(UEdGraphNode* Node);
	static TSharedPtr<FJsonObject> ExportPin(UEdGraphPin* Pin);
	static TSharedPtr<FJsonObject> BuildNodeExtra(UEdGraphNode* Node);
	static TSharedPtr<FJsonObject> BuildPinType(UEdGraphPin* Pin);
};
