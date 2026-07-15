#ifndef BETON_MEDIA_LIBRARY_CACHE_H
#define BETON_MEDIA_LIBRARY_CACHE_H

#include "MediaItem.h"
#include "Messages.h"
#include <Looper.h>
#include <MessageRunner.h>
#include <Messenger.h>
#include <String.h>
#include <Query.h>
#include <Volume.h>
#include <map>
#include <set>
#include <vector>

/**
 * @class MediaLibraryCache
 * @brief Manages the central media library cache.
 *
 * The MediaLibraryCache is responsible for:
 * - Loading and saving the 'media.cache' file.
 * - Coordinating the scanning process (via MediaLibraryScanner).
 * - Maintaining the in-memory state of all known media files (fEntries).
 * - Notifying the UI about progress and updates.
 *
 * It runs as a BLooper to handle asynchronous messages.
 */
class MediaLibraryCache : public BLooper {
public:
  /**
   * @brief Construct a new Cache Manager object
   *
   * @param target The target messenger (usually MainWindow) to receive
   * notifications.
   */
  MediaLibraryCache(const BMessenger &target);

  /**
   * @brief Saves pending cache changes and releases live-query resources.
   */
  ~MediaLibraryCache() override;

  /**
   * @brief Loads the cache from disk.
   */
  void LoadCache();

  /**
   * @brief Saves the current cache to disk.
   */
  void SaveCache();

  /**
   * @brief Starts the scanning process for all configured directories.
   */
  void StartScan();

  /**
   * @brief Scans a single directory path asynchronously.
   * @param dirPath Directory path to scan.
   */
  void ScanPath(const BString &dirPath);

  /**
   * @brief Starts watching a folder and its subdirectories recursively for updates.
   * @param folderPath Directory path to watch.
   */
  void StartWatchingFolder(const BString &folderPath);

  /**
   * @brief Stops watching all folders/files currently watched.
   */
  void StopWatchingFolder();

  /**
   * @brief Handles cache/scanner/query messages on the looper thread.
   * @param msg Incoming message.
   */
  void MessageReceived(BMessage *msg) override;

  /**
   * @brief Returns the internal path-indexed cache map.
   */
  const std::map<BString, MediaItem> &Entries() const { return fEntries; }

  /**
   * @brief Returns a flattened vector of all media items.
   * Useful for UI population.
   */
  std::vector<MediaItem> AllEntries() const;

private:
  /**
   * @brief Inserts or replaces an item in the cache.
   * @param entry Item to store.
   */
  void AddOrUpdateEntry(const MediaItem &entry);

  /**
   * @brief Loads configured source directories into `outDirs`.
   * @param outDirs Output vector with absolute directory paths.
   */
  void LoadDirectories(std::vector<BString> &outDirs);

  /**
   * @brief Marks all entries below a base path as currently unavailable.
   * @param basePath Root path that is offline.
   */
  void MarkBaseOffline(const BString &basePath);

  /** @name Data */
  ///@{
  /** @brief Path-indexed media entries (`key == MediaItem::path`). */
  std::map<BString, MediaItem> fEntries;
  /** @brief UI/update target messenger (usually `MainWindow`). */
  BMessenger fTarget;
  /** @brief Absolute path of the on-disk cache file. */
  BString fCachePath;
  /** @brief Number of currently active scanner loopers. */
  int32 fActiveScanners{0};
  bool fCacheDirty{false}; ///< Set when entries changed, cleared after SaveCache()
  
  /** @brief Active rating live queries (owned pointers). */
  std::vector<BQuery*> fRatingQueries;
  /** @brief Volumes that already have initialized rating live queries. */
  std::set<dev_t> fQueriedVolumes;
  ///@}

  /**
   * @brief Initializes rating live queries for all configured source volumes.
   */
  void _InitAllLiveQueries();

  /**
   * @brief Initializes per-rating live queries on one BFS-capable volume.
   * @param device Device identifier.
   */
  void _InitRatingLiveQueries(dev_t device);

  /**
   * @brief Re-reads BFS metadata attributes and updates an item in-place.
   * @param item Item to refresh.
   * @return `true` if at least one attribute changed.
   */
  bool _RereadBfsAttributes(MediaItem &item);

  void _WatchDirRecursive(BDirectory &dir);
  void _HandleNodeMonitor(BMessage *msg, int32 opcode);
  void _ScanAndAddFile(const BString &pathStr);
  void _RemoveFileFromCache(const BString &pathStr);
  static bool _IsSupportedAudioFile(const BString &path);

  /**
   * @brief Marks the cache dirty and throttles the write-back to disk.
   *
   * Node-monitor events can arrive in bursts (e.g. dropping hundreds of files
   * into a watched folder). Writing the whole cache per event is O(n) each
   * time, so instead arm a one-shot runner and coalesce: at most one SaveCache()
   * per kSaveThrottleDelay while events keep coming.
   */
  void _ScheduleSave();

  /**
   * @brief Drops cache entries under `folderPath` whose file no longer exists.
   *
   * The scanner only reports files it finds, so deletions that happened while
   * the folder was not being watched would otherwise linger until a full
   * rescan. Called on folder entry, before the catch-up scan.
   * @param folderPath Folder root to reconcile.
   */
  void _ReconcileFolderDeletions(const BString &folderPath);

  std::vector<node_ref> fWatchedNodes;
  /** @brief Armed one-shot save runner, or NULL when no save is pending. */
  BMessageRunner *fSaveThrottle{nullptr};
};

#endif // BETON_MEDIA_LIBRARY_CACHE_H
