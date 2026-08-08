#include "MediaLibraryCache.h"
#include "Config.h"
#include "MetadataTagIO.h"
#include <NodeMonitor.h>
#include "Debug.h"
#include "MediaLibraryScanner.h"
#include "Messages.h"
#include "MusicSourceSettings.h"
#include <Directory.h>
#include <Entry.h>
#include <File.h>
#include <FindDirectory.h>
#include <Node.h>
#include <OS.h>
#include <Path.h>
#include <algorithm>
#include <set>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

/** @brief Header magic of the binary cache format. */
const uint32 kCacheMagic = 'BTCA';
/** @brief Cache format version this build writes and is able to read. */
const uint32 kCacheVersion = 2;

/**
 * @brief Smallest possible on-disk record.
 *
 * Thirteen zero-length strings (length word only) plus the fixed numeric
 * block. Used to sanity-check the header's entry count against the real file
 * size before anything is allocated from it.
 */
const off_t kMinRecordSize = 13 * (off_t)sizeof(uint32) +
                             10 * (off_t)sizeof(int32) +
                             3 * (off_t)sizeof(int64) + (off_t)sizeof(uint8);

/** @brief Longest string accepted from a cache record. */
const uint32 kMaxCacheStringLength = 64 * 1024;

/**
 * @brief Bounds-checked sequential reader for the binary cache format.
 *
 * Every read is validated against the real file size before it happens, so a
 * truncated or garbled file fails the load instead of driving a bogus length
 * into BString::LockBuffer().
 */
class CacheReader {
public:
  CacheReader(BFile &file, off_t size) : fFile(file), fSize(size) {}

  bool ReadRaw(void *out, size_t size) {
    if (!fOk)
      return false;
    if ((off_t)size > Remaining() || fFile.Read(out, size) != (ssize_t)size) {
      fOk = false;
      return false;
    }
    fPos += size;
    return true;
  }

  template <typename T> bool Read(T &value) {
    return ReadRaw(&value, sizeof(value));
  }

  bool ReadString(BString &out) {
    uint32 len = 0;
    if (!Read(len))
      return false;
    if (len > kMaxCacheStringLength || (off_t)len > Remaining()) {
      fOk = false;
      return false;
    }
    if (len == 0) {
      out.Truncate(0);
      return true;
    }
    char *buf = out.LockBuffer(len + 1);
    if (buf == NULL) {
      fOk = false;
      return false;
    }
    if (fFile.Read(buf, len) != (ssize_t)len) {
      out.UnlockBuffer(0);
      fOk = false;
      return false;
    }
    buf[len] = '\0';
    out.UnlockBuffer(len);
    fPos += len;
    return true;
  }

  /** @brief Repositions the reader, clearing any earlier failure. */
  bool SeekTo(off_t pos) {
    if (pos < 0 || pos > fSize || fFile.Seek(pos, SEEK_SET) != pos)
      return false;
    fPos = pos;
    fOk = true;
    return true;
  }

  off_t Position() const { return fPos; }
  off_t Remaining() const { return fSize - fPos; }
  bool AtEnd() const { return fPos == fSize; }
  bool IsOk() const { return fOk; }

private:
  BFile &fFile;
  off_t fSize;
  off_t fPos{0};
  bool fOk{true};
};

/** @brief Longest path accepted when hunting for a record boundary. */
const uint32 kMaxPathLength = 1024; // B_PATH_NAME_LENGTH

/** @brief How far past damage to look for the next intact record. */
const off_t kMaxResyncScan = 64 * 1024 * 1024;

/** @brief Candidate boundaries probed before giving up on one damaged spot. */
const int kMaxResyncProbes = 64;

/**
 * @brief Reads one complete record.
 *
 * @return false if the record is short, has an implausible field, or does not
 *         start with an absolute path. Entries are keyed by path and every
 *         path written came from BPath, so a non-absolute one means the
 *         record boundaries have drifted and the bytes cannot be trusted.
 */
bool ReadRecord(CacheReader &reader, MediaItem &e) {
  if (!reader.ReadString(e.path) || !reader.ReadString(e.base) ||
      !reader.ReadString(e.title) || !reader.ReadString(e.artist) ||
      !reader.ReadString(e.album) || !reader.ReadString(e.albumArtist) ||
      !reader.ReadString(e.genre) || !reader.ReadString(e.comment) ||
      !reader.ReadString(e.composer) || !reader.ReadString(e.mbTrackId) ||
      !reader.ReadString(e.mbAlbumId) || !reader.ReadString(e.mbArtistId) ||
      !reader.ReadString(e.acoustId))
    return false;

  if (!reader.Read(e.year) || !reader.Read(e.track) ||
      !reader.Read(e.trackTotal) || !reader.Read(e.disc) ||
      !reader.Read(e.discTotal) || !reader.Read(e.duration) ||
      !reader.Read(e.bitrate) || !reader.Read(e.sampleRate) ||
      !reader.Read(e.channels) || !reader.Read(e.rating) ||
      !reader.Read(e.size) || !reader.Read(e.mtime) || !reader.Read(e.inode))
    return false;

  uint8 flags = 0;
  if (!reader.Read(flags))
    return false;
  e.missing = (flags & 1) != 0;

  return !e.path.IsEmpty() && e.path.ByteAt(0) == '/';
}

/**
 * @brief Scans forward for the next byte offset that looks like a record.
 *
 * A record opens with a path length word followed by that many bytes of an
 * absolute path, which is a distinctive enough shape to lock onto: zero fill
 * and most garbage fail the length range immediately.
 *
 * @return Candidate offset, or -1 if none was found within kMaxResyncScan.
 */
off_t FindRecordStart(BFile &file, off_t from, off_t fileSize) {
  const off_t limit = std::min(fileSize, from + kMaxResyncScan);
  const size_t kWindow = 256 * 1024;
  // Overlap so a candidate straddling the window edge is still fully visible.
  const size_t kOverlap = sizeof(uint32) + kMaxPathLength;

  std::vector<uint8> buf(kWindow + kOverlap);

  for (off_t base = from; base < limit; base += kWindow) {
    const ssize_t got = file.ReadAt(base, buf.data(), buf.size());
    if (got <= (ssize_t)sizeof(uint32))
      break;

    const size_t scanEnd =
        std::min((size_t)got - sizeof(uint32), (size_t)(limit - base));

    for (size_t i = 0; i < scanEnd; i++) {
      uint32 len = 0;
      memcpy(&len, buf.data() + i, sizeof(len));
      if (len == 0 || len > kMaxPathLength)
        continue;
      if (i + sizeof(len) + len > (size_t)got)
        continue;

      const uint8 *path = buf.data() + i + sizeof(len);
      if (path[0] != '/')
        continue;

      bool printable = true;
      for (uint32 j = 0; j < len; j++) {
        if (path[j] < 0x20 || path[j] == 0x7f) {
          printable = false;
          break;
        }
      }
      if (printable)
        return base + (off_t)i;
    }
  }

  return -1;
}

/**
 * @brief Buffered writer for the binary cache format that tracks failures.
 *
 * Batches the many small field writes into large ones and, unlike the old
 * write-and-hope code, reports a short write so a partial file is never left
 * in place of a good cache.
 */
class CacheWriter {
public:
  explicit CacheWriter(BFile &file) : fFile(file) { fBuffer.reserve(kFlushAt); }

  void WriteRaw(const void *data, size_t size) {
    if (!fOk)
      return;
    const char *bytes = static_cast<const char *>(data);
    fBuffer.insert(fBuffer.end(), bytes, bytes + size);
    if (fBuffer.size() >= kFlushAt)
      _Flush();
  }

  template <typename T> void Write(const T &value) {
    WriteRaw(&value, sizeof(value));
  }

  void WriteString(const BString &s) {
    uint32 len = (uint32)s.Length();
    Write(len);
    if (len > 0)
      WriteRaw(s.String(), len);
  }

  /** @brief Flushes the tail and reports whether every write succeeded. */
  bool Finish() {
    _Flush();
    return fOk;
  }

private:
  static constexpr size_t kFlushAt = 256 * 1024;

  void _Flush() {
    if (!fOk || fBuffer.empty())
      return;
    if (fFile.Write(fBuffer.data(), fBuffer.size()) != (ssize_t)fBuffer.size())
      fOk = false;
    fBuffer.clear();
  }

  BFile &fFile;
  std::vector<char> fBuffer;
  bool fOk{true};
};

/** @brief Number of queued files tag-scanned per looper message. */
const size_t kFileScanChunk = 20;

} // namespace

