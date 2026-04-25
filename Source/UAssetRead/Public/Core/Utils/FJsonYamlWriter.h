// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

/**
 * Serializes FJsonObject to JSON (or YAML stub) string and writes to disk or returns as string.
 */
class FJsonYamlWriter
{
public:
	enum class EOutputFormat : uint8
	{
		Json,
		Yaml,  // Not yet implemented; falls back to JSON
	};

	/** Serialize a JSON object to a pretty-printed JSON string. */
	static FString ToJsonString(const TSharedPtr<FJsonObject>& JsonObject);

	/**
	 * Write a JSON object to a file on disk.
	 * Creates intermediate directories as needed.
	 * Returns true on success.
	 */
	static bool WriteToFile(const TSharedPtr<FJsonObject>& JsonObject,
	                        const FString& FilePath,
	                        EOutputFormat Format = EOutputFormat::Json);
};
