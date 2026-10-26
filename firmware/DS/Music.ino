//   Music.ino
//   Native, full-screen MP3 library/player for /sd/music. The catalog and its
//   filtered index live entirely in PSRAM; playback reuses Radio.ino's one Audio
//   engine, task, ES8311 setup, DMA ring, and volume state.

#include <SD_MMC.h>
#include <esp_heap_caps.h>

static MusicTrack* musicTracks = nullptr;
static uint16_t* musicFiltered = nullptr;
static MusicAlbum* musicAlbums = nullptr;
static MusicArtist* musicArtists = nullptr;
static int musicTrackCount = 0;
static int musicAlbumCount = 0;
static int musicArtistCount = 0;
static int musicFilteredCount = 0;

//Browser position. musicScopeArtist is the artist whose albums are listed (-1 = all
//albums), musicOpenAlbum the album whose tracks are listed (-1 = all tracks or a search
//result). Together they say where Escape goes, so no separate back-stack is needed.
//musicSelection remembers the cursor per level, so descending and coming back lands on
//the row you left rather than the top.
static MusicView musicView = MUSIC_VIEW_ROOT;
static int musicScopeArtist = -1;
static int musicOpenAlbum = -1;
static int musicSelection[MUSIC_VIEW_COUNT] = {0, 0, 0, 0};

static int& musicSelected() { return musicSelection[musicView]; }
static volatile int musicCurrentTrack = -1;
static volatile bool musicLibraryReady = false;
static bool musicLibraryTruncated = false;
static volatile bool musicSuppressAutoAdvance = false;
static String musicFilter = "";

static MusicKeyState musicTelnetKeys;
static MusicKeyState musicKeyboardKeys;

static uint32_t musicSyncsafe32(const uint8_t* p) {
    return ((uint32_t)(p[0] & 0x7f) << 21)
         | ((uint32_t)(p[1] & 0x7f) << 14)
         | ((uint32_t)(p[2] & 0x7f) << 7)
         | (uint32_t)(p[3] & 0x7f);
}

static uint32_t musicBigEndian32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16)
         | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static uint32_t musicBigEndian24(const uint8_t* p) {
    return ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | (uint32_t)p[2];
}

static void musicTrimField(char* text) {
    if (!text) return;
    size_t len = strlen(text);
    while (len > 0 && (text[len - 1] == ' ' || text[len - 1] == '\t'
                       || text[len - 1] == '\r' || text[len - 1] == '\n')) {
        text[--len] = '\0';
    }
    size_t first = 0;
    while (text[first] == ' ' || text[first] == '\t') first++;
    if (first > 0) memmove(text, text + first, strlen(text + first) + 1);
}

static void musicAppendCodepoint(char* out, size_t outSize, size_t& used, uint32_t cp) {
    if (cp == 0 || used + 1 >= outSize) return;
    if (cp < 0x80) {
        out[used++] = (char)cp;
    } else if (cp < 0x800 && used + 2 < outSize) {
        out[used++] = (char)(0xc0 | (cp >> 6));
        out[used++] = (char)(0x80 | (cp & 0x3f));
    } else if (used + 3 < outSize) {
        out[used++] = (char)(0xe0 | (cp >> 12));
        out[used++] = (char)(0x80 | ((cp >> 6) & 0x3f));
        out[used++] = (char)(0x80 | (cp & 0x3f));
    }
    out[used] = '\0';
}

//Decode the four ID3 text encodings into the UTF-8 strings TFT_eSPI and telnet already use.
static void musicDecodeId3Text(const uint8_t* data, size_t length, char* out, size_t outSize) {
    if (!out || outSize == 0) return;
    out[0] = '\0';
    if (!data || length < 2) return;

    uint8_t encoding = data[0];
    size_t used = 0;
    if (encoding == 0 || encoding == 3) {
        for (size_t i = 1; i < length && data[i] != 0 && used + 1 < outSize; i++) {
            uint8_t b = data[i];
            if (encoding == 0 && b >= 0x80) {
                musicAppendCodepoint(out, outSize, used, b);   //ISO-8859-1 -> UTF-8
            } else {
                out[used++] = (char)b;
                out[used] = '\0';
            }
        }
    } else if (encoding == 1 || encoding == 2) {
        size_t i = 1;
        bool bigEndian = encoding == 2;
        if (i + 1 < length && data[i] == 0xff && data[i + 1] == 0xfe) {
            bigEndian = false;
            i += 2;
        } else if (i + 1 < length && data[i] == 0xfe && data[i + 1] == 0xff) {
            bigEndian = true;
            i += 2;
        }
        while (i + 1 < length) {
            uint16_t cp = bigEndian ? ((uint16_t)data[i] << 8) | data[i + 1]
                                    : ((uint16_t)data[i + 1] << 8) | data[i];
            i += 2;
            if (cp == 0) break;
            //Astral-plane surrogate pairs are rare in music tags; keep the catalog valid
            //UTF-8 without spending another large conversion buffer on them.
            if (cp >= 0xd800 && cp <= 0xdfff) cp = '?';
            musicAppendCodepoint(out, outSize, used, cp);
        }
    }
    musicTrimField(out);
}

static void musicCopyLatin1Field(const uint8_t* data, size_t length, char* out, size_t outSize) {
    if (!out || outSize == 0) return;
    out[0] = '\0';
    size_t used = 0;
    for (size_t i = 0; i < length && data[i] != 0; i++) {
        musicAppendCodepoint(out, outSize, used, data[i]);
    }
    musicTrimField(out);
}

static uint16_t musicParseTrackNumber(const char* text) {
    if (!text) return 0;
    long value = strtol(text, nullptr, 10);
    return (value > 0 && value <= 65535) ? (uint16_t)value : 0;
}

static bool musicReadExact(File& file, uint32_t position, uint8_t* data, size_t length) {
    return file.seek(position) && file.read(data, length) == length;
}

static void musicReadId3v1(File& file, MusicTrack& track) {
    size_t size = file.size();
    if (size < 128) return;
    uint8_t tag[128];
    if (!musicReadExact(file, size - sizeof(tag), tag, sizeof(tag)) || memcmp(tag, "TAG", 3) != 0) return;

    if (track.title[0] == '\0') musicCopyLatin1Field(tag + 3, 30, track.title, sizeof(track.title));
    if (track.artist[0] == '\0') musicCopyLatin1Field(tag + 33, 30, track.artist, sizeof(track.artist));
    if (track.album[0] == '\0') musicCopyLatin1Field(tag + 63, 30, track.album, sizeof(track.album));
    if (track.trackNumber == 0 && tag[125] == 0 && tag[126] != 0) track.trackNumber = tag[126];
}

static bool musicFrameIdEquals(const char* id, const char* v22, const char* v23) {
    return (v22 && strncmp(id, v22, 3) == 0 && id[3] == '\0')
        || (v23 && strncmp(id, v23, 4) == 0);
}