/**
 * @brief Returns whether a path points to MIDI while MIDI playback is disabled.
 */
static bool IsDisabledMidiPath(const BString &path) {
#if ENABLE_MIDI_PLAYBACK
  (void)path;
  return false;
#else
  BString lower(path);
  lower.ToLower();
  return lower.EndsWith(".mid") || lower.EndsWith(".midi");
#endif
}

/**
 * @brief Constructor.
 * Determines the path to the cache file (user settings) but does not load it
 * yet.
 */
MediaLibraryCache::MediaLibraryCache(const BMessenger &target)
    : BLooper("MediaLibraryCache"), fTarget(target) {
  BPath settingsPath;
  find_directory(B_USER_SETTINGS_DIRECTORY, &settingsPath);
  settingsPath.Append("BeTon/media.cache");
  fCachePath = settingsPath.Path();
}

MediaLibraryCache::~MediaLibraryCache() {
  // Before anything else: no scanner thread may outlive this object. They run
  // their own workers inside TagLib, and letting the team tear down around one
  // is what crashed the app when it was quit during a scan.
  _StopAllScanners();

  // Drop any pending throttled save first; the dirty check below writes the
  // final state synchronously, so a queued MSG_CACHE_FLUSH would be moot.
  delete fSaveThrottle;
  fSaveThrottle = nullptr;

  if (fCacheDirty) {
    DEBUG_PRINT("Saving dirty cache before shutdown...\n");
    SaveCache();
  }

  for (BQuery *q : fRatingQueries) {
    delete q;
  }
  fRatingQueries.clear();
}

/**
 * @brief Loads the list of watched directories from 'directories.settings' or
 * legacy 'directories.txt'.
 * @param outDirs Vector to populate with directory paths.
 */
void MediaLibraryCache::LoadDirectories(std::vector<BString> &outDirs) {
  BPath p;
  if (find_directory(B_USER_SETTINGS_DIRECTORY, &p) != B_OK)
    return;

  // Try loading from the current settings format first.
  BPath settingsPath = p;
  settingsPath.Append("BeTon/directories.settings");
  BFile file(settingsPath.Path(), B_READ_ONLY);

  if (file.InitCheck() == B_OK) {
    BMessage archive;
    if (archive.Unflatten(&file) == B_OK) {
      BMessage srcMsg;
      for (int32 i = 0; archive.FindMessage("source", i, &srcMsg) == B_OK;
           i++) {
        MusicSourceSettings src;
        src.LoadFrom(&srcMsg);
        if (!src.path.IsEmpty()) {
          outDirs.push_back(src.path);
        }
      }
    }
  }

  // Load from settings (folder_source)
  BPath mainSettingsPath = p;
  mainSettingsPath.Append("BeTon/settings");
  BFile mainFile(mainSettingsPath.Path(), B_READ_ONLY);
  if (mainFile.InitCheck() == B_OK) {
    BMessage mainArchive;
    if (mainArchive.Unflatten(&mainFile) == B_OK) {
      BMessage item;
      for (int32 i = 0; mainArchive.FindMessage("folder_source", i, &item) == B_OK; ++i) {
        BString path;
        if (item.FindString("path", &path) == B_OK && !path.IsEmpty()) {
          bool isNested = false;
          for (const auto &d : outDirs) {
            if (path == d) {
              isNested = true;
              break;
            }
            BString prefix = d;
            if (!prefix.EndsWith("/"))
              prefix << "/";
            if (path.StartsWith(prefix)) {
              isNested = true;
              break;
            }
          }
          if (!isNested) {
            outDirs.push_back(path);
          }
        }
        item.MakeEmpty();
      }
    }
  }

  if (!outDirs.empty()) {
    DEBUG_PRINT("Loaded %zu directories from settings\n",
                outDirs.size());
  }
}

/**
 * @brief Triggers a full rescan of all configured directories.
 *
 * Scanning Process:
 * 1. Remove entries that belong to directories no longer monitored.
 * 2. Start Scanners for each directory.
 * 3. Remove existing known files if they are gone from reachable sources.
 *
 * Note: Real sync happens via Scanners reporting back.
 */
void MediaLibraryCache::StartScan() {
  // Steps 1 and 3 below prune entries against what is currently on disk, so
  // starting a full scan while scanners are still reporting would have the
  // two passes fight over the same entries — and would strand the running
  // scanners' completions against a counter they no longer belong to. Hold
  // the request instead; _FinishScanCycle() runs it when the last one lands.
  if (fActiveScanners > 0) {
    DEBUG_PRINT("full scan deferred, %ld scanner(s) still running\n",
                (long)fActiveScanners);
    fRescanPending = true;
    return;
  }
  fRescanPending = false;

  std::vector<BString> dirs;
  LoadDirectories(dirs);

  // 1) Remove entries that belong to directories no longer monitored.
  for (auto it = fEntries.begin(); it != fEntries.end();) {
    const MediaItem &e = it->second;
    bool isUnderMonitored = false;
    for (const auto &dir : dirs) {
      if (e.base == dir) {
        isUnderMonitored = true;
        break;
      }
      BString prefix = dir;
      if (!prefix.EndsWith("/"))
        prefix << "/";
      if (e.base.StartsWith(prefix)) {
        isUnderMonitored = true;
        break;
      }
    }
    if (!isUnderMonitored) {
      it = fEntries.erase(it);
      fCacheDirty = true;
    } else {
      ++it;
    }
  }

  // Notify UI that scan starts from current known state.
  if (fTarget.IsValid()) {
    BMessage update(MSG_CACHE_LOADED);
    fTarget.SendMessage(&update);
  }

  // 2) Start scanners. The counter is never reset here: it is guaranteed to
  // be zero (see the deferral above) and each scanner decrements it exactly
  // once when it reports MSG_SCAN_DONE.
  std::set<BString> offlineBases;
  for (const auto &dirPath : dirs) {
    entry_ref ref;
    status_t s = get_ref_for_path(dirPath.String(), &ref);
    if (s != B_OK) {
      offlineBases.insert(dirPath);
      MarkBaseOffline(dirPath);
      continue;
    }

    BDirectory dir(&ref);
    if (dir.InitCheck() != B_OK) {
      offlineBases.insert(dirPath);
      MarkBaseOffline(dirPath);
      continue;
    }

    BVolume vol(ref.device);
    if (vol.KnowsQuery() &&
        fQueriedVolumes.find(ref.device) == fQueriedVolumes.end()) {
      _InitRatingLiveQueries(ref.device);
    }

    // Launch scanner. It reports via MSG_MEDIA_ITEM_FOUND/MSG_SCAN_DONE.
    _BeginScanCycle();
    auto *scanner =
        new MediaLibraryScanner(ref, BMessenger(this), fNextScanId++);
    scanner->SetCache(fEntries);
    scanner->Run();

    fScanners[scanner->ScanId()] = scanner;

    BMessenger msgr(scanner);
    msgr.SendMessage(MSG_START_SCAN);
    fActiveScanners++;
  }

  // 3) Remove stale files from reachable sources.
  for (auto it = fEntries.begin(); it != fEntries.end();) {
    const BString path = it->first;
    bool wasMissing = it->second.missing;

    bool baseOffline = false;
    for (const auto &base : offlineBases) {
      BString basePrefix(base);
      basePrefix << "/";
      if (path == base || path.StartsWith(basePrefix)) {
        baseOffline = true;
        break;
      }
    }
    if (baseOffline) {
      ++it;
      continue;
    }

    BEntry e(path.String());
    if (!e.Exists()) {
      DEBUG_PRINT("Remove missing file: %s\n", path.String());

      if (fTarget.IsValid()) {
        BMessage gone(MSG_MEDIA_ITEM_REMOVED);
        gone.AddString("path", path);
        fTarget.SendMessage(&gone);
      }

      it = fEntries.erase(it);
      fCacheDirty = true;
      continue;
    }

    if (wasMissing) {
      it->second.missing = false;
      fCacheDirty = true;
    }

    ++it;
  }

  // If no scanners were started (e.g. no directories), finish immediately.
  if (fActiveScanners == 0)
    _FinishScanCycle();
}

