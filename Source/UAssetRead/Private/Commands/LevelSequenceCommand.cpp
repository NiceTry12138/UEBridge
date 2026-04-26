// Copyright Epic Games, Inc. All Rights Reserved.
// Route: POST /dump_level_sequence
// Body: {"path":"/Game/Sequences/LS_Foo","include_keyframes":true}

#include "UAssetReadModule.h"

#include "HttpServerRequest.h"
#include "HttpServerResponse.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Async/Async.h"
#include "UObject/UObjectGlobals.h"

#include "LevelSequence.h"
#include "MovieScene.h"
#include "MovieSceneBinding.h"
#include "MovieSceneTrack.h"
#include "MovieSceneSection.h"
#include "Channels/MovieSceneChannelProxy.h"
#include "Channels/MovieSceneDoubleChannel.h"

#include "UAssetReadHelpers.h"

// ---------------------------------------------------------------------------
// Helper: dump keyframes from a double channel
// ---------------------------------------------------------------------------

static TSharedPtr<FJsonObject> DumpChannelKeys(
	FMovieSceneDoubleChannel* Channel,
	const FFrameRate& TickResolution)
{
	TSharedPtr<FJsonObject> ChObj = MakeShared<FJsonObject>();
	TArrayView<const FFrameNumber> Times = Channel->GetTimes();
	TArrayView<const FMovieSceneDoubleValue> Values = Channel->GetValues();

	TArray<TSharedPtr<FJsonValue>> KeysArray;
	for (int32 i = 0; i < Times.Num(); i++)
	{
		TSharedPtr<FJsonObject> KObj = MakeShared<FJsonObject>();
		KObj->SetNumberField(TEXT("time"), (double)Times[i].Value / TickResolution.AsDecimal());
		KObj->SetNumberField(TEXT("value"), Values[i].Value);
		KeysArray.Add(MakeShared<FJsonValueObject>(KObj));
	}
	ChObj->SetArrayField(TEXT("keys"), KeysArray);
	ChObj->SetNumberField(TEXT("key_count"), KeysArray.Num());
	return ChObj;
}

// ---------------------------------------------------------------------------
// Route handler
// ---------------------------------------------------------------------------

