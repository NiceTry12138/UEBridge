// Copyright Epic Games, Inc. All Rights Reserved.

#include "Core/Utils/FJsonYamlWriter.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/PlatformFileManager.h"

FString FJsonYamlWriter::ToJsonString(const TSharedPtr<FJsonObject>& JsonObject)
{
	if (!JsonObject.IsValid())
	{
		return FString();
	}

	FString OutputString;
	TSharedRef<TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&OutputString);
	FJsonSerializer::Serialize(JsonObject.ToSharedRef(), Writer);
	return OutputString;
}

bool FJsonYamlWriter::WriteToFile(const TSharedPtr<FJsonObject>& JsonObject,
                                   const FString& FilePath,
                                   EOutputFormat Format)
{
	if (!JsonObject.IsValid())
	{
		return false;
	}

	// YAML is not yet implemented; fall back to JSON
	const FString Content = ToJsonString(JsonObject);
	if (Content.IsEmpty())
	{
		return false;
	}

	// Ensure directory exists
	const FString Directory = FPaths::GetPath(FilePath);
	if (!Directory.IsEmpty())
	{
		IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
		if (!PlatformFile.DirectoryExists(*Directory))
		{
			PlatformFile.CreateDirectoryTree(*Directory);
		}
	}

	return FFileHelper::SaveStringToFile(Content, *FilePath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
}