void MediaLibraryCache::_FinishScanCycle() {
  // A full scan asked for while scanners were running was held back; now that
  // the field is clear, run it. It drives its own completion from here.
  if (fRescanPending) {
    DEBUG_PRINT("running deferred full scan\n");
    fRescanPending = false;
    StartScan();
    return;
  }

  if (fCacheDirty) {
    DEBUG_PRINT("all scanners finished, writing media.cache\n");
    SaveCache();
  }

  // Queued single-file work (playlist tracks outside any scanned folder) runs
  // now that the scanners are out of the way. It reports the MSG_SCAN_DONE to
  // the UI itself once the queue drains.
  if (fFileScanIndex < fPendingFileScans.size() && !fFileScanRunning) {
    // Filtered here, not when the paths arrived: the scanners have finished,
    // so anything they already refreshed drops out of the workload instead of
    // inflating the total the user is watching.
    _PrepareFileScanQueue();
    if (!fPendingFileScans.empty()) {
      _BeginScanCycle(); // No-op mid-cycle; starts the clock if we were idle.
      fFileScanRunning = true;
      _EmitScanProgress(true);
      PostMessage(MSG_SCAN_FILES_STEP);
      return;
    }
  } else if (fFileScanIndex < fPendingFileScans.size()) {
    return; // Drain already in flight.
  }

  fScanCycleActive = false;

  if (fTarget.IsValid()) {
    DEBUG_PRINT("forward MSG_SCAN_DONE to MainWindow\n");
    BMessage done(MSG_SCAN_DONE);
    // Marks the one completion that means the whole cycle is over. Each
    // scanner also reports directly to the window, so without this the UI
    // could not tell the last one from the first.
    done.AddBool("final", true);
    fTarget.SendMessage(&done);
  }
}

/**
 * @brief Saves the current in-memory cache to disk.
 *
 * Written to a sibling temp file and renamed into place, so an interrupted
 * write (crash, KDL, power loss, full disk) leaves the previous good cache
 * intact instead of a half-written one. The old in-place `B_ERASE_FILE` write
 * had a window of several hundred milliseconds during which any interruption
 * produced exactly the truncated file that LoadCache() now has to recover
 * from.
 */
void MediaLibraryCache::SaveCache() {
  bigtime_t t0 = system_time();

  BString tempPath(fCachePath);
  tempPath << ".new";

  BFile file(tempPath.String(), B_WRITE_ONLY | B_CREATE_FILE | B_ERASE_FILE);
  if (file.InitCheck() != B_OK) {
    DEBUG_PRINT("SaveCache: Failed to open %s\n", tempPath.String());
    return;
  }

  uint32 magic = kCacheMagic;
  uint32 version = kCacheVersion;
  uint32 count = (uint32)fEntries.size();

  CacheWriter writer(file);
  writer.Write(magic);
  writer.Write(version);
  writer.Write(count);

  for (const auto &[key, e] : fEntries) {
    writer.WriteString(e.path);
    writer.WriteString(e.base);
    writer.WriteString(e.title);
    writer.WriteString(e.artist);
    writer.WriteString(e.album);
    writer.WriteString(e.albumArtist);
    writer.WriteString(e.genre);
    writer.WriteString(e.comment);
    writer.WriteString(e.composer);
    writer.WriteString(e.mbTrackId);
    writer.WriteString(e.mbAlbumId);
    writer.WriteString(e.mbArtistId);
    writer.WriteString(e.acoustId);

    writer.Write(e.year);
    writer.Write(e.track);
    writer.Write(e.trackTotal);
    writer.Write(e.disc);
    writer.Write(e.discTotal);
    writer.Write(e.duration);
    writer.Write(e.bitrate);
    writer.Write(e.sampleRate);
    writer.Write(e.channels);
    writer.Write(e.rating);
    writer.Write(e.size);
    writer.Write(e.mtime);
    writer.Write(e.inode);

    uint8 flags = e.missing ? 1 : 0;
    writer.Write(flags);
  }

  if (!writer.Finish()) {
    DEBUG_PRINT("SaveCache: short write to %s, keeping previous cache\n",
                tempPath.String());
    file.Unset();
    unlink(tempPath.String());
    return;
  }

  // Get the bytes onto the disk before the rename publishes them, so the
  // rename can never expose a file whose contents have not landed yet.
  file.Sync();
  file.Unset();

  if (rename(tempPath.String(), fCachePath.String()) != 0) {
    DEBUG_PRINT("SaveCache: rename to %s failed\n", fCachePath.String());
    unlink(tempPath.String());
    return;
  }

  bigtime_t t1 = system_time();
  DEBUG_PRINT("SaveCache: %lu items in %lld us to %s\n", (unsigned long)count,
              (long long)(t1 - t0), fCachePath.String());
  fCacheDirty = false;
}

/**
 * @brief Loads the cache from disk into memory, recovering from a bad file.
 *
 * Supports both the binary format (magic 'BTCA') and the legacy BMessage
 * format. If the file turns out to be unreadable — truncated by an
 * interrupted write, scrambled by a crash, or simply not a cache at all — the
 * entries loaded so far are dropped, the file is moved aside, and the UI is
 * told to rebuild via MSG_CACHE_CORRUPT. Nothing here is fatal: a damaged
 * cache costs a rescan, never a crash on startup.
 */
void MediaLibraryCache::LoadCache() {
  fEntries.clear();

  const CacheLoadResult result = _ReadCacheFile();

  if (result == kCacheCorrupt) {
    fEntries.clear();
    fCacheDirty = false;
    _DiscardCorruptCache();
  } else if (result == kCacheSalvaged) {
    // Keep the bad file for post-mortem, then rewrite a clean one from what
    // was recovered so the next start is an ordinary load.
    _DiscardCorruptCache();
    fCacheDirty = true;
    SaveCache();
  }

  if (fTarget.IsValid()) {
    BMessage msg(MSG_CACHE_LOADED);
    fTarget.SendMessage(&msg);
  }

  _InitAllLiveQueries();

  if (!fTarget.IsValid())
    return;

  // Sent after MSG_CACHE_LOADED so the window has already taken on whatever
  // was recovered before it starts filling in the gaps.
  if (result != kCacheOk) {
    BMessage corrupt(MSG_CACHE_CORRUPT);
    corrupt.AddInt32("recovered", (int32)fEntries.size());
    corrupt.AddInt32("declared", (int32)fSalvage.declared);
    if (!fCorruptBackupPath.IsEmpty())
      corrupt.AddString("backup", fCorruptBackupPath);
    fTarget.SendMessage(&corrupt);
    return;
  }

  // A readable cache holding nothing means either no cache file has been
  // written yet or the last one was emptied. Either way the library has to be
  // built from scratch, which nothing else at startup would do. The window
  // decides whether that is worth it — it can also see the saved playlists —
  // so pass along how many source directories are configured.
  if (fEntries.empty()) {
    std::vector<BString> dirs;
    LoadDirectories(dirs);

    DEBUG_PRINT("Cache is empty, %zu source directories configured\n",
                dirs.size());

    BMessage empty(MSG_CACHE_EMPTY);
    empty.AddInt32("sources", (int32)dirs.size());
    fTarget.SendMessage(&empty);
  }
}

/**
 * @brief Opens the cache file and dispatches to the matching format reader.
 */
MediaLibraryCache::CacheLoadResult MediaLibraryCache::_ReadCacheFile() {
  BFile file(fCachePath, B_READ_ONLY);
  if (file.InitCheck() != B_OK) {
    DEBUG_PRINT("No cache found (%s)\n", fCachePath.String());
    return kCacheOk; // Nothing to load is not a corruption.
  }

  off_t fileSize = 0;
  if (file.GetSize(&fileSize) != B_OK) {
    DEBUG_PRINT("Cache: cannot determine size of %s\n", fCachePath.String());
    return kCacheCorrupt;
  }
  if (fileSize == 0) {
    // An empty file is the classic result of an interrupted create+write.
    DEBUG_PRINT("Cache: %s is empty\n", fCachePath.String());
    return kCacheCorrupt;
  }

  uint32 magic = 0;
  if (fileSize < (off_t)sizeof(magic) ||
      file.Read(&magic, sizeof(magic)) != (ssize_t)sizeof(magic)) {
    DEBUG_PRINT("Cache: %s is too small to hold a header\n",
                fCachePath.String());
    return kCacheCorrupt;
  }

  file.Seek(0, SEEK_SET);
  return (magic == kCacheMagic) ? _ReadBinaryCache(file, fileSize)
                                : _ReadLegacyCache(file);
}

/**
 * @brief Reads the binary cache format, validating every field as it goes.
 */