bool FUAssetReadModule::HandleDumpLevelSequence(
	const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
	TArray<uint8> BodyBytes = Request.Body;
	BodyBytes.Add(0);
	FString BodyStr = UTF8_TO_TCHAR(reinterpret_cast<const char*>(BodyBytes.GetData()));

	TSharedPtr<FJsonObject> BodyJson;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(BodyStr);
	if (!FJsonSerializer::Deserialize(Reader, BodyJson) || !BodyJson.IsValid())
	{
		OnComplete(MakeJsonError(EHttpServerResponseCodes::BadRequest, TEXT("Invalid JSON body")));
		return true;
	}

	FString AssetPath;
	if (!BodyJson->TryGetStringField(TEXT("path"), AssetPath) || AssetPath.IsEmpty())
	{
		OnComplete(MakeJsonError(EHttpServerResponseCodes::BadRequest, TEXT("Missing field: path")));
		return true;
	}

	bool bIncludeKeyframes = true;
	BodyJson->TryGetBoolField(TEXT("include_keyframes"), bIncludeKeyframes);

	TSharedRef<bool> AliveRef = bAlive;

	AsyncTask(ENamedThreads::GameThread,
		[AliveRef, AssetPath = MoveTemp(AssetPath), bIncludeKeyframes, OnComplete]() mutable
		{
			if (!*AliveRef) { return; }

			ULevelSequence* Sequence = LoadObject<ULevelSequence>(nullptr, *AssetPath);
			if (!Sequence)
			{
				OnComplete(MakeJsonError(EHttpServerResponseCodes::NoContent,
					FString::Printf(TEXT("Failed to load Level Sequence: %s"), *AssetPath)));
				return;
			}

			UMovieScene* MovieScene = Sequence->GetMovieScene();
			if (!MovieScene)
			{
				OnComplete(MakeJsonError(EHttpServerResponseCodes::ServerError,
					TEXT("Level Sequence has no MovieScene")));
				return;
			}

			TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetStringField(TEXT("sequence_path"), AssetPath);
			Result->SetStringField(TEXT("sequence_name"), Sequence->GetName());

			FFrameRate DisplayRate = MovieScene->GetDisplayRate();
			FFrameRate TickResolution = MovieScene->GetTickResolution();
			Result->SetNumberField(TEXT("display_fps"), DisplayRate.AsDecimal());
			Result->SetNumberField(TEXT("tick_resolution"), TickResolution.AsDecimal());

			TRange<FFrameNumber> PlaybackRange = MovieScene->GetPlaybackRange();
			if (PlaybackRange.HasLowerBound() && PlaybackRange.HasUpperBound())
			{
				double StartSeconds = (double)PlaybackRange.GetLowerBoundValue().Value / TickResolution.AsDecimal();
				double EndSeconds   = (double)PlaybackRange.GetUpperBoundValue().Value / TickResolution.AsDecimal();
				Result->SetNumberField(TEXT("start_time"), StartSeconds);
				Result->SetNumberField(TEXT("end_time"), EndSeconds);
				Result->SetNumberField(TEXT("duration"), EndSeconds - StartSeconds);
			}

			// Helper lambda to serialize tracks
			auto DumpTracks = [&](const TArray<UMovieSceneTrack*>& Tracks) -> TArray<TSharedPtr<FJsonValue>>
			{
				TArray<TSharedPtr<FJsonValue>> TracksArray;
				for (UMovieSceneTrack* Track : Tracks)
				{
					if (!Track) continue;
					TSharedPtr<FJsonObject> TrackObj = MakeShared<FJsonObject>();
					TrackObj->SetStringField(TEXT("name"), Track->GetDisplayName().ToString());
					TrackObj->SetStringField(TEXT("class"), Track->GetClass()->GetName());

					TArray<TSharedPtr<FJsonValue>> SectionsArray;
					for (UMovieSceneSection* Section : Track->GetAllSections())
					{
						if (!Section) continue;
						TSharedPtr<FJsonObject> SectionObj = MakeShared<FJsonObject>();
						SectionObj->SetStringField(TEXT("class"), Section->GetClass()->GetName());

						TRange<FFrameNumber> SectionRange = Section->GetRange();
						if (SectionRange.HasLowerBound())
						{
							SectionObj->SetNumberField(TEXT("start_time"),
								(double)SectionRange.GetLowerBoundValue().Value / TickResolution.AsDecimal());
						}
						if (SectionRange.HasUpperBound())
						{
							SectionObj->SetNumberField(TEXT("end_time"),
								(double)SectionRange.GetUpperBoundValue().Value / TickResolution.AsDecimal());
						}

						if (bIncludeKeyframes)
						{
							FMovieSceneChannelProxy& ChannelProxy = Section->GetChannelProxy();
							TArrayView<FMovieSceneDoubleChannel*> DoubleChannels = ChannelProxy.GetChannels<FMovieSceneDoubleChannel>();
							if (DoubleChannels.Num() > 0)
							{
								TArray<TSharedPtr<FJsonValue>> ChannelsArray;
								for (int32 ChIdx = 0; ChIdx < DoubleChannels.Num(); ChIdx++)
								{
									if (DoubleChannels[ChIdx]->GetTimes().Num() > 0)
									{
										TSharedPtr<FJsonObject> ChObj = DumpChannelKeys(DoubleChannels[ChIdx], TickResolution);
										ChObj->SetNumberField(TEXT("channel_index"), ChIdx);
										ChannelsArray.Add(MakeShared<FJsonValueObject>(ChObj));
									}
								}
								if (ChannelsArray.Num() > 0)
								{
									SectionObj->SetArrayField(TEXT("channels"), ChannelsArray);
								}
							}
						}

						SectionsArray.Add(MakeShared<FJsonValueObject>(SectionObj));
					}
					TrackObj->SetArrayField(TEXT("sections"), SectionsArray);
					TracksArray.Add(MakeShared<FJsonValueObject>(TrackObj));
				}
				return TracksArray;
			};

			// Bindings
			TArray<TSharedPtr<FJsonValue>> BindingsArray;
			const TArray<FMovieSceneBinding>& Bindings = MovieScene->GetBindings();
			for (const FMovieSceneBinding& Binding : Bindings)
			{
				TSharedPtr<FJsonObject> BindObj = MakeShared<FJsonObject>();
				BindObj->SetStringField(TEXT("name"), Binding.GetName());
				BindObj->SetStringField(TEXT("binding_id"), Binding.GetObjectGuid().ToString());

				FMovieScenePossessable* Possessable = MovieScene->FindPossessable(Binding.GetObjectGuid());
				if (Possessable)
				{
					BindObj->SetStringField(TEXT("binding_type"), TEXT("possessable"));
					BindObj->SetStringField(TEXT("possessed_class"),
						Possessable->GetPossessedObjectClass()
							? Possessable->GetPossessedObjectClass()->GetName() : TEXT("Unknown"));
				}
				else
				{
					FMovieSceneSpawnable* Spawnable = MovieScene->FindSpawnable(Binding.GetObjectGuid());
					if (Spawnable)
					{
						BindObj->SetStringField(TEXT("binding_type"), TEXT("spawnable"));
					}
				}

				BindObj->SetArrayField(TEXT("tracks"), DumpTracks(Binding.GetTracks()));
				BindingsArray.Add(MakeShared<FJsonValueObject>(BindObj));
			}
			Result->SetArrayField(TEXT("bindings"), BindingsArray);

			// Top-level tracks
			Result->SetArrayField(TEXT("master_tracks"), DumpTracks(MovieScene->GetMasterTracks()));

			// Camera cut track
			UMovieSceneTrack* CameraCut = MovieScene->GetCameraCutTrack();
			if (CameraCut)
			{
				TArray<UMovieSceneTrack*> CutArray = { CameraCut };
				Result->SetArrayField(TEXT("camera_cut_track"), DumpTracks(CutArray));
			}

			FString OutStr;
			TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutStr);
			FJsonSerializer::Serialize(Result.ToSharedRef(), Writer);

			OnComplete(MakeJsonResponse(OutStr));
		});

	return true;
}
