// Copyright Epic Games, Inc. All Rights Reserved.

#include "Core/Exporters/FMediaExporter.h"

#include "Sound/SoundWave.h"
#include "Engine/Texture2D.h"
#include "PixelFormat.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

TSharedPtr<FJsonObject> FMediaExporter::Export(UObject* Asset)
{
	// ---- USoundWave --------------------------------------------------------
	if (USoundWave* Sound = Cast<USoundWave>(Asset))
	{
		TSharedPtr<FJsonObject> Root = MakeShareable(new FJsonObject);
		Root->SetStringField(TEXT("assetPath"), Sound->GetPathName());
		Root->SetStringField(TEXT("assetType"), TEXT("SoundWave"));

		FResourceSizeEx ResSize(EResourceSizeMode::Exclusive);
		Sound->GetResourceSizeEx(ResSize);
		Root->SetNumberField(TEXT("sizeBytes"), static_cast<double>(ResSize.GetTotalMemoryBytes()));
		Root->SetNumberField(TEXT("duration"), Sound->GetDuration());
		Root->SetNumberField(TEXT("sampleRate"), Sound->GetSampleRateForCurrentPlatform());
		Root->SetNumberField(TEXT("numChannels"), Sound->NumChannels);
		return Root;
	}

	// ---- UTexture2D --------------------------------------------------------
	if (UTexture2D* Texture = Cast<UTexture2D>(Asset))
	{
		TSharedPtr<FJsonObject> Root = MakeShareable(new FJsonObject);
		Root->SetStringField(TEXT("assetPath"), Texture->GetPathName());
		Root->SetStringField(TEXT("assetType"), TEXT("Texture2D"));

		FResourceSizeEx ResSize(EResourceSizeMode::Exclusive);
		Texture->GetResourceSizeEx(ResSize);
		Root->SetNumberField(TEXT("sizeBytes"), static_cast<double>(ResSize.GetTotalMemoryBytes()));
		Root->SetNumberField(TEXT("width"), Texture->GetSizeX());
		Root->SetNumberField(TEXT("height"), Texture->GetSizeY());

		EPixelFormat PixFmt = Texture->GetPixelFormat();
		Root->SetStringField(TEXT("format"),
			(PixFmt < PF_MAX) ? GPixelFormats[PixFmt].Name : TEXT("Unknown"));

		const int32 NumMips = Texture->GetNumMips();
		Root->SetBoolField(TEXT("hasMips"), NumMips > 1);
		Root->SetNumberField(TEXT("mipCount"), NumMips);
		return Root;
	}

	return nullptr;
}