MediaLibraryCache::CacheLoadResult
MediaLibraryCache::_ReadBinaryCache(BFile &file, off_t fileSize) {
  bigtime_t t0 = system_time();

  CacheReader reader(file, fileSize);

  uint32 magic = 0;
  uint32 version = 0;
  uint32 count = 0;
  if (!reader.Read(magic) || !reader.Read(version) || !reader.Read(count)) {
    DEBUG_PRINT("Cache: truncated header\n");
    return kCacheCorrupt;
  }

  if (version == 0 || version > kCacheVersion) {
    DEBUG_PRINT("Cache: unsupported version %lu\n", (unsigned long)version);
    return kCacheCorrupt;
  }

  fSalvage = SalvageReport();
  fSalvage.declared = count;
  bool damaged = false;

  // Every record needs at least kMinRecordSize bytes, so a count that cannot
  // fit in the rest of the file is already proof of damage. It no longer
  // aborts the load: the records that *are* present still read back fine.
  if ((off_t)count * kMinRecordSize > reader.Remaining()) {
    DEBUG_PRINT("Cache: entry count %lu impossible for %lld bytes\n",
                (unsigned long)count, (long long)fileSize);
    damaged = true;
  }

  while (fSalvage.recovered < count) {
    const off_t recordStart = reader.Position();

    MediaItem e;
    if (ReadRecord(reader, e)) {
      fSalvage.recovered++;
      if (IsDisabledMidiPath(e.path))
        fCacheDirty = true;
      else
        fEntries[e.path] = std::move(e);
      continue;
    }

    // The record is damaged or the file ended early. Look for the next intact
    // one; finding none means there is nothing left worth reading.
    damaged = true;
    const off_t resume = _ResyncToNextRecord(file, recordStart, fileSize);
    if (resume < 0 || !reader.SeekTo(resume))
      break;

    DEBUG_PRINT("Cache: damage at %lld, resynchronised at %lld\n",
                (long long)recordStart, (long long)resume);
    fSalvage.resyncs++;
    if (fSalvage.resyncs >= kMaxResyncProbes)
      break;
  }

  if (!damaged && !reader.AtEnd()) {
    DEBUG_PRINT("Cache: %lld trailing bytes after %lu records\n",
                (long long)reader.Remaining(), (unsigned long)count);
    damaged = true;
  }

  bigtime_t t1 = system_time();

  if (!damaged) {
    DEBUG_PRINT("LoadCache (binary v%lu): %zu items in %lld us\n",
                (unsigned long)version, fEntries.size(),
                (long long)(t1 - t0));
    return kCacheOk;
  }

  if (fEntries.empty()) {
    DEBUG_PRINT("Cache: damaged, nothing salvageable\n");
    return kCacheCorrupt;
  }

  fSalvage.dropped = _VerifySalvagedEntries();

  DEBUG_PRINT("Cache: SALVAGED %lu of %lu declared entries in %lld us "
              "(%ld resyncs, %lu dropped as stale, %zu kept)\n",
              (unsigned long)fSalvage.recovered, (unsigned long)count,
              (long long)(t1 - t0), (long)fSalvage.resyncs,
              (unsigned long)fSalvage.dropped, fEntries.size());

  return fEntries.empty() ? kCacheCorrupt : kCacheSalvaged;
}

off_t MediaLibraryCache::_ResyncToNextRecord(BFile &file, off_t after,
                                             off_t fileSize) {
  off_t from = after + 1;

  for (int probe = 0; probe < kMaxResyncProbes; probe++) {
    const off_t candidate = FindRecordStart(file, from, fileSize);
    if (candidate < 0)
      return -1;

    // A plausible-looking header is not enough; only accept the offset if a
    // whole record actually parses from it.
    CacheReader trial(file, fileSize);
    MediaItem item;
    if (trial.SeekTo(candidate) && ReadRecord(trial, item))
      return candidate;

    from = candidate + 1;
  }

  return -1;
}

uint32 MediaLibraryCache::_VerifySalvagedEntries() {
  uint32 dropped = 0;

  for (auto it = fEntries.begin(); it != fEntries.end();) {
    struct stat st;
    if (stat(it->first.String(), &st) != 0) {
      // Unreachable right now (typically an unmounted volume). Keep it and
      // let the normal offline/missing handling decide.
      ++it;
      continue;
    }

    if (it->second.size != (int64)st.st_size ||
        it->second.mtime != (int64)st.st_mtime) {
      DEBUG_PRINT("Cache: dropping stale salvaged entry %s\n",
                  it->first.String());
      it = fEntries.erase(it);
      dropped++;
      continue;
    }

    ++it;
  }

  return dropped;
}

/**
 * @brief Reads the pre-'BTCA' flattened-BMessage cache format.
 *
 * Kept for migration; the entries are re-saved in the binary format on the
 * next write (fCacheDirty is set).
 */
MediaLibraryCache::CacheLoadResult
MediaLibraryCache::_ReadLegacyCache(BFile &file) {
  bigtime_t t0 = system_time();

  BMessage archive;
  if (archive.Unflatten(&file) != B_OK) {
    // Not a binary cache and not a flattened BMessage either.
    DEBUG_PRINT("Cache: could not unflatten %s\n", fCachePath.String());
    return kCacheCorrupt;
  }

  bigtime_t t1 = system_time();

  MediaItem entry;
  for (int32 i = 0;; i++) {
    BMessage item;
    if (archive.FindMessage("entry", i, &item) != B_OK)
      break;

    entry.path = item.GetString("path", "");
    entry.base = item.GetString("base", "");
    entry.title = item.GetString("title", "");
    entry.artist = item.GetString("artist", "");
    entry.album = item.GetString("album", "");
    entry.genre = item.GetString("genre", "");
    entry.year = item.GetInt32("year", 0);
    entry.track = item.GetInt32("track", 0);
    entry.disc = item.GetInt32("disc", 0);
    entry.duration = item.GetInt32("duration", 0);
    entry.bitrate = item.GetInt32("bitrate", 0);
    entry.size = item.GetInt64("size", 0);
    entry.mtime = item.GetInt64("mtime", 0);
    entry.inode = item.GetInt64("inode", 0);
    entry.missing = item.GetBool("missing", false);

    entry.mbAlbumId = item.GetString("mbAlbumId", "");
    entry.mbArtistId = item.GetString("mbArtistId", "");
    entry.mbTrackId = item.GetString("mbTrackId", "");
    entry.rating = item.GetInt32("rating", 0);

    if (entry.path.IsEmpty())
      continue;

    if (IsDisabledMidiPath(entry.path)) {
      fCacheDirty = true;
      continue;
    }

    fEntries[entry.path] = entry;
  }

  bigtime_t t2 = system_time();
  DEBUG_PRINT("LoadCache (BMessage legacy): %zu items "
              "(unflatten=%lld us, extract=%lld us)\n",
              fEntries.size(), (long long)(t1 - t0), (long long)(t2 - t1));

  fCacheDirty = true;
  return kCacheOk;
}

/**
 * @brief Moves a corrupt cache file aside so the next start reads a clean one.
 */
void MediaLibraryCache::_DiscardCorruptCache() {
  BString badPath(fCachePath);
  badPath << ".corrupt";

  fCorruptBackupPath.Truncate(0);

  if (rename(fCachePath.String(), badPath.String()) == 0) {
    fCorruptBackupPath = badPath;
    DEBUG_PRINT("Cache: corrupt file kept as %s\n", badPath.String());
  } else if (unlink(fCachePath.String()) == 0) {
    DEBUG_PRINT("Cache: corrupt file %s deleted\n", fCachePath.String());
  } else {
    DEBUG_PRINT("Cache: could not remove corrupt file %s\n",
                fCachePath.String());
  }
}

/**
 * @brief Returns a copy of all current media items.
 * @return std::vector<MediaItem>
 */
std::vector<MediaItem> MediaLibraryCache::AllEntries() const {
  std::vector<MediaItem> out;
  out.reserve(fEntries.size());

  for (const auto &kv : fEntries) {
    out.push_back(kv.second);
  }
  return out;
}

/**
 * @brief Main message loop for the MediaLibraryCache looper.
 * Handles loading, batch updates, and scanning notifications.
 */
