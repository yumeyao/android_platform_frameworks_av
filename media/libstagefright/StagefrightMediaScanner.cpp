/*
 * Copyright (C) 2009 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

//#define LOG_NDEBUG 0
#define LOG_TAG "StagefrightMediaScanner"
#include <utils/Log.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include <media/stagefright/StagefrightMediaScanner.h>

#include <media/IMediaHTTPService.h>
#include <media/mediametadataretriever.h>
#include <private/media/VideoFrame.h>

// Sonivox includes
#include <libsonivox/eas.h>

namespace android {

StagefrightMediaScanner::StagefrightMediaScanner() {}

StagefrightMediaScanner::~StagefrightMediaScanner() {}

static MediaScanResult HandleMIDI(
        const char *filename, MediaScannerClient *client) {
    // get the library configuration and do sanity check
    const S_EAS_LIB_CONFIG* pLibConfig = EAS_Config();
    if ((pLibConfig == NULL) || (LIB_VERSION != pLibConfig->libVersion)) {
        ALOGE("EAS library/header mismatch\n");
        return MEDIA_SCAN_RESULT_ERROR;
    }
    EAS_I32 temp;

    // spin up a new EAS engine
    EAS_DATA_HANDLE easData = NULL;
    EAS_HANDLE easHandle = NULL;
    EAS_RESULT result = EAS_Init(&easData);
    if (result == EAS_SUCCESS) {
        EAS_FILE file;
        file.path = filename;
        file.fd = 0;
        file.offset = 0;
        file.length = 0;
        result = EAS_OpenFile(easData, &file, &easHandle);
    }
    if (result == EAS_SUCCESS) {
        result = EAS_Prepare(easData, easHandle);
    }
    if (result == EAS_SUCCESS) {
        result = EAS_ParseMetaData(easData, easHandle, &temp);
    }
    if (easHandle) {
        EAS_CloseFile(easData, easHandle);
    }
    if (easData) {
        EAS_Shutdown(easData);
    }

    if (result != EAS_SUCCESS) {
        return MEDIA_SCAN_RESULT_SKIPPED;
    }

    char buffer[20];
    sprintf(buffer, "%ld", temp);
    status_t status = client->addStringTag("duration", buffer);
    if (status != OK) {
        return MEDIA_SCAN_RESULT_ERROR;
    }
    return MEDIA_SCAN_RESULT_OK;
}

MediaScanResult StagefrightMediaScanner::processFile(
        const char *path, const char *mimeType,
        MediaScannerClient &client) {
    ALOGV("processFile '%s'.", path);

    client.setLocale(locale());
    client.beginFile();
    MediaScanResult result = processFileInternal(path, mimeType, client);
    client.endFile();
    return result;
}

/*
 * Return 0 for MIDI
 * Return 1 for other valid types
 * Return -1 for non-valid
 */
static inline int FileHasAcceptableExtension(const char *extension, int len) {
    static const char kValidExtensions[] =
        //2 bytes - only non-MIDI
        "fl" "ts" //please fill to make it multiple of 4
        //3 bytes
        //MIDI types go first
        "ota rtx xmf imy smf mid "
        //Non-MIDI types without numeric go next
        "awb mpg avi mka mkv amr wav aac wma ogg "
        //Non-MIDI types with numeric go last
        //A mask is used to mask alpha to lowercase
        "\0 \0 3g2 "   "\0   3gp "   " \0  m4a "   "  \0 mp4 "   "  \0 mp3 "
        //4 bytes
        //MIDI types go first
        "midi" "mxmf"
        //Non-MIDI types go last
        "mpga" "webm" "mpeg" "flac"
        //5 bytes - 3gpp2 can share with 3gpp.
        //Non-MIDI
        "\0   3gpp2\0\0\0"
        //MIDI
        "rtttl\0\0\0";
    static const int num2 = 2;
    static const size_t size2 = (num2+1) * sizeof(int16_t) / sizeof(int32_t);
    static const int num3MIDI = 6;
    static const int num3NonMIDIA = 10; //Alpha
    static const int num3NonMIDIN = 5;  //Numeric
    static const size_t size3 = num3MIDI + num3NonMIDIA + 2 * num3NonMIDIN;
    static const int num4MIDI = 2;
    static const int num4NonMIDIA = 4;  //Alpha
    static const int num4NonMIDIN = 1;  //Numeric
    static const size_t size4 = num4MIDI + num4NonMIDIA + 2 * num4NonMIDIN;

    if (len < 2 || len > 5) return -1;

    const char *cbase = kValidExtensions;
    int count;
    if (unlikely(len==2)) {
        const int16_t *base = reinterpret_cast<const int16_t*>(cbase);
        int16_t extexpr = *(reinterpret_cast<const int16_t*>(extension));
        extexpr |= 0x2020;
        for (count=num2-1; count>=0; --count) {
            if (extexpr==base[count]) return 1;
        }
        return -1;
    }
    const int32_t *base = reinterpret_cast<const int32_t*>(cbase);
    int32_t extexpr = *(reinterpret_cast<const int32_t*>(extension));
    if (unlikely(len==5)) {
        base += (size2 + size3 + size4) - 1*2; //3gpp2 & 3gpp to minus 2
        if ((extexpr | base[0]) == base[1] && extension[4] == '2') return 1;
        if ((extexpr | 0x20202020) == base[3]
            && (extension[4] | 0x20) == 'l') return 0;
        return -1;
    }

    //Now len==3 || len==4
    //Non-MIDI with numeric
    base = len == 3 ? base + size2 + size3 - num3NonMIDIN *2
                    : base + size2 + size3 + size4 - num4NonMIDIN *2;
    count = len == 3 ? (num3NonMIDIN - 1)*2 : (num4NonMIDIN - 1)*2;
    for (; count>=0; count-=2) {
        if ((extexpr | base[count]) == base[count+1]) return 1;
    }

    //Non-MIDI without numeric
    count = len == 3 ? num3NonMIDIA : num4NonMIDIA;
    extexpr |= 0x20202020;
    base -= count;
    --count;
    for (; count>=0; --count) {
        if (extexpr == base[count]) return 1;
    }

    //MIDI
    count = len == 3 ? num3MIDI : num4MIDI;
    base -= count;
    --count;
    for (; count>=0; --count) {
        if (extexpr == base[count]) return 0;
    }
    return -1;
}