static void musicReadId3v2(File& file, MusicTrack& track) {
    uint8_t header[10];
    if (!musicReadExact(file, 0, header, sizeof(header)) || memcmp(header, "ID3", 3) != 0) return;
    uint8_t version = header[3];
    if (version < 2 || version > 4) return;

    uint32_t tagEnd = 10 + musicSyncsafe32(header + 6);
    if (tagEnd > file.size()) tagEnd = file.size();
    uint32_t position = 10;
    uint8_t frameHeader[10];
    uint8_t textData[512];

    //Skip the optional extended header before walking normal frames. ID3v2.3's
    //length excludes its own four-byte size field; v2.4's includes it.
    if ((header[5] & 0x40) && version >= 3 && position + 4 <= tagEnd
        && musicReadExact(file, position, frameHeader, 4)) {
        uint32_t extendedSize = version == 4 ? musicSyncsafe32(frameHeader)
                                             : musicBigEndian32(frameHeader) + 4;
        if (extendedSize >= 4 && extendedSize <= tagEnd - position) position += extendedSize;
    }

    while (position + (version == 2 ? 6U : 10U) <= tagEnd) {
        size_t headerSize = version == 2 ? 6 : 10;
        if (!musicReadExact(file, position, frameHeader, headerSize)) break;
        if (frameHeader[0] == 0) break;   //padding

        char id[5] = "";
        size_t idLength = version == 2 ? 3 : 4;
        memcpy(id, frameHeader, idLength);
        uint32_t frameSize = version == 2 ? musicBigEndian24(frameHeader + 3)
                           : version == 4 ? musicSyncsafe32(frameHeader + 4)
                                          : musicBigEndian32(frameHeader + 4);
        uint32_t dataPosition = position + headerSize;
        if (frameSize == 0 || dataPosition > tagEnd || frameSize > tagEnd - dataPosition) break;

        char* destination = nullptr;
        bool isTrack = false;
        if (musicFrameIdEquals(id, "TT2", "TIT2")) destination = track.title;
        else if (musicFrameIdEquals(id, "TP1", "TPE1")) destination = track.artist;
        else if (musicFrameIdEquals(id, "TAL", "TALB")) destination = track.album;
        else if (musicFrameIdEquals(id, "TRK", "TRCK")) isTrack = true;

        if ((destination || isTrack) && frameSize > 1) {
            size_t toRead = min((size_t)frameSize, sizeof(textData));
            if (musicReadExact(file, dataPosition, textData, toRead)) {
                char decoded[MUSIC_METADATA_MAX];
                musicDecodeId3Text(textData, toRead, decoded, sizeof(decoded));
                if (destination && decoded[0] != '\0') {
                    strncpy(destination, decoded, MUSIC_METADATA_MAX - 1);
                    destination[MUSIC_METADATA_MAX - 1] = '\0';
                } else if (isTrack) {
                    track.trackNumber = musicParseTrackNumber(decoded);
                }
            }
        }
        position = dataPosition + frameSize;
    }
}

static String musicBaseName(String path) {
    int slash = path.lastIndexOf('/');
    if (slash >= 0) path = path.substring(slash + 1);
    return path;
}

static String musicJoinPath(const String& parent, const String& child) {
    if (parent.length() == 0 || parent == "/") return "/" + child;
    return parent + "/" + child;
}

static void musicSetFallbackTitle(MusicTrack& track) {
    if (track.title[0] != '\0') return;
    String name = musicBaseName(track.path);
    if (name.endsWith(".mp3") || name.endsWith(".MP3")) name.remove(name.length() - 4);
    strncpy(track.title, name.c_str(), sizeof(track.title) - 1);
    track.title[sizeof(track.title) - 1] = '\0';
}

static void musicCopyField(char* destination, const String& value) {
    strncpy(destination, value.c_str(), MUSIC_METADATA_MAX - 1);
    destination[MUSIC_METADATA_MAX - 1] = '\0';
}

//Tags win wherever they exist, but an untagged card leaves artist and album empty on
//every track, so musicCompareTracks ties on its first three keys and the whole library
//degenerates into one alphabetical list of every file with folders interleaved. Untagged
//collections are near-always foldered, so read the missing fields back out of the
//directory names: <root>/<artist>/<album>/track.mp3, or <root>/<album>/track.mp3 where
//there's only one level (an album folder is the commoner single level than an artist
//one, and guessing album still groups it correctly). Anything deeper takes the two
//directories nearest the file and ignores the rest. Empty fields only -- this never
//overwrites a real tag, and the two are filled independently, so a track carrying TALB
//but no TPE1 still picks up its artist folder.
static void musicApplyFolderMetadata(MusicTrack& track) {
    if (track.artist[0] != '\0' && track.album[0] != '\0') return;

    const String root = "/sd/music/";
    String path = track.path;
    if (!path.startsWith(root)) return;

    String relative = path.substring(root.length());
    int fileSlash = relative.lastIndexOf('/');
    if (fileSlash < 0) return;                     //loose file sitting at the library root
    String directories = relative.substring(0, fileSlash);

    int parentSlash = directories.lastIndexOf('/');
    String parent = directories.substring(parentSlash + 1);
    String grandparent = "";
    if (parentSlash > 0) {
        String above = directories.substring(0, parentSlash);
        grandparent = above.substring(above.lastIndexOf('/') + 1);
    }

    if (track.album[0] == '\0' && parent.length() > 0) musicCopyField(track.album, parent);
    if (track.artist[0] == '\0' && grandparent.length() > 0) musicCopyField(track.artist, grandparent);
}

static bool musicIsMp3Name(String name) {
    name.toLowerCase();
    return name.endsWith(".mp3");
}