void MediaLibraryCache::MessageReceived(BMessage *msg) {
  switch (msg->what) {
  case MSG_LOAD_CACHE:
    DEBUG_PRINT("Asynchronous cache load started\n");
    LoadCache();
    break;

  case MSG_MEDIA_BATCH: {
    type_code type;
    int32 count = 0;
    if (msg->GetInfo("path", &type, &count) != B_OK)
      break;

    const char *baseStr = nullptr;
    msg->FindString("base", &baseStr);

    for (int32 i = 0; i < count; i++) {
      MediaItem e;
      const char *itemBaseStr = nullptr;
      if (msg->FindString("item_base", i, &itemBaseStr) == B_OK)
        e.base = itemBaseStr;
      else if (baseStr)
        e.base = baseStr;

      const char *tmp = nullptr;
      if (msg->FindString("path", i, &tmp) == B_OK)
        e.path = tmp;
      if (msg->FindString("title", i, &tmp) == B_OK)
        e.title = tmp;
      if (msg->FindString("artist", i, &tmp) == B_OK)
        e.artist = tmp;
      if (msg->FindString("album", i, &tmp) == B_OK)
        e.album = tmp;
      if (msg->FindString("genre", i, &tmp) == B_OK)
        e.genre = tmp;

      msg->FindInt32("year", i, &e.year);
      msg->FindInt32("track", i, &e.track);
      msg->FindInt32("disc", i, &e.disc);
      msg->FindInt32("duration", i, &e.duration);
      msg->FindInt32("bitrate", i, &e.bitrate);
      msg->FindInt64("size", i, &e.size);
      msg->FindInt64("mtime", i, &e.mtime);
      msg->FindInt64("inode", i, &e.inode);
      msg->FindInt32("rating", i, &e.rating);
      if (e.rating > 0)
        DEBUG_PRINT("Received rating %d for %s\n", (int)e.rating,
                    e.path.String());

      if (msg->FindString("mbAlbumId", i, &tmp) == B_OK)
        e.mbAlbumId = tmp;
      if (msg->FindString("mbArtistId", i, &tmp) == B_OK)
        e.mbArtistId = tmp;
      if (msg->FindString("mbTrackId", i, &tmp) == B_OK)
        e.mbTrackId = tmp;

      AddOrUpdateEntry(e);
    }

    DEBUG_PRINT("Processed batch of %d items\n", (int)count);

    // Ensure cache is saved after scan completion.
    fCacheDirty = true;

    if (fTarget.IsValid())
      fTarget.SendMessage(msg);
    break;
  }

  case MSG_MEDIA_ITEM_FOUND: {
    MediaItem e;
    const char *tmpStr = nullptr;

    if (msg->FindString("path", &tmpStr) != B_OK)
      break;
    e.path = tmpStr;

    auto existing = fEntries.find(e.path);
    if (existing != fEntries.end())
      e = existing->second;
    e.path = tmpStr;

    if (msg->FindString("base", &tmpStr) == B_OK)
      e.base = tmpStr;
    if (msg->FindString("title", &tmpStr) == B_OK)
      e.title = tmpStr;
    if (msg->FindString("artist", &tmpStr) == B_OK)
      e.artist = tmpStr;
    if (msg->FindString("album", &tmpStr) == B_OK)
      e.album = tmpStr;
    if (msg->FindString("genre", &tmpStr) == B_OK)
      e.genre = tmpStr;
    if (msg->FindString("comment", &tmpStr) == B_OK)
      e.comment = tmpStr;
    if (msg->FindString("albumArtist", &tmpStr) == B_OK)
      e.albumArtist = tmpStr;
    if (msg->FindString("composer", &tmpStr) == B_OK)
      e.composer = tmpStr;

    msg->FindInt32("year", &e.year);
    msg->FindInt32("track", &e.track);
    msg->FindInt32("trackTotal", &e.trackTotal);
    msg->FindInt32("disc", &e.disc);
    msg->FindInt32("discTotal", &e.discTotal);
    msg->FindInt32("duration", &e.duration);
    msg->FindInt32("bitrate", &e.bitrate);
    msg->FindInt32("sampleRate", &e.sampleRate);
    msg->FindInt32("channels", &e.channels);
    msg->FindInt32("rating", &e.rating);
    msg->FindInt64("size", &e.size);
    msg->FindInt64("mtime", &e.mtime);
    msg->FindInt64("inode", &e.inode);

    if (msg->FindString("mbAlbumId", &tmpStr) == B_OK ||
        msg->FindString("mbAlbumID", &tmpStr) == B_OK)
      e.mbAlbumId = tmpStr;
    if (msg->FindString("mbArtistId", &tmpStr) == B_OK ||
        msg->FindString("mbArtistID", &tmpStr) == B_OK)
      e.mbArtistId = tmpStr;
    if (msg->FindString("mbTrackId", &tmpStr) == B_OK ||
        msg->FindString("mbTrackID", &tmpStr) == B_OK)
      e.mbTrackId = tmpStr;

    AddOrUpdateEntry(e);

    fCacheDirty = true;

    DEBUG_PRINT("Item found: path=%s, title=%s\n",
                e.path.String(), e.title.String());

    if (fTarget.IsValid())
      fTarget.SendMessage(msg);
    break;
  }

  case MSG_FILE_MOVED: {
    BString from, to;
    if (msg->FindString("from", &from) != B_OK ||
        msg->FindString("to", &to) != B_OK)
      break;

    auto it = fEntries.find(from);
    if (it == fEntries.end())
      break;

    MediaItem e = it->second;
    fEntries.erase(it);
    e.path = to;
    fEntries[e.path] = e;

    DEBUG_PRINT("File moved in cache: %s -> %s\n", from.String(),
                to.String());
    SaveCache();
    fCacheDirty = false;
    break;
  }

  case MSG_REGISTER_TARGET: {
    BMessenger newTarget;
    if (msg->FindMessenger("target", &newTarget) == B_OK) {
      fTarget = newTarget;
      DEBUG_PRINT("UI target registered\n");
    }
    break;
  }
  case MSG_RESCAN: {
    DEBUG_PRINT("received MSG_RESCAN, starting new scan\n");
    BString scanPath;
    if (msg->FindString("path", &scanPath) == B_OK && !scanPath.IsEmpty()) {
      ScanPath(scanPath);
    } else {
      StartScan();
    }
    break;
  }

  case MSG_WATCH_FOLDER: {
    BString watchPath;
    if (msg->FindString("path", &watchPath) == B_OK && !watchPath.IsEmpty()) {
      StartWatchingFolder(watchPath);
    } else {
      StopWatchingFolder();
    }
    break;
  }

  case MSG_CACHE_FLUSH: {
    delete fSaveThrottle;
    fSaveThrottle = nullptr;
    if (fCacheDirty) {
      SaveCache();
      fCacheDirty = false;
    }
    break;
  }

  case B_NODE_MONITOR: {
    int32 opcode;
    if (msg->FindInt32("opcode", &opcode) == B_OK) {
      _HandleNodeMonitor(msg, opcode);
    }
    break;
  }

  case MSG_SCAN_TOTALS: {
    int32 scanId = 0;
    if (msg->FindInt32("scan_id", &scanId) != B_OK)
      break;
    ScanCounters &counters = fScanCounters[scanId];
    msg->FindInt32("folders", &counters.folders);
    msg->FindInt32("files", &counters.filesTotal);
    _EmitScanProgress(true);
    break;
  }

  case MSG_SCAN_PROGRESS: {
    int32 scanId = 0;
    if (msg->FindInt32("scan_id", &scanId) != B_OK)
      break;
    msg->FindInt32("files_done", &fScanCounters[scanId].filesDone);
    _EmitScanProgress();
    break;
  }

  case MSG_SCAN_FILES: {
    _QueueFileScans(msg);
    break;
  }

  case MSG_SCAN_FILES_STEP: {
    _ProcessFileScanChunk();
    break;
  }

  case MSG_SCAN_DONE: {
    // With the counter no longer being reset under running scanners this
    // should not happen; ignore it rather than reporting a second completion.
    if (fActiveScanners <= 0) {
      DEBUG_PRINT("stray MSG_SCAN_DONE, no scanner outstanding\n");
      break;
    }

    fActiveScanners--;
    DEBUG_PRINT("received MSG_SCAN_DONE (scanners left: %ld)\n",
                (long)fActiveScanners);

    // Retire the reporting scanner now rather than letting it delete itself:
    // owning the lifetime here is what makes the shutdown sweep possible.
    int32 scanId = 0;
    if (msg->FindInt32("scan_id", &scanId) == B_OK)
      _DestroyScanner(scanId);

    if (fActiveScanners == 0)
      _FinishScanCycle();
    break;
  }

  case B_QUERY_UPDATE: {
    int32 opcode;
    if (msg->FindInt32("opcode", &opcode) == B_OK) {
      DEBUG_PRINT("B_QUERY_UPDATE received with opcode %ld\n",
                  (long)opcode);

      dev_t device;
      ino_t directory;
      const char *name;
      if (msg->FindInt32("device", &device) == B_OK &&
          msg->FindInt64("directory", &directory) == B_OK &&
          msg->FindString("name", &name) == B_OK) {
        entry_ref ref(device, directory, name);
        BPath path(&ref);
        if (path.InitCheck() == B_OK) {
          BString pathStr = path.Path();
          DEBUG_PRINT("B_QUERY_UPDATE path: %s\n",
                      pathStr.String());

          auto it = fEntries.find(pathStr);
          if (it != fEntries.end()) {
            DEBUG_PRINT("Path found in fEntries, rereading "
                        "attributes...\n");
            if (_RereadBfsAttributes(it->second)) {
              DEBUG_PRINT("Attributes changed! Sending "
                          "MSG_MEDIA_ITEM_FOUND\n");
              fCacheDirty = true;
              if (fTarget.IsValid()) {
                BMessage update(MSG_MEDIA_ITEM_FOUND);
                update.AddString("path", it->second.path);
                update.AddString("title", it->second.title);
                update.AddString("artist", it->second.artist);
                update.AddString("album", it->second.album);
                update.AddString("genre", it->second.genre);
                update.AddString("comment", it->second.comment);
                update.AddString("albumArtist", it->second.albumArtist);
                update.AddString("composer", it->second.composer);
                update.AddInt32("year", it->second.year);
                update.AddInt32("track", it->second.track);
                update.AddInt32("rating", it->second.rating);
                update.AddInt32("duration", it->second.duration);
                update.AddInt32("bitrate", it->second.bitrate);
                fTarget.SendMessage(&update);
              }
            }
          }
        }
      }
    }
    break;
  }

  default:
    BLooper::MessageReceived(msg);
  }
}

