// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"
#include "HttpRouteHandle.h"
#include "HttpResultCallback.h"

class IHttpRouter;
struct FHttpServerRequest;

// ---------------------------------------------------------------------------
// Asset loaded from disk and kept alive in the LRU cache.
// The object is AddToRoot'd to prevent GC; we explicitly RemoveFromRoot on eviction.
// ---------------------------------------------------------------------------
struct FCachedAsset
{
	UObject* Asset         = nullptr;
	double   LastAccessTime = 0.0;
	bool     bWasRooted    = false; // true = we called AddToRoot, we must call RemoveFromRoot
};

// ---------------------------------------------------------------------------

class FUAssetReadModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

private:
	// ---- Menu extensions ---------------------------------------------------
	void RegisterMenuExtensions();
	void UnregisterMenuExtensions();

	// ---- HTTP server -------------------------------------------------------
	void StartHttpServer();
	void StopHttpServer();

	bool HandleDumpAsset (const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);
	bool HandleListAssets(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);
	bool HandleHealth    (const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);

	TSharedPtr<IHttpRouter>    HttpRouter;
	TArray<FHttpRouteHandle>   RouteHandles;
	static constexpr uint32    HttpPort = 8765;

	// ---- Asset cache (game-thread only) ------------------------------------
	UObject* GetOrLoadAsset(const FString& AssetPath);
	void     PruneCache();
	void     EvictCacheEntry(FCachedAsset& Entry);

	TMap<FString, FCachedAsset> AssetCache;
	static constexpr int32  MaxCacheEntries  = 20;
	static constexpr double CacheTimeoutSecs = 300.0;

	// ---- Shutdown guard (shared with AsyncTask lambdas) --------------------
	// Lambdas capture a copy; checking *bAlive avoids use-after-free if the
	// module is torn down before a queued game-thread task runs.
	TSharedRef<bool> bAlive{ MakeShared<bool>(true) };
};