static int musicCompareTextCI(const char* a, const char* b) {
    while (*a && *b) {
        int ca = tolower((unsigned char)*a++);
        int cb = tolower((unsigned char)*b++);
        if (ca != cb) return ca - cb;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

static int musicCompareTracks(const void* left, const void* right) {
    const MusicTrack* a = (const MusicTrack*)left;
    const MusicTrack* b = (const MusicTrack*)right;
    int compared = musicCompareTextCI(a->artist, b->artist);
    if (compared != 0) return compared;
    compared = musicCompareTextCI(a->album, b->album);
    if (compared != 0) return compared;
    if (a->trackNumber != b->trackNumber && (a->trackNumber || b->trackNumber)) {
        if (!a->trackNumber) return 1;
        if (!b->trackNumber) return -1;
        return (int)a->trackNumber - (int)b->trackNumber;
    }
    compared = musicCompareTextCI(a->title, b->title);
    return compared != 0 ? compared : musicCompareTextCI(a->path, b->path);
}

static bool musicAllocateLibrary() {
    if (musicTracks && musicFiltered) return true;
    if (!psramFound()) {
        outLine("music: PSRAM is required for the library", C_RED);
        return false;
    }
    musicTracks = (MusicTrack*)heap_caps_calloc(
        MUSIC_LIBRARY_MAX_TRACKS, sizeof(MusicTrack), MALLOC_CAP_SPIRAM);
    musicFiltered = (uint16_t*)heap_caps_calloc(
        MUSIC_LIBRARY_MAX_TRACKS, sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    //Worst case every track is its own album by its own artist, so both indexes are
    //sized to the track cap; at 1024 entries that is 4KB + 8KB, cheap next to the catalog.
    musicAlbums = (MusicAlbum*)heap_caps_calloc(
        MUSIC_LIBRARY_MAX_TRACKS, sizeof(MusicAlbum), MALLOC_CAP_SPIRAM);
    musicArtists = (MusicArtist*)heap_caps_calloc(
        MUSIC_LIBRARY_MAX_TRACKS, sizeof(MusicArtist), MALLOC_CAP_SPIRAM);
    if (!musicTracks || !musicFiltered || !musicAlbums || !musicArtists) {
        if (musicTracks) heap_caps_free(musicTracks);
        if (musicFiltered) heap_caps_free(musicFiltered);
        if (musicAlbums) heap_caps_free(musicAlbums);
        if (musicArtists) heap_caps_free(musicArtists);
        musicTracks = nullptr;
        musicFiltered = nullptr;
        musicAlbums = nullptr;
        musicArtists = nullptr;
        outLine("music: not enough PSRAM for the library", C_RED);
        return false;
    }
    Serial.printf("[music] library: %u bytes + %u-byte filter index + %u-byte browse index -> PSRAM\n",
                  (unsigned)(MUSIC_LIBRARY_MAX_TRACKS * sizeof(MusicTrack)),
                  (unsigned)(MUSIC_LIBRARY_MAX_TRACKS * sizeof(uint16_t)),
                  (unsigned)(MUSIC_LIBRARY_MAX_TRACKS * (sizeof(MusicAlbum) + sizeof(MusicArtist))));
    return true;
}

//One pass over the sorted catalog builds both spans: a new album whenever artist+album
//changes, a new artist whenever artist changes (which is always also an album boundary,
//so the artist's albums are the contiguous run starting at the album open at that point).
static void musicBuildBrowseIndexes() {
    musicAlbumCount = 0;
    musicArtistCount = 0;
    if (!musicAlbums || !musicArtists) return;

    for (int i = 0; i < musicTrackCount; i++) {
        const MusicTrack& track = musicTracks[i];
        bool newArtist = musicArtistCount == 0
            || musicCompareTextCI(musicTracks[musicArtists[musicArtistCount - 1].firstTrack].artist,
                                  track.artist) != 0;
        bool newAlbum = newArtist || musicAlbumCount == 0
            || musicCompareTextCI(musicTracks[musicAlbums[musicAlbumCount - 1].firstTrack].album,
                                  track.album) != 0;

        if (newAlbum) {
            musicAlbums[musicAlbumCount].firstTrack = (uint16_t)i;
            musicAlbums[musicAlbumCount].trackCount = 0;
            musicAlbumCount++;
        }
        if (newArtist) {
            musicArtists[musicArtistCount].firstAlbum = (uint16_t)(musicAlbumCount - 1);
            musicArtists[musicArtistCount].albumCount = 0;
            musicArtists[musicArtistCount].firstTrack = (uint16_t)i;
            musicArtists[musicArtistCount].trackCount = 0;
            musicArtistCount++;
        }
        musicAlbums[musicAlbumCount - 1].trackCount++;
        musicArtists[musicArtistCount - 1].trackCount++;
        musicArtists[musicArtistCount - 1].albumCount =
            (uint16_t)(musicAlbumCount - musicArtists[musicArtistCount - 1].firstAlbum);
    }
}

static int musicAlbumForTrack(int trackIndex) {
    for (int i = 0; i < musicAlbumCount; i++) {
        if (trackIndex >= musicAlbums[i].firstTrack
            && trackIndex < musicAlbums[i].firstTrack + musicAlbums[i].trackCount) return i;
    }
    return -1;
}

static int musicArtistForAlbum(int albumIndex) {
    for (int i = 0; i < musicArtistCount; i++) {
        if (albumIndex >= musicArtists[i].firstAlbum
            && albumIndex < musicArtists[i].firstAlbum + musicArtists[i].albumCount) return i;
    }
    return -1;
}

//Empty artist/album tags are normal on a card the folder fallback couldn't help either
//(loose files at the library root), so both lists need a name for the blank bucket.
static const char* musicArtistName(const MusicTrack& track) {
    return track.artist[0] ? track.artist : "(no artist)";
}

static const char* musicAlbumName(const MusicTrack& track) {
    return track.album[0] ? track.album : "(no album)";
}

static void musicScanDirectory(fs::FS& fs, const String& realDirectory,
                               const String& logicalDirectory, int depth) {
    if (musicTrackCount >= MUSIC_LIBRARY_MAX_TRACKS || depth > 16) {
        musicLibraryTruncated = true;
        return;
    }
    ledPulseStorageRead(true);
    File directory = fs.open(realDirectory);
    if (!directory || !directory.isDirectory()) {
        if (directory) directory.close();
        return;
    }

    File entry = directory.openNextFile();
    while (entry && musicTrackCount < MUSIC_LIBRARY_MAX_TRACKS) {
        ledPulseStorageRead(true);
        String name = musicBaseName(entry.name());
        bool isDirectory = entry.isDirectory();
        if (name.length() > 0 && name != "." && name != "..") {
            String realPath = musicJoinPath(realDirectory, name);
            String logicalPath = musicJoinPath(logicalDirectory, name);
            if (isDirectory) {
                entry.close();
                musicScanDirectory(fs, realPath, logicalPath, depth + 1);
            } else if (musicIsMp3Name(name)) {
                if (logicalPath.length() < MUSIC_PATH_MAX) {
                    MusicTrack& track = musicTracks[musicTrackCount];
                    strncpy(track.path, logicalPath.c_str(), sizeof(track.path) - 1);
                    track.fileSize = entry.size();
                    musicReadId3v2(entry, track);
                    musicReadId3v1(entry, track);
                    musicSetFallbackTitle(track);
                    musicApplyFolderMetadata(track);
                    musicTrackCount++;
                } else {
                    musicLibraryTruncated = true;
                }
                entry.close();
                if ((musicTrackCount & 7) == 0) delay(1);   //feed IDLE/WDT during large scans
            } else {
                entry.close();
            }
        } else {
            entry.close();
        }
        entry = directory.openNextFile();
    }
    if (entry) {
        musicLibraryTruncated = true;
        entry.close();
    }
    directory.close();
}

static bool musicContainsCI(const char* haystack, const char* needle) {
    if (!needle || needle[0] == '\0') return true;
    if (!haystack) return false;
    for (const char* start = haystack; *start; start++) {
        const char* h = start;
        const char* n = needle;
        while (*h && *n && tolower((unsigned char)*h) == tolower((unsigned char)*n)) {
            h++;
            n++;
        }
        if (*n == '\0') return true;
    }
    return false;
}

static void musicRebuildFilter(const String& filter) {
    musicFilter = filter;
    musicFilteredCount = 0;
    const char* query = musicFilter.c_str();
    for (int i = 0; i < musicTrackCount; i++) {
        const MusicTrack& track = musicTracks[i];
        if (musicContainsCI(track.title, query) || musicContainsCI(track.artist, query)
            || musicContainsCI(track.album, query) || musicContainsCI(track.path, query)) {
            musicFiltered[musicFilteredCount++] = (uint16_t)i;
        }
    }
}

static void musicStopLocalAndWait() {
    RadioState state;
    bool isLocal;
    uint32_t current, duration;
    radioGetPlaybackSnapshot(state, isLocal, nullptr, 0, nullptr, 0, nullptr, 0, current, duration);
    if (!isLocal) return;
    musicSuppressAutoAdvance = true;
    radioStopPlayback();
    for (int waited = 0; waited < 1000; waited += 10) {
        delay(10);
        radioGetPlaybackSnapshot(state, isLocal, nullptr, 0, nullptr, 0, nullptr, 0, current, duration);
        if (!isLocal || state == RADIO_STOPPED) break;
    }
    radioTakeLocalEof();
    musicSuppressAutoAdvance = false;
}

static bool musicScanLibrary() {
    if (!sdCardMounted) {
        outLine("music: SD not mounted (insert card and reboot)", C_RED);
        return false;
    }
    if (!musicAllocateLibrary()) return false;

    musicStopLocalAndWait();
    musicCurrentTrack = -1;
    musicTrackCount = 0;
    musicAlbumCount = 0;
    musicArtistCount = 0;
    musicFilteredCount = 0;
    //the old spans point into a catalog that is about to be refilled
    musicView = MUSIC_VIEW_ROOT;
    musicScopeArtist = -1;
    musicOpenAlbum = -1;
    for (int i = 0; i < MUSIC_VIEW_COUNT; i++) musicSelection[i] = 0;
    musicLibraryTruncated = false;
    musicLibraryReady = false;
    memset(musicTracks, 0, MUSIC_LIBRARY_MAX_TRACKS * sizeof(MusicTrack));

    File root = SD_MMC.open("/music");
    if (!root || !root.isDirectory()) {
        if (root) root.close();
        outLine("music: /sd/music does not exist", C_RED);
        return false;
    }
    root.close();

    Serial.println("[music] scanning /sd/music recursively...");
    musicScanDirectory(SD_MMC, "/music", "/sd/music", 0);
    if (musicTrackCount > 1) {
        qsort(musicTracks, musicTrackCount, sizeof(MusicTrack), musicCompareTracks);
    }
    musicBuildBrowseIndexes();   //spans depend on the sort, so this has to follow qsort
    musicLibraryReady = true;
    musicRebuildFilter("");
    Serial.printf("[music] library ready: %d track(s), %d album(s), %d artist(s)%s\n",
                  musicTrackCount, musicAlbumCount, musicArtistCount,
                  musicLibraryTruncated ? " (truncated)" : "");
    return true;
}

static bool musicEnsureLibrary() {
    return musicLibraryReady || musicScanLibrary();
}

static bool musicPlayTrack(int index) {
    if (index < 0 || index >= musicTrackCount) return false;
    RoutedPath routed = routePath(musicTracks[index].path);
    if (!routed.isSd || !sdCardMounted) return false;
    musicSuppressAutoAdvance = false;
    radioTakeLocalEof();
    if (!radioPlayLocalFile(routed.realPath.c_str())) return false;
    musicCurrentTrack = index;
    return true;
}

static bool musicAdvance(int delta) {
    if (!musicLibraryReady || musicTrackCount == 0) return false;
    int next = musicCurrentTrack;
    if (next < 0) next = delta < 0 ? musicTrackCount - 1 : 0;
    else next = (next + delta + musicTrackCount) % musicTrackCount;
    return musicPlayTrack(next);
}

//Radio.ino calls this from its existing playback worker after a natural EOF. It only
//copies fixed PSRAM-backed state -- no String allocation, drawing, filesystem access,
//or shell output from the task. The returned path is SD_MMC-native (/music/...), so the
//worker can open it directly and albums continue through edit/SSH/other modal screens.
bool musicNextFileForAudioTask(char* realPath, size_t realPathSize) {
    if (!realPath || realPathSize == 0 || musicSuppressAutoAdvance
        || !musicLibraryReady || musicTrackCount <= 0 || musicCurrentTrack < 0) {
        return false;
    }
    int next = (musicCurrentTrack + 1) % musicTrackCount;
    const char* logical = musicTracks[next].path;
    if (strncmp(logical, "/sd/", 4) != 0) return false;
    strncpy(realPath, logical + 3, realPathSize - 1);   //keep the slash: /sd/music -> /music
    realPath[realPathSize - 1] = '\0';
    musicCurrentTrack = next;
    return true;
}

static int musicFilteredPositionForTrack(int trackIndex) {
    for (int i = 0; i < musicFilteredCount; i++) {
        if (musicFiltered[i] == trackIndex) return i;
    }
    return -1;
}

static void musicTrackLabel(const MusicTrack& track, char* out, size_t outSize) {
    if (track.artist[0] != '\0') snprintf(out, outSize, "%s - %s", track.artist, track.title);
    else snprintf(out, outSize, "%s", track.title);
}

//   ---- row model ---------------------------------------------------------------
//
//   Both renderers (TFT and telnet) draw the same rows, so the view branching lives here
//   once instead of twice. A row is identified by its position in whatever list is on
//   screen; these three functions turn that into a count, a label, and whether the row
//   contains what is currently playing.

//The album list is scoped to one artist when reached through Artists, and unscoped when
//reached from the root, so its rows are a window into musicAlbums either way.
static int musicAlbumRowBase() {
    return (musicScopeArtist >= 0 && musicScopeArtist < musicArtistCount)
        ? musicArtists[musicScopeArtist].firstAlbum : 0;
}

static int musicRowCount() {
    switch (musicView) {
        case MUSIC_VIEW_ROOT:    return MUSIC_ROOT_ROWS;
        case MUSIC_VIEW_ARTISTS: return musicArtistCount;
        case MUSIC_VIEW_ALBUMS:
            return (musicScopeArtist >= 0 && musicScopeArtist < musicArtistCount)
                ? musicArtists[musicScopeArtist].albumCount : musicAlbumCount;
        default:                 return musicFilteredCount;
    }
}

static void musicRowLabel(int position, char* out, size_t outSize) {
    out[0] = '\0';
    if (position < 0 || position >= musicRowCount()) return;

    if (musicView == MUSIC_VIEW_ROOT) {
        if (position == 0)      snprintf(out, outSize, "Artists   (%d)", musicArtistCount);
        else if (position == 1) snprintf(out, outSize, "Albums    (%d)", musicAlbumCount);
        else                    snprintf(out, outSize, "Tracks    (%d)", musicTrackCount);
        return;
    }

    if (musicView == MUSIC_VIEW_ARTISTS) {
        const MusicArtist& artist = musicArtists[position];
        snprintf(out, outSize, "%s  (%u)", musicArtistName(musicTracks[artist.firstTrack]),
                 (unsigned)artist.albumCount);
        return;
    }

    if (musicView == MUSIC_VIEW_ALBUMS) {
        const MusicAlbum& album = musicAlbums[musicAlbumRowBase() + position];
        const MusicTrack& track = musicTracks[album.firstTrack];
        //inside an artist the name is redundant on every row; unscoped it is the only
        //thing telling two same-titled albums apart
        if (musicScopeArtist >= 0) {
            snprintf(out, outSize, "%s  (%u)", musicAlbumName(track), (unsigned)album.trackCount);
        } else {
            snprintf(out, outSize, "%s - %s  (%u)", musicArtistName(track),
                     musicAlbumName(track), (unsigned)album.trackCount);
        }
        return;
    }

    const MusicTrack& track = musicTracks[musicFiltered[position]];
    if (musicOpenAlbum >= 0 && track.trackNumber > 0) {
        snprintf(out, outSize, "%02u  %s", (unsigned)track.trackNumber, track.title);
    } else if (musicOpenAlbum >= 0) {
        snprintf(out, outSize, "%s", track.title);
    } else {
        musicTrackLabel(track, out, outSize);   //all-tracks and search results need the artist
    }
}

static bool musicRowIsPlaying(int position, bool local) {
    if (!local || musicCurrentTrack < 0 || position < 0 || position >= musicRowCount()) return false;
    switch (musicView) {
        case MUSIC_VIEW_ROOT:
            return false;
        case MUSIC_VIEW_ARTISTS: {
            const MusicArtist& artist = musicArtists[position];
            return musicCurrentTrack >= artist.firstTrack
                && musicCurrentTrack < artist.firstTrack + artist.trackCount;
        }
        case MUSIC_VIEW_ALBUMS: {
            const MusicAlbum& album = musicAlbums[musicAlbumRowBase() + position];
            return musicCurrentTrack >= album.firstTrack
                && musicCurrentTrack < album.firstTrack + album.trackCount;
        }
        default:
            return musicFiltered[position] == musicCurrentTrack;
    }
}

//Breadcrumb for the header: says which list is on screen and, once scoped, to what.
static void musicViewTitle(char* out, size_t outSize) {
    switch (musicView) {
        case MUSIC_VIEW_ROOT:
            snprintf(out, outSize, "DOLL-OS MUSIC");
            return;
        case MUSIC_VIEW_ARTISTS:
            snprintf(out, outSize, "ARTISTS");
            return;
        case MUSIC_VIEW_ALBUMS:
            if (musicScopeArtist >= 0 && musicScopeArtist < musicArtistCount) {
                snprintf(out, outSize, "%s",
                         musicArtistName(musicTracks[musicArtists[musicScopeArtist].firstTrack]));
            } else {
                snprintf(out, outSize, "ALBUMS");
            }
            return;
        default:
            if (musicOpenAlbum >= 0 && musicOpenAlbum < musicAlbumCount) {
                snprintf(out, outSize, "%s",
                         musicAlbumName(musicTracks[musicAlbums[musicOpenAlbum].firstTrack]));
            } else if (musicFilter.length() > 0) {
                snprintf(out, outSize, "SEARCH");
            } else {
                snprintf(out, outSize, "ALL TRACKS");
            }
            return;
    }
}

static void musicRowSummary(char* out, size_t outSize) {
    switch (musicView) {
        case MUSIC_VIEW_ROOT:
            snprintf(out, outSize, "VOL %d", radioGetVolume());
            return;
        case MUSIC_VIEW_ARTISTS:
            snprintf(out, outSize, "%d artists  VOL %d", musicArtistCount, radioGetVolume());
            return;
        case MUSIC_VIEW_ALBUMS:
            snprintf(out, outSize, "%d albums  VOL %d", musicRowCount(), radioGetVolume());
            return;
        default:
            snprintf(out, outSize, "%d/%d  VOL %d", musicFilteredCount, musicTrackCount,
                     radioGetVolume());
            return;
    }
}

//   ---- navigation --------------------------------------------------------------

static void musicShowAllTracks() {
    musicOpenAlbum = -1;
    musicRebuildFilter("");
}

static void musicShowAlbumTracks(int albumIndex) {
    if (albumIndex < 0 || albumIndex >= musicAlbumCount) return;
    const MusicAlbum& album = musicAlbums[albumIndex];
    musicFilter = "";
    musicFilteredCount = 0;
    for (int i = 0; i < album.trackCount; i++) {
        musicFiltered[musicFilteredCount++] = (uint16_t)(album.firstTrack + i);
    }
    musicOpenAlbum = albumIndex;
}

//Enter: root picks a browse mode, the two list levels descend, tracks play. Returns true
//when the press started playback so the caller can say so.
static bool musicEnterRow(int position) {
    if (position < 0 || position >= musicRowCount()) return false;

    switch (musicView) {
        case MUSIC_VIEW_ROOT:
            if (position == 0) {
                musicView = MUSIC_VIEW_ARTISTS;
            } else if (position == 1) {
                musicScopeArtist = -1;
                musicView = MUSIC_VIEW_ALBUMS;
            } else {
                musicShowAllTracks();
                musicView = MUSIC_VIEW_TRACKS;
            }
            musicSelected() = 0;
            return false;
        case MUSIC_VIEW_ARTISTS:
            musicScopeArtist = position;
            musicView = MUSIC_VIEW_ALBUMS;
            musicSelected() = 0;
            return false;
        case MUSIC_VIEW_ALBUMS:
            musicShowAlbumTracks(musicAlbumRowBase() + position);
            musicView = MUSIC_VIEW_TRACKS;
            musicSelected() = 0;
            return false;
        default:
            return musicPlayTrack(musicFiltered[position]);
    }
}

//Escape: one level back up whichever path got here. Returns false at the root, which is
//the caller's cue to close the player.
static bool musicBackRow() {
    switch (musicView) {
        case MUSIC_VIEW_ROOT:
            return false;
        case MUSIC_VIEW_ARTISTS:
            musicView = MUSIC_VIEW_ROOT;
            return true;
        case MUSIC_VIEW_ALBUMS:
            musicView = musicScopeArtist >= 0 ? MUSIC_VIEW_ARTISTS : MUSIC_VIEW_ROOT;
            return true;
        default:
            //a search result or the flat all-tracks list has no album above it
            musicView = musicOpenAlbum >= 0 ? MUSIC_VIEW_ALBUMS : MUSIC_VIEW_ROOT;
            musicOpenAlbum = -1;
            musicFilter = "";
            return true;
    }
}

//N/P, the button bar and auto-advance all walk the whole sorted library, so the new track
//can land outside the list on screen. Re-scope to wherever it actually lives and put the
//cursor on it, so the display never disagrees with what is coming out of the speaker.
static void musicFollowCurrentTrack() {
    if (musicCurrentTrack < 0) return;
    int album = musicAlbumForTrack(musicCurrentTrack);
    int artist = album >= 0 ? musicArtistForAlbum(album) : -1;
    if (artist >= 0) musicSelection[MUSIC_VIEW_ARTISTS] = artist;

    if (musicView == MUSIC_VIEW_TRACKS && musicOpenAlbum >= 0 && album >= 0
        && album != musicOpenAlbum) {
        if (musicScopeArtist >= 0 && artist >= 0) musicScopeArtist = artist;
        musicShowAlbumTracks(album);
    }
    if (musicView == MUSIC_VIEW_ALBUMS && album >= 0) {
        musicSelected() = max(0, album - musicAlbumRowBase());
    } else if (musicView == MUSIC_VIEW_ARTISTS && artist >= 0) {
        musicSelected() = artist;
    } else if (musicView == MUSIC_VIEW_TRACKS) {
        musicSelected() = max(0, musicFilteredPositionForTrack(musicCurrentTrack));
    }
    if (album >= 0) musicSelection[MUSIC_VIEW_ALBUMS] = max(0, album - musicAlbumRowBase());
}

//The track a press should start when nothing is playing: the highlighted track, or the
//first track of the highlighted artist/album. -1 at the root, which has nothing to start.
static int musicRowStartTrack(int position) {
    if (position < 0 || position >= musicRowCount()) return -1;
    switch (musicView) {
        case MUSIC_VIEW_ROOT:    return -1;
        case MUSIC_VIEW_ARTISTS: return musicArtists[position].firstTrack;
        case MUSIC_VIEW_ALBUMS:  return musicAlbums[musicAlbumRowBase() + position].firstTrack;
        default:                 return musicFiltered[position];
    }
}

//Clip a stack buffer in place so the once-per-second UI refresh does not construct and
//discard a row of small Strings in scarce internal RAM.
static void musicFitBuffer(char* text, int maxWidth) {
    if (!text || frameSprite.textWidth(text) <= maxWidth) return;
    size_t length = strlen(text);
    while (length > 1) {
        text[--length] = '\0';
        if (frameSprite.textWidth(text) + frameSprite.textWidth("~") <= maxWidth) break;
    }
    if (length + 1 < 256) {
        text[length++] = '~';
        text[length] = '\0';
    }
}

static void musicFormatTime(uint32_t seconds, char* out, size_t outSize) {
    snprintf(out, outSize, "%02lu:%02lu", (unsigned long)(seconds / 60),
             (unsigned long)(seconds % 60));
}

static const char* musicPlaybackStateName(RadioState state, bool local) {
    if (!local) return "idle";
    switch (state) {
        case RADIO_CONNECTING: return "opening";
        case RADIO_PLAYING: return "playing";
        case RADIO_PAUSED: return "paused";
        case RADIO_ERROR: return "error";
        case RADIO_STOPPED: return "stopped";
        default: return "idle";
    }
}

static void musicDrawTft(int selected, bool searching, const String& note) {
    RadioState state;
    bool local;
    char liveTitle[128], liveArtist[MUSIC_METADATA_MAX], liveAlbum[MUSIC_METADATA_MAX];
    uint32_t current, duration;
    radioGetPlaybackSnapshot(state, local, liveTitle, sizeof(liveTitle),
                             liveArtist, sizeof(liveArtist), liveAlbum, sizeof(liveAlbum),
                             current, duration);

    const int left = 10;
    const int width = DISPLAY_WIDTH - left * 2;
    const int top = 8;
    const int listTop = 34;
    const int infoTop = DISPLAY_HEIGHT - 78;
    const int rowHeight = 16;
    const int rowCount = musicRowCount();
    int visibleRows = max(3, (infoTop - listTop - 2) / rowHeight);
    int first = selected - visibleRows / 2;
    if (first < 0) first = 0;
    if (first + visibleRows > rowCount) first = rowCount - visibleRows;
    if (first < 0) first = 0;

    char line[256];
    char summary[64];
    musicRowSummary(summary, sizeof(summary));
    frameSprite.fillSprite(TFT_BLACK);
    frameSprite.setTextSize(1);
    frameSprite.setTextDatum(TR_DATUM);
    frameSprite.setTextColor(TFT_CYAN, TFT_BLACK);
    frameSprite.drawString(summary, DISPLAY_WIDTH - left, top);
    frameSprite.setTextDatum(TL_DATUM);
    frameSprite.setTextColor(TFT_PINK, TFT_BLACK);
    musicViewTitle(line, sizeof(line));
    musicFitBuffer(line, width - frameSprite.textWidth(summary) - 8);
    frameSprite.drawString(line, left, top);
    frameSprite.drawFastHLine(left, top + 15, width, TFT_PINK);

    if (rowCount == 0) {
        frameSprite.setTextColor(TFT_YELLOW, TFT_BLACK);
        frameSprite.drawString(musicFilter.length() ? "No tracks match this search"
                                                    : "Library is empty", left, listTop + 8);
    } else {
        for (int row = 0; row < visibleRows && first + row < rowCount; row++) {
            int position = first + row;
            int y = listTop + row * rowHeight;
            bool on = position == selected;
            bool playing = musicRowIsPlaying(position, local);
            frameSprite.setTextColor(on ? TFT_YELLOW : (playing ? TFT_CYAN : TFT_WHITE), TFT_BLACK);
            frameSprite.drawString(on ? ">" : (playing ? "*" : " "), left, y);
            musicRowLabel(position, line, sizeof(line));
            musicFitBuffer(line, width - 18);
            frameSprite.drawString(line, left + 12, y);
        }
    }

    frameSprite.drawFastHLine(left, infoTop, width, TFT_DARKGREY);
    frameSprite.setTextColor(TFT_CYAN, TFT_BLACK);
    const char* nowTitle = "No local track";
    if (local && musicCurrentTrack >= 0 && musicCurrentTrack < musicTrackCount) {
        nowTitle = liveTitle[0] ? liveTitle : musicTracks[musicCurrentTrack].title;
    }
    snprintf(line, sizeof(line), "[%s] %s", musicPlaybackStateName(state, local), nowTitle);
    musicFitBuffer(line, width);
    frameSprite.drawString(line, left, infoTop + 5);

    line[0] = '\0';
    if (local && musicCurrentTrack >= 0 && musicCurrentTrack < musicTrackCount) {
        const MusicTrack& track = musicTracks[musicCurrentTrack];
        const char* artist = liveArtist[0] ? liveArtist : track.artist;
        const char* album = liveAlbum[0] ? liveAlbum : track.album;
        if (artist[0] && album[0]) snprintf(line, sizeof(line), "%s / %s", artist, album);
        else snprintf(line, sizeof(line), "%s", artist[0] ? artist : album);
    }
    frameSprite.setTextColor(TFT_WHITE, TFT_BLACK);
    musicFitBuffer(line, width);
    frameSprite.drawString(line, left, infoTop + 19);

    char currentText[12], durationText[12];
    musicFormatTime(current, currentText, sizeof(currentText));
    musicFormatTime(duration, durationText, sizeof(durationText));
    const int timeWidth = 72;
    const int barX = left;
    const int barY = infoTop + 36;
    const int barWidth = max(20, width - timeWidth);
    frameSprite.drawRect(barX, barY, barWidth, 8, TFT_DARKGREY);
    int filled = duration ? (int)(((uint64_t)(barWidth - 2) * min(current, duration)) / duration) : 0;
    if (filled > 0) frameSprite.fillRect(barX + 1, barY + 1, filled, 6, TFT_PINK);
    frameSprite.setTextDatum(TR_DATUM);
    snprintf(line, sizeof(line), "%s/%s", currentText, durationText);
    frameSprite.drawString(line, DISPLAY_WIDTH - left, barY - 1);
    frameSprite.setTextDatum(TL_DATUM);

    frameSprite.setTextColor(searching ? TFT_YELLOW : TFT_DARKGREY, TFT_BLACK);
    if (searching) snprintf(line, sizeof(line), "Search: %s_", musicFilter.c_str());
    else if (musicView == MUSIC_VIEW_TRACKS) {
        snprintf(line, sizeof(line), "Enter play  Space pause  <-/-> seek  +/- vol");
    } else {
        snprintf(line, sizeof(line), "Enter open  Space pause  <-/-> seek  +/- vol");
    }
    musicFitBuffer(line, width);
    frameSprite.drawString(line, left, DISPLAY_HEIGHT - 25);
    frameSprite.setTextColor(note.length() ? TFT_YELLOW : TFT_DARKGREY, TFT_BLACK);
    snprintf(line, sizeof(line), "%s", note.length() ? note.c_str()
             : (musicView == MUSIC_VIEW_ROOT ? "J/K move  N/P track  / find  R rescan  Q leave"
                                             : "J/K move  N/P track  / find  Esc back  Q leave"));
    musicFitBuffer(line, width);
    frameSprite.drawString(line, left, DISPLAY_HEIGHT - 12);
    frameSprite.setTextColor(TFT_WHITE, TFT_BLACK);
    pushDisplayFrame();
}

static void musicRenderTelnet(int selected, bool searching, const String& note) {
    if (!telnetClient || !telnetClient.connected()) return;
    RadioState state;
    bool local;
    char liveTitle[128], liveArtist[MUSIC_METADATA_MAX], liveAlbum[MUSIC_METADATA_MAX];
    uint32_t current, duration;
    radioGetPlaybackSnapshot(state, local, liveTitle, sizeof(liveTitle),
                             liveArtist, sizeof(liveArtist), liveAlbum, sizeof(liveAlbum),
                             current, duration);
    const int rowCount = musicRowCount();
    const int rows = 10;
    int first = selected - rows / 2;
    if (first < 0) first = 0;
    if (first + rows > rowCount) first = rowCount - rows;
    if (first < 0) first = 0;

    char title[128], summary[64];
    musicViewTitle(title, sizeof(title));
    musicRowSummary(summary, sizeof(summary));
    telnetClient.printf("\x1b[?25l\x1b[2J\x1b[H\x1b[95m%s\x1b[0m  %s\r\n", title, summary);
    telnetClient.print("------------------------------------------------------------\r\n");
    for (int row = 0; row < rows; row++) {
        int position = first + row;
        if (position >= rowCount) {
            telnetClient.print("\r\n");
            continue;
        }
        bool on = position == selected;
        bool playing = musicRowIsPlaying(position, local);
        telnetClient.print(on ? "\x1b[33m> " : (playing ? "\x1b[36m* " : "  "));
        char label[160];
        musicRowLabel(position, label, sizeof(label));
        if (strlen(label) > 56) {
            label[55] = '~';
            label[56] = '\0';
        }
        telnetClient.print(label);
        telnetClient.print("\x1b[0m\r\n");
    }
    char currentText[12], durationText[12];
    musicFormatTime(current, currentText, sizeof(currentText));
    musicFormatTime(duration, durationText, sizeof(durationText));
    int filled = duration ? (int)((24ULL * min(current, duration)) / duration) : 0;
    telnetClient.print("------------------------------------------------------------\r\n");
    telnetClient.printf("[%s] %s", musicPlaybackStateName(state, local),
                        (local && liveTitle[0]) ? liveTitle : "No local track");
    if (local && liveArtist[0]) telnetClient.printf(" - %s", liveArtist);
    telnetClient.print("\r\n[");
    for (int i = 0; i < 24; i++) telnetClient.print(i < filled ? "#" : "-");
    telnetClient.printf("] %s/%s\r\n", currentText, durationText);
    if (searching) telnetClient.printf("\x1b[33mSearch: %s_\x1b[0m\r\n", musicFilter.c_str());
    else if (musicView == MUSIC_VIEW_TRACKS) {
        telnetClient.print("Enter play  Space pause  Left/Right seek  +/- volume\r\n");
    } else {
        telnetClient.print("Enter open  Space pause  Left/Right seek  +/- volume\r\n");
    }
    telnetClient.print(note.length() ? note
        : (musicView == MUSIC_VIEW_ROOT ? "J/K move  N/P track  / search  R rescan  Q leave"
                                        : "J/K move  N/P track  / search  Esc back  Q leave"));
    telnetClient.print("\r\n");
}

static void musicRender(int selected, bool searching, const String& note) {
    musicDrawTft(selected, searching, note);
    musicRenderTelnet(selected, searching, note);
}

static bool musicFeedKeyByte(uint8_t b, MusicKeyState& state, MusicKey& key, char& ch) {
    key = MK_NONE;
    ch = 0;
    if (state.esc == UESC_GOT_ESC) {
        if (b == '[') {
            state.esc = UESC_GOT_CSI;
            state.params = "";
            return false;
        }
        state.esc = UESC_NONE;
        key = MK_ESCAPE;
        return true;
    }
    if (state.esc == UESC_GOT_CSI) {
        if ((b >= '0' && b <= '9') || b == ';') {
            state.params += (char)b;
            return false;
        }
        state.esc = UESC_NONE;
        if (b == 'A') key = MK_UP;
        else if (b == 'B') key = MK_DOWN;
        else if (b == 'C') key = MK_RIGHT;
        else if (b == 'D') key = MK_LEFT;
        else if (b == '~' && state.params == "5") key = MK_PAGE_UP;
        else if (b == '~' && state.params == "6") key = MK_PAGE_DOWN;
        return key != MK_NONE;
    }

    if (b == '\n' && state.lastByteWasCR) {
        state.lastByteWasCR = false;
        return false;
    }
    state.lastByteWasCR = b == '\r';
    if (b == 0x1b) {
        state.esc = UESC_GOT_ESC;
        state.escAtMs = millis();
        return false;
    }
    if (b == 0x14 || b == 0x18) key = MK_ABORT;    //Ctrl+T / ^X -- the terminate pair
    else if (b == 0x03) key = MK_ESCAPE;           //Ctrl+C: cancel the screen, not the audio
    else if (b == '\r' || b == '\n') key = MK_ENTER;
    else if (b == 0x08 || b == 0x7f) key = MK_BACKSPACE;
    else if (b >= 0x20 && b <= 0x7e) { key = MK_CHAR; ch = (char)b; }
    return key != MK_NONE;
}

static bool musicTimedOutEscape(MusicKeyState& state, MusicKey& key, char& ch) {
    if (state.esc == UESC_GOT_ESC && millis() - state.escAtMs > 40) {
        state.esc = UESC_NONE;
        key = MK_ESCAPE;
        ch = 0;
        return true;
    }
    return false;
}

static bool musicNextKey(MusicKey& key, char& ch) {
    if (musicTimedOutEscape(musicTelnetKeys, key, ch)
        || musicTimedOutEscape(musicKeyboardKeys, key, ch)) return true;
    int b;
    while ((b = telnetReadFilteredByte()) >= 0) {
        if (musicFeedKeyByte((uint8_t)b, musicTelnetKeys, key, ch)) return true;
    }
    while ((b = keyboardReadRawByte()) >= 0) {
        if (musicFeedKeyByte((uint8_t)b, musicKeyboardKeys, key, ch)) return true;
    }
    return false;
}

static void musicResetKeys() {
    musicTelnetKeys.esc = UESC_NONE;
    musicTelnetKeys.params = "";
    musicTelnetKeys.lastByteWasCR = false;
    musicKeyboardKeys.esc = UESC_NONE;
    musicKeyboardKeys.params = "";
    musicKeyboardKeys.lastByteWasCR = false;
}

//   ---- button bar (PadButtons.ino) -------------------------------------------
//
//   Start = previous track, A = next track, applied to the library. B depends on whether
//   the browser is on screen: with the player open it is Enter, because the bar is the
//   only way a keyboard-less handheld can descend root -> artists -> albums -> tracks
//   (the joystick's click sends Escape, which only ever goes back up). With the player
//   closed there is nothing to select, so B keeps its background meaning, pause/resume.
//
//   The one exception is the row that is already playing: re-entering it would restart
//   the track from zero, so B pauses there instead. That keeps pause reachable from the
//   bar inside the player -- the cursor follows the current track anyway (N/P, the bar
//   and auto-advance all call musicFollowCurrentTrack), so it is usually already there.
//
//   Returns false when the library is not what the press should act on, so the caller
//   can hand it to the radio instead.
//
//   Never scans the card. A scan walks all of /sd/music reading ID3 tags, which would
//   stall the shell mid-press, and musicAdvance() already refuses to move without a
//   library -- if a track is playing, the library is loaded by definition.
static bool musicPadTransport(PadButton button, bool inPlayer) {
    RadioState state;
    bool local;
    uint32_t current, duration;
    radioGetPlaybackSnapshot(state, local, nullptr, 0, nullptr, 0, nullptr, 0, current, duration);
    const bool playingFromLibrary = local && musicCurrentTrack >= 0;

    if (button == PAD_BTN_B && inPlayer) {
        //musicRowIsPlaying is only exact on the track list; an artist or album row counts
        //as playing when it merely *contains* the current track, and those must descend.
        if (playingFromLibrary && musicView == MUSIC_VIEW_TRACKS
            && musicRowIsPlaying(musicSelected(), local)) {
            radioTogglePlaybackPause();
        } else {
            musicEnterRow(musicSelected());
        }
        return true;
    }

    //Stepping still needs something to step: a track playing, or the open player sitting
    //on a row it could start from (-1 at the root menu, which has nothing to start).
    if (!playingFromLibrary && (!inPlayer || musicRowStartTrack(musicSelected()) < 0)) {
        return false;
    }

    switch (button) {
        case PAD_BTN_START:
        case PAD_BTN_A:
            return musicAdvance(button == PAD_BTN_A ? 1 : -1);
        case PAD_BTN_B:
            radioTogglePlaybackPause();   //player closed, so the guard above proved it is playing
            return true;
        default:
            return false;   //Select: reserved
    }
}

//   Background case: the player is closed but a library track is still what's playing,
//   so the bar stays on the library rather than reaching past it to the radio. Prints
//   the new track because there is no player screen up to show it (pause/resume already
//   announces itself from the audio task).
bool musicPadTransportBackground(PadButton button) {
    const bool trackChange = button == PAD_BTN_START || button == PAD_BTN_A;
    if (!musicPadTransport(button, false)) {
        return false;
    }
    if (trackChange && musicCurrentTrack >= 0) {
        outLine("music: " + String(musicTracks[musicCurrentTrack].title), C_PINK);
    }
    return true;
}

//   The terminate chord (Ctrl+T / ^X, which is what DS-Slave's rotary Settings >
//   Terminate App sends) offered to the library before the shell's line editor sees it.
//   Every modal screen in DOLL-OS already reads that chord as "stop what is running",
//   and a track playing with the player closed is the one thing still running that no
//   screen is left to stop -- the player's own Escape/q deliberately walk out and leave
//   it playing. At the prompt the chord otherwise does nothing at all (an unbound
//   control byte, dropped by processLineEditByte), so nothing is being taken away.
//
//   Only reachable from the shell readers, which only run when no modal app owns
//   loop() -- an app's own reader gets the byte first and keeps it, so terminating an
//   app still terminates the app rather than the music behind it.
//
//   A stream is deliberately left alone: it is background audio, not an app, and B on
//   the bar already stops it. Returns true when the chord was spent here, which is the
//   caller's cue to drop the byte instead of feeding it to the line editor.
bool musicHandleShellTerminate(uint8_t ch) {
    if (ch != 0x14 && ch != 0x18) return false;

    RadioState state;
    bool local;
    uint32_t current, duration;
    radioGetPlaybackSnapshot(state, local, nullptr, 0, nullptr, 0, nullptr, 0, current, duration);
    if (!local || musicCurrentTrack < 0) return false;

    //suppress first, exactly as the player's own MK_ABORT does: otherwise the audio
    //task opens the next track behind us the moment this one is torn down
    musicSuppressAutoAdvance = true;
    radioStopPlayback();
    outLine("music: terminated", C_PINK);
    return true;
}

static void musicRunPlayer() {
    if (!musicEnsureLibrary()) return;
    if (musicTrackCount == 0) {
        outLine("music: no .mp3 files found under /sd/music", C_YELLOW);
        return;
    }

    //Open at the root menu. The per-level cursors are static, so reopening the player
    //returns to the rows last left behind on each level; a rescan is what clears them.
    musicShowAllTracks();
    musicView = MUSIC_VIEW_ROOT;
    musicScopeArtist = -1;
    musicFollowCurrentTrack();
    bool searching = false;
    bool quit = false;
    bool aborted = false;   //left by the terminate chord, which stops playback too
    bool redraw = true;
    String note = musicLibraryTruncated
        ? "Library full; showing first " + String(MUSIC_LIBRARY_MAX_TRACKS) + " tracks" : "";
    unsigned long lastProgressDraw = 0;
    musicResetKeys();
    if (telnetClient && telnetClient.connected()) telnetClient.print("\x1b[?25l");

    while (!quit) {
        MusicKey key;
        char ch;
        while (musicNextKey(key, ch)) {
            redraw = true;
            note = "";
            if (key == MK_ABORT) {
                //Leave *and* silence, which is the one thing q and Escape deliberately
                //don't do. Checked ahead of the search box so a terminate never needs a
                //second press to be heard -- the whole point of the chord is that it is
                //the way out when something is stuck. Suppressing auto-advance first
                //stops the audio task opening the next track behind us.
                musicSuppressAutoAdvance = true;
                radioStopPlayback();
                aborted = true;
                quit = true;
                break;
            }
            if (searching) {
                if (key == MK_ESCAPE) {
                    searching = false;
                } else if (key == MK_ENTER) {
                    searching = false;
                } else if (key == MK_BACKSPACE && musicFilter.length() > 0) {
                    musicFilter.remove(musicFilter.length() - 1);
                    musicRebuildFilter(musicFilter);
                    musicSelected() = 0;
                } else if (key == MK_CHAR && musicFilter.length() < 48) {
                    musicFilter += ch;
                    musicRebuildFilter(musicFilter);
                    musicSelected() = 0;
                }
                continue;
            }

            const int rowCount = musicRowCount();
            if (key == MK_CHAR && (ch == 'q' || ch == 'Q')) {
                quit = true;
            } else if (key == MK_ESCAPE) {
                //one level back up, and out of the player entirely from the root
                if (!musicBackRow()) quit = true;
            } else if (key == MK_UP || (key == MK_CHAR && (ch == 'k' || ch == 'K'))) {
                if (rowCount) musicSelected() = (musicSelected() + rowCount - 1) % rowCount;
            } else if (key == MK_DOWN || (key == MK_CHAR && (ch == 'j' || ch == 'J'))) {
                if (rowCount) musicSelected() = (musicSelected() + 1) % rowCount;
            } else if (key == MK_PAGE_UP) {
                musicSelected() = max(0, musicSelected() - 8);
            } else if (key == MK_PAGE_DOWN) {
                musicSelected() = min(max(0, rowCount - 1), musicSelected() + 8);
            } else if (key == MK_ENTER) {
                if (musicEnterRow(musicSelected())) note = "Opening track...";
            } else if (key == MK_LEFT) {
                radioSeekPlayback(-10);
            } else if (key == MK_RIGHT) {
                radioSeekPlayback(10);
            } else if (key == MK_CHAR && ch == ' ') {
                RadioState state;
                bool local;
                uint32_t current, duration;
                radioGetPlaybackSnapshot(state, local, nullptr, 0, nullptr, 0, nullptr, 0, current, duration);
                if (local) radioTogglePlaybackPause();
                else musicPlayTrack(musicRowStartTrack(musicSelected()));
            } else if (key == MK_CHAR && (ch == 'n' || ch == 'N')) {
                if (musicAdvance(1)) musicFollowCurrentTrack();
            } else if (key == MK_CHAR && (ch == 'p' || ch == 'P')) {
                if (musicAdvance(-1)) musicFollowCurrentTrack();
            } else if (key == MK_CHAR && (ch == 's' || ch == 'S')) {
                musicSuppressAutoAdvance = true;
                radioStopPlayback();
                note = "Stopped";
            } else if (key == MK_CHAR && ch == '/') {
                //search is global rather than per-level: it drops into a flat result list
                //that Escape leaves, so a query never has to be repeated per album
                musicView = MUSIC_VIEW_TRACKS;
                musicOpenAlbum = -1;
                musicRebuildFilter("");
                musicSelected() = 0;
                searching = true;
            } else if (key == MK_CHAR && (ch == '+' || ch == '=')) {
                radioAdjustVolume(1);
            } else if (key == MK_CHAR && ch == '-') {
                radioAdjustVolume(-1);
            } else if (key == MK_CHAR && (ch == 'r' || ch == 'R')) {
                musicRender(musicSelected(), false, "Scanning /sd/music...");
                if (musicScanLibrary()) {
                    musicShowAllTracks();
                    musicView = MUSIC_VIEW_ROOT;
                    note = String("Library rebuilt: ") + musicTrackCount + " tracks, "
                           + musicAlbumCount + " albums";
                } else {
                    note = "Library scan failed";
                }
            }
        }

        //The player owns the screen while it runs, so it takes button-bar presses itself
        //instead of leaving them for padButtonService() (which never runs until we exit).
        PadButton pad;
        while (padButtonTake(pad)) {
            const bool trackChange = pad == PAD_BTN_START || pad == PAD_BTN_A;
            if (!musicPadTransport(pad, true)) continue;
            //follow the cursor to whatever started, as N/P do; a bare pause leaves it put
            if (trackChange) musicFollowCurrentTrack();
            note = "";
            redraw = true;
        }

        radioService();
        ftpService();
        maintainInternetConnection();
        ledService();
        if (millis() - lastProgressDraw >= 1000) redraw = true;
        if (redraw) {
            //a rescan or a backspaced search can shrink the list under the cursor
            const int rows = musicRowCount();
            if (musicSelected() >= rows) musicSelected() = max(0, rows - 1);
            musicRender(musicSelected(), searching, note);
            redraw = false;
            lastProgressDraw = millis();
        }
        delay(2);
    }

    if (telnetClient && telnetClient.connected()) {
        telnetClient.print("\x1b[?25h\x1b[2J\x1b[H");
    }
    setActiveInput(shellPrompt(), "", false);
    displayDirty = true;
    outLine(aborted ? "music: player closed; playback stopped"
                    : "music: player closed; playback continues in the background", C_GREEN);
    drawDisplayFrame();
    printPrompt();
}

static void musicPrintStatus() {
    RadioState state;
    bool local;
    char title[128], artist[MUSIC_METADATA_MAX], album[MUSIC_METADATA_MAX];
    uint32_t current, duration;
    radioGetPlaybackSnapshot(state, local, title, sizeof(title), artist, sizeof(artist),
                             album, sizeof(album), current, duration);
    outLine("");
    outLine("Music library", C_CYAN);
    outLine("-------------");
    outLine("Root: /sd/music");
    outLine("Tracks: " + String(musicLibraryReady ? musicTrackCount : 0)
            + (musicLibraryTruncated ? " (truncated at " + String(MUSIC_LIBRARY_MAX_TRACKS) + ")" : ""));
    outLine("Artists: " + String(musicLibraryReady ? musicArtistCount : 0)
            + "   Albums: " + String(musicLibraryReady ? musicAlbumCount : 0));
    outLine("Catalog: " + String(musicTracks ? MUSIC_LIBRARY_MAX_TRACKS * sizeof(MusicTrack) : 0)
            + " bytes PSRAM");
    outLine("State: " + String(musicPlaybackStateName(state, local)));
    if (local && musicCurrentTrack >= 0) {
        //Fall back to the catalog entry the way musicDrawTft does: the audio engine only
        //reports what the file's own tags carried, so folder-derived artist/album would
        //otherwise be visible in the player list but missing from status.
        const MusicTrack& playing = musicTracks[musicCurrentTrack];
        const char* statusArtist = artist[0] ? artist : playing.artist;
        const char* statusAlbum = album[0] ? album : playing.album;
        outLine("Title: " + String(title[0] ? title : playing.title));
        if (statusArtist[0]) outLine("Artist: " + String(statusArtist));
        if (statusAlbum[0]) outLine("Album: " + String(statusAlbum));
        char currentText[12], durationText[12];
        musicFormatTime(current, currentText, sizeof(currentText));
        musicFormatTime(duration, durationText, sizeof(durationText));
        outLine("Time: " + String(currentText) + "/" + durationText);
    }
    outLine("");
}

static void musicPrintHelp() {
    outLine("music -- open the full-screen MP3 library/player", C_CYAN);
    outLine("         browse Artists / Albums / Tracks; Enter descends, Esc backs out", C_CYAN);
    outLine("music rescan -- rebuild metadata library from /sd/music recursively", C_CYAN);
    outLine("music status -- show library and playback status", C_CYAN);
    outLine("music play <number> | next | prev | stop", C_CYAN);
}

void handleMusicCommand(const String parts[], int partCount) {
    String sub = partCount > 1 ? parts[1] : "";
    sub.toLowerCase();
    if (sub == "help" || sub == "-h" || sub == "--help" || sub == "?") {
        musicPrintHelp();
        return;
    }
    if (sub == "rescan" || sub == "scan") {
        outLine("music: scanning /sd/music and reading ID3 metadata...", C_CYAN);
        if (musicScanLibrary()) {
            outLine("music: library contains " + String(musicTrackCount) + " track(s)"
                    + (musicLibraryTruncated
                       ? " (truncated at " + String(MUSIC_LIBRARY_MAX_TRACKS) + ")" : ""), C_GREEN);
        }
        return;
    }
    if (sub == "status") {
        musicPrintStatus();
        return;
    }
    if (sub == "stop") {
        musicSuppressAutoAdvance = true;
        radioStopPlayback();
        outLine("music: stopped", C_PINK);
        return;
    }
    if (sub == "next" || sub == "prev") {
        if (musicEnsureLibrary() && musicAdvance(sub == "next" ? 1 : -1)) {
            outLine("music: opening " + String(musicTracks[musicCurrentTrack].title), C_PINK);
        }
        return;
    }
    if (sub == "play") {
        if (partCount < 3 || !musicEnsureLibrary()) {
            musicPrintHelp();
            return;
        }
        int index = parts[2].toInt();
        if (index < 1 || index > musicTrackCount || String(index) != parts[2]) {
            outLine("music: track number must be 1-" + String(musicTrackCount), C_RED);
            return;
        }
        if (musicPlayTrack(index - 1)) {
            outLine("music: opening " + String(musicTracks[index - 1].title), C_PINK);
        }
        return;
    }
    if (sub.length() != 0) {
        musicPrintHelp();
        return;
    }
    musicRunPlayer();
}