/**
 * @brief Updates or inserts a media item into the internal map.
 * Also checks for potential conflicts or data integrity issues (warns on DB ID
 * loss).
 * @param entry The item to store.
 */
void MediaLibraryCache::AddOrUpdateEntry(const MediaItem &entry) {
  auto it = fEntries.find(entry.path);
  if (it == fEntries.end()) {
    fEntries[entry.path] = entry;

  } else {
    const MediaItem &old = it->second;
    if (!old.mbTrackId.IsEmpty() && entry.mbTrackId.IsEmpty()) {
      DEBUG_PRINT("WARNING: Overwriting existing MB Track ID "
                  "for %s with empty value!\n",
                  entry.path.String());
    }
    fEntries[entry.path] = entry;
  }
}

/**
 * @brief Marks all entries belonging to a specific base path as "missing".
 * This is used when a configured directory is not found/mounted.
 */
void MediaLibraryCache::MarkBaseOffline(const BString &basePath) {
  for (auto &kv : fEntries) {
    if (kv.first.StartsWith(basePath)) {
      kv.second.missing = true;
    }
  }

  if (fTarget.IsValid()) {
    BMessage off(MSG_BASE_OFFLINE);
    off.AddString("base", basePath);
    fTarget.SendMessage(&off);
  }
}

/**
 * @brief Creates live queries for `Media:Rating == 1..10` on one device.
 * @param device Target BFS volume device id.
 */
void MediaLibraryCache::_InitRatingLiveQueries(dev_t device) {
  BVolume vol(device);
  if (vol.InitCheck() != B_OK || !vol.KnowsQuery())
    return;

  for (int32 i = 1; i <= 10; i++) {
    BQuery *q = new BQuery();
    q->SetVolume(&vol);
    q->PushAttr("Media:Rating");
    q->PushInt32(i);
    q->PushOp(B_EQ);
    q->SetTarget(BMessenger(this));
    status_t status = q->Fetch();
    if (status == B_OK) {
      entry_ref dummy;
      while (q->GetNextRef(&dummy) == B_OK) {
        // Drain initial results so the live query starts emitting updates.
      }
    } else {
      DEBUG_PRINT("Live Query failed for rating %ld on device "
                  "%d (status %d)\n",
                  (long)i, (int)device, (int)status);
    }
    fRatingQueries.push_back(q);
  }

  fQueriedVolumes.insert(device);
  DEBUG_PRINT(
      "Initialized 10 Rating Live Queries for device %d\n",
      (int)device);
}

/**
 * @brief Initializes live queries for all configured directories.
 * This is called after the cache is loaded to ensure queries are active
 * even if a full scan is not performed.
 */
void MediaLibraryCache::_InitAllLiveQueries() {
  std::vector<BString> dirs;
  LoadDirectories(dirs);
  for (const BString &dirPath : dirs) {
    BEntry entry(dirPath.String(), true);
    if (entry.InitCheck() == B_OK && entry.Exists() && entry.IsDirectory()) {
      entry_ref ref;
      entry.GetRef(&ref);
      BVolume vol(ref.device);
      if (vol.KnowsQuery() &&
          fQueriedVolumes.find(ref.device) == fQueriedVolumes.end()) {
        _InitRatingLiveQueries(ref.device);
      }
    }
  }
}

/**
 * @brief Re-reads BFS attributes for a single media item.
 *
 * Opens the file's BNode and reads Haiku's standard audio BFS attributes
 * (Media:Title, Audio:Artist, Media:Rating, etc.). Updates the
 * MediaItem fields in-place.
 *
 * @param item The item to update.
 * @return True if any attribute value changed, false otherwise.
 */
bool MediaLibraryCache::_RereadBfsAttributes(MediaItem &item) {
  BNode node(item.path.String());
  if (node.InitCheck() != B_OK)
    return false;

  char buffer[512];
  memset(buffer, 0, sizeof(buffer));
  int32 intVal = 0;
  bool changed = false;

  auto readStr = [&](const char *attr, BString &field) {
    memset(buffer, 0, sizeof(buffer));
    if (node.ReadAttr(attr, B_STRING_TYPE, 0, buffer, sizeof(buffer)) > 0) {
      BString val(buffer);
      if (val != field) {
        field = val;
        changed = true;
      }
    }
  };

  auto readInt = [&](const char *attr, int32 &field) {
    intVal = 0;
    if (node.ReadAttr(attr, B_INT32_TYPE, 0, &intVal, sizeof(intVal)) > 0) {
      if (intVal != field) {
        field = intVal;
        changed = true;
      }
    }
  };

  readStr("Media:Title", item.title);
  readStr("Audio:Artist", item.artist);
  readStr("Audio:Album", item.album);
  readStr("Media:Genre", item.genre);
  readStr("Media:Comment", item.comment);
  readInt("Media:Year", item.year);
  readInt("Audio:Track", item.track);
  readInt("Media:Rating", item.rating);
  readInt("Media:Length", item.duration);
  readInt("Audio:Bitrate", item.bitrate);

  return changed;
}

void MediaLibraryCache::ScanPath(const BString &dirPath) {
  entry_ref ref;
  status_t s = get_ref_for_path(dirPath.String(), &ref);
  if (s != B_OK)
    return;

  BDirectory dir(&ref);
  if (dir.InitCheck() != B_OK)
    return;

  // The scanner only reports files it finds, so anything deleted while this
  // folder was not being watched would survive as a stale row. Reconcile
  // before scanning so the view drops it.
  _ReconcileFolderDeletions(dirPath);

  BVolume vol(ref.device);
  if (vol.KnowsQuery() &&
      fQueriedVolumes.find(ref.device) == fQueriedVolumes.end()) {
    _InitRatingLiveQueries(ref.device);
  }

  // Launch scanner. It reports via MSG_MEDIA_ITEM_FOUND/MSG_SCAN_DONE.
  _BeginScanCycle();
  auto *scanner =
      new MediaLibraryScanner(ref, BMessenger(this), fNextScanId++);
  scanner->SetCache(fEntries);
  scanner->Run();

  fScanners[scanner->ScanId()] = scanner;

  BMessenger msgr(scanner);
  msgr.SendMessage(MSG_START_SCAN);
  fActiveScanners++;
}

void MediaLibraryCache::StartWatchingFolder(const BString &folderPath) {
  StopWatchingFolder();

  BEntry entry(folderPath.String(), true);
  if (entry.InitCheck() != B_OK || !entry.Exists() || !entry.IsDirectory())
    return;

  BDirectory dir(&entry);
  _WatchDirRecursive(dir);
}

void MediaLibraryCache::StopWatchingFolder() {
  for (const auto &nref : fWatchedNodes) {
    watch_node(&nref, B_STOP_WATCHING, this);
  }
  fWatchedNodes.clear();
}

