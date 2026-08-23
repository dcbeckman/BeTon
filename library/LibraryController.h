#ifndef BETON_LIBRARY_CONTROLLER_H
#define BETON_LIBRARY_CONTROLLER_H

#include <Message.h>
#include <MessageRunner.h>
#include <String.h>

class MainWindow;

/**
 * @brief Coordinates library-scanning and cache update flow for the main window.
 *
 * This controller bridges asynchronous cache/scan messages to UI updates,
 * keeps the in-memory media index in sync, and triggers view refreshes.
 */
class LibraryController {
public:
  /**
   * @brief Creates a library controller bound to the given main window.
   * @param window Owning window context.
   */
  LibraryController(MainWindow* window);

  /**
   * @brief Destroys the controller.
   */
  ~LibraryController();

  /**
   * @brief Opens the music-source manager dialog.
   */
  void ShowDirectoryManager();

  /**
   * @brief Reveals one or more files in Tracker.
   * @param msg Message containing `refs` or a nested `files` message with refs.
   */
  void RevealInTracker(BMessage* msg);

  /**
   * @brief Moves a file on disk and updates library state on success.
   * @param msg Message with "from" and "to" absolute path strings.
   * On failure nothing changes and the error is shown in the status bar.
   */
  void HandleFileMove(BMessage* msg);

  /**
   * @brief Handles initial cache-load completion and refreshes views.
   */
  void HandleCacheLoaded();

  /**
   * @brief Rebuilds the library after the cache file was found to be corrupt.
   *
   * The cache looper has already moved the bad file aside and reported
   * whatever it could salvage from it. This kicks off a rescan of every
   * configured source and sidebar folder, plus an explicit scan of the tracks
   * referenced by saved playlists, so the gaps get filled in.
   *
   * If nothing was salvageable the views are cleared first; when entries were
   * recovered they are kept, because the scan fast-skips those files and will
   * not re-report them.
   *
   * @param msg Message carrying the "recovered"/"declared" counts and the
   *            "backup" path of the file that was set aside.
   */
  void HandleCacheCorrupt(BMessage *msg);

  /**
   * @brief Rebuilds the library when the cache loaded but held no entries.
   *
   * Covers a cache file that is missing entirely as well as one that reads
   * back with zero records. Nothing else at startup would notice, so a user
   * with configured folders or saved playlists would be left staring at an
   * empty library until they rescanned by hand. Does nothing when there is
   * genuinely nothing configured to scan.
   *
   * @param msg Message carrying the configured source-directory count.
   */
  void HandleCacheEmpty(BMessage *msg);

  /**
   * @brief Starts a full media rescan and clears visible list views.
   */
  void StartFullRescan();

  /**
   * @brief Takes a new aggregated progress report from the cache.
   * @param msg Message with folder/playlist scope, file counts and elapsed time.
   */
  void UpdateScanProgress(BMessage* msg);

  /**
   * @brief Redraws the scan status from the last report.
   *
   * Driven by a one-second timer so the clock keeps moving between reports and
   * the trailing time label can alternate on schedule.
   */
  void TickScanStatus();

  /**
   * @brief Finalizes rescan state and reloads library entries from cache.
   * @param msg Scan-done message with elapsed time metadata.
   */
  void HandleScanDone(BMessage* msg);

  /**
   * @brief Processes a cache-loading batch timer tick.
   */
  void HandleBatchTimer();

  /**
   * @brief Applies a batch of media item updates from cache/scanner.
   * @param msg Batch message containing repeated item fields.
   */
  void HandleMediaBatch(BMessage* msg);

  /**
   * @brief Performs a debounced partial refresh of filtered list views.
   */
  void RefreshPartialViews();

  /**
   * @brief Applies an incremental single-item metadata update.
   * @param msg Message containing item path and updated fields.
   */
  void HandleMediaItemFound(BMessage* msg);

  /**
   * @brief Removes a media item from views and in-memory index.
   * @param msg Message containing the item path.
   */
  void HandleMediaItemRemoved(BMessage* msg);

  /**
   * @brief Rebuilds the path-to-index lookup for `fAllItems`.
   */
  void RebuildPathIndex();

private:
  /**
   * @brief Scans every configured source, sidebar folder and saved playlist.
   *
   * Shared by the corrupt- and empty-cache paths. Playlist tracks are queued
   * by path on top of the folder scan, because a playlist may reference files
   * that live outside every scanned folder.
   */
  void _StartLibraryRebuild();

  /**
   * @brief Tells the user their cache was damaged and what is being done.
   *
   * Shown asynchronously: the rebuild is already running and the window must
   * stay responsive behind the alert.
   *
   * @param recovered Entries salvaged from the damaged file (may be 0).
   * @param declared Entry count the damaged file claimed to hold.
   * @param backupPath Where the damaged file was kept, empty if it was not.
   */
  void _ShowCacheDamagedAlert(int32 recovered, int32 declared,
                              const BString &backupPath);

  /**
   * @brief Counts sidebar entries that stand for a collection of files.
   * @return Number of Folder and Playlist items in the sidebar.
   */
  int32 _CountSidebarCollections() const;

  /**
   * @brief Renders the scan status line from the stored snapshot.
   *
   * The trailing label alternates between elapsed and estimated remaining
   * time every five seconds, so both are available without needing room for
   * both at once.
   */
  void _RenderScanStatus();

  /** @brief Starts the one-second status timer if it is not already running. */
  void _StartScanStatusTimer();

  /** @brief Stops the status timer once a scan cycle is over. */
  void _StopScanStatusTimer();

  /** @brief Owning main window and shared state access. */
  MainWindow* fWindow;

  /** @name Scan status snapshot */
  ///@{
  /** @brief Folders holding work in the running scan. */
  int32 fScanFolders{0};
  /** @brief Playlists holding work in the running scan. */
  int32 fScanPlaylists{0};
  /** @brief Files scanned so far. */
  int32 fScanFilesDone{0};
  /** @brief Files this scan has to get through. */
  int32 fScanFilesTotal{0};
  /** @brief When the running scan started, for the live elapsed clock. */
  bigtime_t fScanStartTime{0};
  /** @brief Repeating one-second tick, or NULL when no scan is running. */
  BMessageRunner* fScanStatusRunner{nullptr};
  ///@}
};

#endif // BETON_LIBRARY_CONTROLLER_H