MediaScanResult StagefrightMediaScanner::processFileInternal(
        const char *path, const char * /* mimeType */,
        MediaScannerClient &client) {
    int length = strlen(path);
    int extoffset = length;
    while (extoffset >= 0 && path[extoffset]!='.') --extoffset;

    if (extoffset < 0) {
        return MEDIA_SCAN_RESULT_SKIPPED;
    }

    ++extoffset; // No need to compare the '.'
    const char *extension = path+extoffset;
    int filetype = FileHasAcceptableExtension(extension, length - extoffset);
    if (filetype < 0) {
        return MEDIA_SCAN_RESULT_SKIPPED;
    } else if (filetype == 0) {
        return HandleMIDI(path, &client);
    }

    sp<MediaMetadataRetriever> mRetriever(new MediaMetadataRetriever);

    int fd = open(path, O_RDONLY | O_LARGEFILE);
    status_t status;
    if (fd < 0) {
        // couldn't open it locally, maybe the media server can?
        status = mRetriever->setDataSource(NULL /* httpService */, path);
    } else {
        status = mRetriever->setDataSource(fd, 0, 0x7ffffffffffffffL);
        close(fd);
    }

    if (status) {
        return MEDIA_SCAN_RESULT_ERROR;
    }

    const char *value;
    if ((value = mRetriever->extractMetadata(
                    METADATA_KEY_MIMETYPE)) != NULL) {
        status = client.setMimeType(value);
        if (status) {
            return MEDIA_SCAN_RESULT_ERROR;
        }
    }

    struct KeyMap {
        const char *tag;
        int key;
    };
    static const KeyMap kKeyMap[] = {
        { "tracknumber", METADATA_KEY_CD_TRACK_NUMBER },
        { "discnumber", METADATA_KEY_DISC_NUMBER },
        { "album", METADATA_KEY_ALBUM },
        { "artist", METADATA_KEY_ARTIST },
        { "albumartist", METADATA_KEY_ALBUMARTIST },
        { "composer", METADATA_KEY_COMPOSER },
        { "genre", METADATA_KEY_GENRE },
        { "title", METADATA_KEY_TITLE },
        { "year", METADATA_KEY_YEAR },
        { "duration", METADATA_KEY_DURATION },
        { "writer", METADATA_KEY_WRITER },
        { "compilation", METADATA_KEY_COMPILATION },
        { "isdrm", METADATA_KEY_IS_DRM },
        { "width", METADATA_KEY_VIDEO_WIDTH },
        { "height", METADATA_KEY_VIDEO_HEIGHT },
    };
    static const size_t kNumEntries = sizeof(kKeyMap) / sizeof(kKeyMap[0]);

    for (size_t i = 0; i < kNumEntries; ++i) {
        const char *value;
        if ((value = mRetriever->extractMetadata(kKeyMap[i].key)) != NULL) {
            status = client.addStringTag(kKeyMap[i].tag, value);
            if (status != OK) {
                return MEDIA_SCAN_RESULT_ERROR;
            }
        }
    }

    return MEDIA_SCAN_RESULT_OK;
}

MediaAlbumArt *StagefrightMediaScanner::extractAlbumArt(int fd) {
    ALOGV("extractAlbumArt %d", fd);

    off64_t size = lseek64(fd, 0, SEEK_END);
    if (size < 0) {
        return NULL;
    }
    lseek64(fd, 0, SEEK_SET);

    sp<MediaMetadataRetriever> mRetriever(new MediaMetadataRetriever);
    if (mRetriever->setDataSource(fd, 0, size) == OK) {
        sp<IMemory> mem = mRetriever->extractAlbumArt();
        if (mem != NULL) {
            MediaAlbumArt *art = static_cast<MediaAlbumArt *>(mem->pointer());
            return art->clone();
        }
    }

    return NULL;
}

}  // namespace android