void MediaLibraryCache::_WatchDirRecursive(BDirectory &dir) {
  node_ref nref;
  if (dir.GetNodeRef(&nref) == B_OK) {
    bool alreadyWatched = false;
    for (const auto &w : fWatchedNodes) {
      if (w.device == nref.device && w.node == nref.node) {
        alreadyWatched = true;
        break;
      }
    }
    if (!alreadyWatched) {
      if (watch_node(&nref, B_WATCH_DIRECTORY | B_WATCH_NAME, this) == B_OK) {
        fWatchedNodes.push_back(nref);
      }
    }
  }

  dir.Rewind();
  BEntry entry;
  while (dir.GetNextEntry(&entry) == B_OK) {
    if (entry.IsDirectory()) {
      BDirectory subDir(&entry);
      _WatchDirRecursive(subDir);
    } else if (entry.IsFile()) {
      BPath path;
      if (entry.GetPath(&path) == B_OK && _IsSupportedAudioFile(path.Path())) {
        node_ref fileRef;
        if (entry.GetNodeRef(&fileRef) == B_OK) {
          bool alreadyWatched = false;
          for (const auto &w : fWatchedNodes) {
            if (w.device == fileRef.device && w.node == fileRef.node) {
              alreadyWatched = true;
              break;
            }
          }
          if (!alreadyWatched) {
            if (watch_node(&fileRef, B_WATCH_STAT, this) == B_OK) {
              fWatchedNodes.push_back(fileRef);
            }
          }
        }
      }
    }
  }
}

void MediaLibraryCache::_HandleNodeMonitor(BMessage *msg, int32 opcode) {
  switch (opcode) {
    case B_ENTRY_CREATED: {
      dev_t device;
      ino_t directory;
      const char *name = nullptr;
      if (msg->FindInt32("device", &device) == B_OK &&
          msg->FindInt64("directory", &directory) == B_OK &&
          msg->FindString("name", &name) == B_OK) {
        entry_ref ref(device, directory, name);
        BEntry entry(&ref, true);
        if (entry.Exists()) {
          if (entry.IsDirectory()) {
            node_ref nref;
            if (entry.GetNodeRef(&nref) == B_OK) {
              if (watch_node(&nref, B_WATCH_DIRECTORY | B_WATCH_NAME, this) == B_OK) {
                fWatchedNodes.push_back(nref);
                BDirectory dir(&entry);
                _WatchDirRecursive(dir);
              }
            }
          } else if (entry.IsFile()) {
            BPath path(&ref);
            if (path.InitCheck() == B_OK && _IsSupportedAudioFile(path.Path())) {
              node_ref fileRef;
              if (entry.GetNodeRef(&fileRef) == B_OK) {
                watch_node(&fileRef, B_WATCH_STAT, this);
                fWatchedNodes.push_back(fileRef);
              }
              _ScanAndAddFile(path.Path());
            }
          }
        }
      }
      break;
    }

    case B_ENTRY_REMOVED: {
      ino_t node;
      if (msg->FindInt64("node", &node) == B_OK) {
        for (auto it = fEntries.begin(); it != fEntries.end(); ++it) {
          if (it->second.inode == node) {
            _RemoveFileFromCache(it->first);
            break;
          }
        }
      }
      break;
    }

    case B_ENTRY_MOVED: {
      dev_t device;
      ino_t fromDir;
      ino_t toDir;
      const char *fromName = nullptr;
      const char *toName = nullptr;
      ino_t node;
      if (msg->FindInt32("device", &device) == B_OK &&
          msg->FindInt64("from_directory", &fromDir) == B_OK &&
          msg->FindInt64("to_directory", &toDir) == B_OK &&
          msg->FindString("from_name", &fromName) == B_OK &&
          msg->FindString("to_name", &toName) == B_OK &&
          msg->FindInt64("node", &node) == B_OK) {
        
        BString oldPath;
        MediaItem item;
        bool found = false;
        for (auto it = fEntries.begin(); it != fEntries.end(); ++it) {
          if (it->second.inode == node) {
            oldPath = it->first;
            item = it->second;
            found = true;
            break;
          }
        }

        if (found) {
          entry_ref toRef(device, toDir, toName);
          BPath path(&toRef);
          if (path.InitCheck() == B_OK) {
            BString newPath = path.Path();
            fEntries.erase(oldPath);
            item.path = newPath;
            BPath parentPath;
            if (path.GetParent(&parentPath) == B_OK) {
              item.base = parentPath.Path();
            }
            fEntries[newPath] = item;
            _ScheduleSave();

            if (fTarget.IsValid()) {
              BMessage fileMoved(MSG_FILE_MOVED);
              fileMoved.AddString("from", oldPath);
              fileMoved.AddString("to", newPath);
              fTarget.SendMessage(&fileMoved);
            }
          }
        }
      }
      break;
    }

    case B_STAT_CHANGED: {
      ino_t node;
      if (msg->FindInt64("node", &node) == B_OK) {
        for (auto it = fEntries.begin(); it != fEntries.end(); ++it) {
          if (it->second.inode == node) {
            _ScanAndAddFile(it->first);
            break;
          }
        }
      }
      break;
    }
  }
}

void MediaLibraryCache::_ScanAndAddFile(const BString &pathStr) {
  struct stat st;
  if (stat(pathStr.String(), &st) != 0)
    return;

  MediaItem item;
  item.path = pathStr;
  BPath path(pathStr.String());
  BPath parentPath;
  if (path.GetParent(&parentPath) == B_OK) {
    item.base = parentPath.Path();
  } else {
    item.base = pathStr;
  }
  item.title = path.Leaf() ? BString(path.Leaf()) : pathStr;
  item.size = st.st_size;
  item.mtime = st.st_mtime;
  item.inode = st.st_ino;

  TagData td;
  MetadataWriteTargets targets = MetadataTagIO::WriteTargetsForPath(pathStr);
  bool readOk = (!targets.tags && targets.bfs)
      ? MetadataTagIO::ReadBfsAttributes(path, td)
      : MetadataTagIO::ReadTags(path, td);
  if (readOk) {
    if (!td.title.IsEmpty())
      item.title = td.title;
    item.artist = td.artist;
    item.album = td.album;
    item.albumArtist = td.albumArtist;
    item.composer = td.composer;
    item.genre = td.genre;
    item.comment = td.comment;
    item.mbTrackId = td.mbTrackID;
    item.mbAlbumId = td.mbAlbumID;
    item.mbArtistId = td.mbArtistID;
    item.year = td.year;
    item.track = td.track;
    item.trackTotal = td.trackTotal;
    item.disc = td.disc;
    item.discTotal = td.discTotal;
    item.duration = td.lengthSec;
    item.bitrate = td.bitrate;
    item.sampleRate = td.sampleRate;
    item.channels = td.channels;
    item.rating = td.rating;
  }

  AddOrUpdateEntry(item);
  _ScheduleSave();

  if (fTarget.IsValid()) {
    BMessage update(MSG_MEDIA_ITEM_FOUND);
    update.AddString("path", item.path);
    update.AddString("title", item.title);
    update.AddString("artist", item.artist);
    update.AddString("album", item.album);
    update.AddString("genre", item.genre);
    update.AddString("comment", item.comment);
    update.AddString("albumArtist", item.albumArtist);
    update.AddString("composer", item.composer);
    update.AddInt32("year", item.year);
    update.AddInt32("track", item.track);
    update.AddInt32("trackTotal", item.trackTotal);
    update.AddInt32("disc", item.disc);
    update.AddInt32("discTotal", item.discTotal);
    update.AddInt32("rating", item.rating);
    update.AddInt32("duration", item.duration);
    update.AddInt32("bitrate", item.bitrate);
    fTarget.SendMessage(&update);
  }
}

void MediaLibraryCache::_RemoveFileFromCache(const BString &pathStr) {
  auto it = fEntries.find(pathStr);
  if (it != fEntries.end()) {
    fEntries.erase(it);
    _ScheduleSave();

    if (fTarget.IsValid()) {
      BMessage removed(MSG_MEDIA_ITEM_REMOVED);
      removed.AddString("path", pathStr);
      fTarget.SendMessage(&removed);
    }
  }
}

bool MediaLibraryCache::_IsSupportedAudioFile(const BString &path) {
  BString lower(path);
  lower.ToLower();

  static const char *exts[] = {".mp3", ".wav", ".flac", ".ogg",
                               ".opus", ".m4a", ".aac",  ".wma"
#if ENABLE_MIDI_PLAYBACK
                               ,
                               ".mid", ".midi"
#endif
  };

  for (auto ext : exts) {
    if (lower.EndsWith(ext))
      return true;
  }
  return false;
}

/** @brief Delay before a throttled cache write-back runs. */
static const bigtime_t kSaveThrottleDelay = 1500000; // 1.5 s

