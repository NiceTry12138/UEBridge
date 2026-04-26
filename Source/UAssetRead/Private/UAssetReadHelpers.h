// Copyright Epic Games, Inc. All Rights Reserved.
#pragma once

#include "HttpServerResponse.h"
#include "Dom/JsonObject.h"

/**
 * Shared JSON response helpers used by UAssetReadModule and all Command .cpp files.
 * Include this header in any Command .cpp that needs MakeJsonResponse / MakeJsonError.
 * The functions are inline to avoid ODR violations across translation units.
 */

inline TUniquePtr<FHttpServerResponse> MakeJsonResponse(
	const FString& Body,
	EHttpServerResponseCodes Code = EHttpServerResponseCodes::Ok)
{
	auto Response = FHttpServerResponse::Create(Body, TEXT("application/json; charset=utf-8"));
	Response->Code = Code;
	Response->Headers.Add(TEXT("Access-Control-Allow-Origin"), { TEXT("*") });
	return Response;
}

inline TUniquePtr<FHttpServerResponse> MakeJsonError(
	EHttpServerResponseCodes Code, const FString& Message)
{
	FString Body = FString::Printf(TEXT("{\"error\":\"%s\"}"), *Message.ReplaceCharWithEscapedChar());
	return MakeJsonResponse(Body, Code);
}