void MediaLibraryCache::_ScheduleSave() {
  fCacheDirty = true;

  if (fFileScanRunning)
    return; // The queue drain writes the cache once when it finishes.

  if (fSaveThrottle != nullptr)
    return; // A write-back is already pending; this change rides along.

  BMessage flush(MSG_CACHE_FLUSH);
  fSaveThrottle =
      new BMessageRunner(BMessenger(this), &flush, kSaveThrottleDelay, 1);
  if (fSaveThrottle->InitCheck() != B_OK) {
    // Could not arm the runner: fall back to writing straight through rather
    // than risk losing the change entirely.
    delete fSaveThrottle;
    fSaveThrottle = nullptr;
    SaveCache();
    fCacheDirty = false;
  }
}

void MediaLibraryCache::_DestroyScanner(int32 scanId) {
  auto it = fScanners.find(scanId);
  if (it == fScanners.end())
    return;

  MediaLibraryScanner *scanner = it->second;
  fScanners.erase(it);

  // In the normal case the worker has already returned by the time its
  // MSG_SCAN_DONE reaches us, so this costs nothing.
  scanner->RequestStop();
  scanner->WaitForStop();

  // Quit() from another thread posts _QUIT_ and blocks until the looper
  // thread has exited, so the object is gone for good when this returns.
  if (scanner->Lock())
    scanner->Quit();
}

void MediaLibraryCache::_StopAllScanners() {
  if (fScanners.empty()) {
    // fActiveScanners is counted independently of this map, so the two
    // disagreeing means a scanner was launched without being registered here
    // — and that scanner's worker thread would outlive us.
    if (fActiveScanners > 0) {
      DEBUG_PRINT("WARNING: %ld scanner(s) running but none tracked; "
                  "their threads cannot be stopped\n",
                  (long)fActiveScanners);
    }
    return;
  }

  DEBUG_PRINT("stopping %zu scanner(s) still running\n", fScanners.size());

  // Two passes on purpose: signal everything first, so no worker is still
  // sending batches to this looper while it sits in wait_for_thread below.
  for (auto &kv : fScanners)
    kv.second->RequestStop();

  for (auto &kv : fScanners) {
    kv.second->WaitForStop();
    if (kv.second->Lock())
      kv.second->Quit();
  }

  fScanners.clear();
  fActiveScanners = 0;
}

void MediaLibraryCache::_BeginScanCycle() {
  if (fScanCycleActive)
    return;

  fScanCycleActive = true;
  fScanStartTime = system_time();
  fLastProgressSent = 0;
  fScanCounters.clear();
  fQueuedPlaylists = 0;
  fQueuedFilesDone = 0;
}

bool MediaLibraryCache::_NeedsScan(const BString &path) const {
  struct stat st;
  if (stat(path.String(), &st) != 0)
    return false; // Gone or unreachable: not work this scan can do.

  auto it = fEntries.find(path);
  if (it == fEntries.end())
    return true;

  return it->second.mtime != (int64)st.st_mtime ||
         it->second.size != (int64)st.st_size;
}

void MediaLibraryCache::_PrepareFileScanQueue() {
  std::vector<BString> needed;
  needed.reserve(fPendingFileScans.size());

  std::set<int32> playlistsWithWork;
  std::set<BString> seen;

  for (size_t i = 0; i < fPendingFileScans.size(); i++) {
    const BString &path = fPendingFileScans[i];
    if (!_NeedsScan(path))
      continue;

    // A track shared by two playlists is scanned once but counts as work for
    // both, so the playlist figure reflects what the user would expect.
    if (i < fPendingFileScanPlaylists.size())
      playlistsWithWork.insert(fPendingFileScanPlaylists[i]);

    if (seen.insert(path).second)
      needed.push_back(path);
  }

  fPendingFileScans.swap(needed);
  fPendingFileScanPlaylists.clear();
  fFileScanIndex = 0;
  fQueuedPlaylists = (int32)playlistsWithWork.size();
  fQueuedFilesDone = 0;

  DEBUG_PRINT("File queue: %zu tracks need scanning across %ld playlist(s)\n",
              fPendingFileScans.size(), (long)fQueuedPlaylists);
}

void MediaLibraryCache::_EmitScanProgress(bool force) {
  if (!fTarget.IsValid())
    return;

  const bigtime_t now = system_time();
  if (!force && fLastProgressSent != 0 &&
      (now - fLastProgressSent) < 200000) {
    return; // At most five updates a second; the UI ticks on its own between.
  }
  fLastProgressSent = now;

  int32 folders = 0;
  int32 filesTotal = 0;
  int32 filesDone = 0;
  for (const auto &kv : fScanCounters) {
    folders += kv.second.folders;
    filesTotal += kv.second.filesTotal;
    filesDone += kv.second.filesDone;
  }

  filesTotal += (int32)fPendingFileScans.size();
  filesDone += fQueuedFilesDone;

  BMessage progress(MSG_SCAN_PROGRESS);
  progress.AddInt32("folders", folders);
  progress.AddInt32("playlists", fQueuedPlaylists);
  progress.AddInt32("files_done", std::min(filesDone, filesTotal));
  progress.AddInt32("files_total", filesTotal);
  progress.AddInt64("elapsed_sec", (int64)((now - fScanStartTime) / 1000000));
  fTarget.SendMessage(&progress);
}

void MediaLibraryCache::_QueueFileScans(BMessage *msg) {
  BString path;
  size_t added = 0;
  for (int32 i = 0; msg->FindString("path", i, &path) == B_OK; ++i) {
    if (path.IsEmpty())
      continue;

    int32 playlist = 0;
    msg->FindInt32("playlist", i, &playlist);

    fPendingFileScans.push_back(path);
    fPendingFileScanPlaylists.push_back(playlist);
    added++;
  }

  if (added == 0)
    return;

  DEBUG_PRINT("Queued %zu files for scanning (%zu pending)\n", added,
              fPendingFileScans.size() - fFileScanIndex);

  // While scanners are running, the queue is drained from the MSG_SCAN_DONE
  // handler instead, so the two do not compete for this looper thread.
  if (fActiveScanners == 0 && !fFileScanRunning)
    _FinishScanCycle();
}

void MediaLibraryCache::_ProcessFileScanChunk() {
  size_t scanned = 0;

  while (fFileScanIndex < fPendingFileScans.size() && scanned < kFileScanChunk) {
    const BString path = fPendingFileScans[fFileScanIndex++];

    // Already covered by a folder scan, or simply gone: nothing to read.
    if (fEntries.find(path) != fEntries.end())
      continue;
    if (!BEntry(path.String()).Exists())
      continue;

    _ScanAndAddFile(path);
    scanned++;
    fQueuedFilesDone++;
  }

  if (fFileScanIndex < fPendingFileScans.size()) {
    _EmitScanProgress();
    // Yield: let scanner batches and node-monitor events through between
    // chunks rather than holding the looper for the whole queue.
    PostMessage(MSG_SCAN_FILES_STEP);
    return;
  }

  DEBUG_PRINT("File scan queue drained (%zu paths)\n",
              fPendingFileScans.size());

  fPendingFileScans.clear();
  fPendingFileScanPlaylists.clear();
  fFileScanIndex = 0;
  fFileScanRunning = false;
  fScanCycleActive = false;

  if (fCacheDirty)
    SaveCache();

  if (fTarget.IsValid()) {
    BMessage done(MSG_SCAN_DONE);
    done.AddBool("final", true);
    fTarget.SendMessage(&done);
  }
}

void MediaLibraryCache::_ReconcileFolderDeletions(const BString &folderPath) {
  BString prefix = folderPath;
  if (!prefix.EndsWith("/"))
    prefix << "/";

  bool removedAny = false;

  for (auto it = fEntries.begin(); it != fEntries.end();) {
    const BString &path = it->first;
    if (path != folderPath && !path.StartsWith(prefix)) {
      ++it;
      continue;
    }

    BEntry entry(path.String());
    if (entry.Exists()) {
      ++it;
      continue;
    }

    BString gonePath = path; // Copy: `path` refers into the node we erase.
    DEBUG_PRINT("Folder reconcile: dropping missing file %s\n",
                gonePath.String());

    it = fEntries.erase(it);
    removedAny = true;

    if (fTarget.IsValid()) {
      BMessage gone(MSG_MEDIA_ITEM_REMOVED);
      gone.AddString("path", gonePath);
      fTarget.SendMessage(&gone);
    }
  }

  if (removedAny)
    _ScheduleSave();
}
