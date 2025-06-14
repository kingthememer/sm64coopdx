#include <ultra64.h>
#include "sm64.h"
#include "game_init.h"
#include "main.h"
#include "engine/math_util.h"
#include "area.h"
#include "level_update.h"
#include "save_file.h"
#include "sound_init.h"
#include "level_table.h"
#include "course_table.h"
#include "rumble_init.h"
#include "hardcoded.h"
#include "macros.h"
#include "pc/network/network.h"
#include "pc/lua/utils/smlua_level_utils.h"
#include "pc/utils/misc.h"

#ifndef bcopy
#define bcopy(b1,b2,len) (memmove((b2), (b1), (len)), (void) 0)
#endif

#define MENU_DATA_MAGIC 0x4849
#define SAVE_FILE_MAGIC 0x4441

#define INVALID_FILE_INDEX(_fi) ((u32)_fi >= NUM_SAVE_FILES)
#define INVALID_SRC_SLOT(_ss) ((u32)_ss >= 2)
#define INVALID_LEVEL_NUM(_ln) ((u32)_ln >= LEVEL_COUNT)
#define INVALID_COURSE_STAR_INDEX(_ci) ((u32)_ci >= COURSE_COUNT)
#define INVALID_COURSE_COIN_INDEX(_ci) ((u32)_ci >= COURSE_STAGES_COUNT)

STATIC_ASSERT(sizeof(struct SaveBuffer) == EEPROM_SIZE, "eeprom buffer size must match");

extern struct SaveBuffer gSaveBuffer;

struct WarpCheckpoint gWarpCheckpoint;

s8 gMainMenuDataModified;
s8 gSaveFileModified;

u8 gLastCompletedCourseNum = COURSE_NONE;
u8 gLastCompletedStarNum = 0;
s8 sUnusedGotGlobalCoinHiScore = 0;
u8 gGotFileCoinHiScore = FALSE;
u8 gCurrCourseStarFlags = 0;

u8 gSaveFileUsingBackupSlot = FALSE;

#define STUB_LEVEL(_0, _1, courseenum, _3, _4, _5, _6, _7, _8) courseenum,
#define DEFINE_LEVEL(_0, _1, courseenum, _3, _4, _5, _6, _7, _8, _9, _10) courseenum,

s8 gLevelToCourseNumTable[] = {
    #include "levels/level_defines.h"
};
#undef STUB_LEVEL
#undef DEFINE_LEVEL

#define STUB_LEVEL(_0, levelenum, courseenum, _3, _4, _5, _6, _7, _8) [courseenum] = levelenum,
#define DEFINE_LEVEL(_0, levelenum, courseenum, _3, _4, _5, _6, _7, _8, _9, _10) [courseenum] = levelenum,
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Woverride-init" // this is hacky, but its dealt with in the getter function
s8 sCourseNumToLevelNumTable[] = {
#include "levels/level_defines.h"
};
#pragma GCC diagnostic pop
#undef STUB_LEVEL
#undef DEFINE_LEVEL

STATIC_ASSERT(ARRAY_COUNT(gLevelToCourseNumTable) == LEVEL_COUNT - 1,
              "change this array if you are adding levels");

s8 get_level_num_from_course_num(s16 courseNum) {
    if (courseNum < 0 || courseNum >= COURSE_COUNT) {
        return LEVEL_NONE;
    }
    switch (courseNum) { // deal with the overridden courses
        case COURSE_NONE:
            return LEVEL_CASTLE;
        case COURSE_BITDW:
            return LEVEL_BITDW;
        case COURSE_BITFS:
            return LEVEL_BITFS;
        case COURSE_BITS:
            return LEVEL_BITS;
    }
    return sCourseNumToLevelNumTable[courseNum];
}

s8 get_level_course_num(s16 levelNum) {
    if (levelNum >= CUSTOM_LEVEL_NUM_START) {
        struct CustomLevelInfo* info = smlua_level_util_get_info(levelNum);
        return (info ? info->courseNum : COURSE_NONE);
    }

    levelNum = levelNum - 1;

    if (INVALID_LEVEL_NUM(levelNum)) {
        return COURSE_NONE;
    }
    return gLevelToCourseNumTable[levelNum];
}

// This was probably used to set progress to 100% for debugging, but
// it was removed from the release ROM.
static void stub_save_file_1(void) {
    UNUSED s32 pad;
}

/**
 * Byteswap all multibyte fields in a SaveBlockSignature.
 */
static inline void bswap_signature(struct SaveBlockSignature *data) {
    data->magic = BSWAP16(data->magic);
    data->chksum = BSWAP16(data->chksum); // valid as long as the checksum is a literal sum
}

/**
 * Byteswap all multibyte fields in a MainMenuSaveData.
 */
static inline void bswap_menudata(struct MainMenuSaveData *data) {
    for (s32 i = 0; i < NUM_SAVE_FILES; ++i)
        data->coinScoreAges[i] = BSWAP32(data->coinScoreAges[i]);
    data->soundMode = BSWAP16(data->soundMode);
#ifdef VERSION_EU
    data->language = BSWAP16(data->language);
#endif
    bswap_signature(&data->signature);
}

/**
 * Byteswap all multibyte fields in a SaveFile.
 */
static inline void bswap_savefile(struct SaveFile *data) {
    data->capPos[0] = BSWAP16(data->capPos[0]);
    data->capPos[1] = BSWAP16(data->capPos[1]);
    data->capPos[2] = BSWAP16(data->capPos[2]);
    data->flags = BSWAP32(data->flags);
    bswap_signature(&data->signature);
}

/**
 * Read from EEPROM to a given address.
 * The EEPROM address is computed using the offset of the destination address from gSaveBuffer.
 * Try at most 4 times, and return 0 on success. On failure, return the status returned from
 * osEepromLongRead. It also returns 0 if EEPROM isn't loaded correctly in the system.
 */
static s32 read_eeprom_data(void *buffer, s32 size) {
    s32 status = 0;

    if (gEepromProbe != 0) {
        s32 triesLeft = 4;
        u32 offset = (u32)((u8 *) buffer - (u8 *) &gSaveBuffer) / 8;

        do {
            block_until_rumble_pak_free();
            triesLeft--;
            status = osEepromLongRead(&gSIEventMesgQueue, offset, buffer, size);
            release_rumble_pak_control();
        } while (triesLeft > 0 && status != 0);
    }

    return status;
}

/**
 * Write data to EEPROM.
 * The EEPROM address was originally computed using the offset of the source address from gSaveBuffer.
 * Try at most 4 times, and return 0 on success. On failure, return the status returned from
 * osEepromLongWrite. Unlike read_eeprom_data, return 1 if EEPROM isn't loaded.
 */
static s32 write_eeprom_data(void *buffer, s32 size, const uintptr_t baseofs) {
    s32 status = 1;

    if (gEepromProbe != 0) {
        s32 triesLeft = 4;
        u32 offset = (u32)baseofs >> 3;

        do {
            block_until_rumble_pak_free();
            triesLeft--;
            status = osEepromLongWrite(&gSIEventMesgQueue, offset, buffer, size);
            release_rumble_pak_control();
        } while (triesLeft > 0 && status != 0);
    }

    return status;
}

/**
 * Wrappers that byteswap the data on LE platforms before writing it to 'EEPROM'
 */

static inline s32 write_eeprom_savefile(const u32 file, const u32 slot, const u32 num) {
    if (INVALID_FILE_INDEX(file)) { return 0; }
    if (INVALID_SRC_SLOT(slot)) { return 0; }
    // calculate the EEPROM address using the file number and slot
    const uintptr_t ofs = (u8*)&gSaveBuffer.files[file][slot] - (u8*)&gSaveBuffer;

#if IS_BIG_ENDIAN
    return write_eeprom_data(&gSaveBuffer.files[file][slot], num * sizeof(struct SaveFile), ofs);
#else
    // byteswap the data and then write it
    struct SaveFile sf[num];
    bcopy(&gSaveBuffer.files[file][slot], sf, num * sizeof(sf[0]));
    for (u32 i = 0; i < num; ++i) bswap_savefile(&sf[i]);
    return write_eeprom_data(&sf, sizeof(sf), ofs);
#endif
}

static inline s32 write_eeprom_menudata(const u32 slot, const u32 num) {
    if (INVALID_SRC_SLOT(slot)) { return 0; }
    // calculate the EEPROM address using the slot
    const uintptr_t ofs = (u8*)&gSaveBuffer.menuData[slot] - (u8*)&gSaveBuffer;

#if IS_BIG_ENDIAN
    return write_eeprom_data(&gSaveBuffer.menuData[slot], num * sizeof(struct MainMenuSaveData), ofs);
#else
    // byteswap the data and then write it
    struct MainMenuSaveData md[num];
    bcopy(&gSaveBuffer.menuData[slot], md, num * sizeof(md[0]));
    for (u32 i = 0; i < num; ++i) bswap_menudata(&md[i]);
    return write_eeprom_data(&md, sizeof(md), ofs);
#endif
}

/**
 * Sum the bytes in data to data + size - 2. The last two bytes are ignored
 * because that is where the checksum is stored.
 */
static u16 calc_checksum(u8 *data, s32 size) {
    u16 chksum = 0;

    while (size-- > 2) {
        chksum += *data++;
    }
    return chksum;
}

/**
 * Verify the signature at the end of the block to check if the data is valid.
 */
UNUSED static s32 verify_save_block_signature(void *buffer, s32 size, u16 magic) {
    struct SaveBlockSignature *sig = (struct SaveBlockSignature *) ((size - 4) + (u8 *) buffer);

    if (sig->magic != magic) {
        return FALSE;
    }
    if (sig->chksum != calc_checksum(buffer, size)) {
        return FALSE;
    }
    return TRUE;
}

/**
 * Write a signature at the end of the block to make sure the data is valid
 */
static void add_save_block_signature(void *buffer, s32 size, u16 magic) {
    struct SaveBlockSignature *sig = (struct SaveBlockSignature *) ((size - 4) + (u8 *) buffer);

    sig->magic = magic;
    sig->chksum = calc_checksum(buffer, size);
}

/**
 * Copy main menu data from one backup slot to the other slot.
 */
UNUSED static void restore_main_menu_data(s32 srcSlot) {
    if (INVALID_SRC_SLOT(srcSlot)) { return; }
    s32 destSlot = srcSlot ^ 1;
    if (INVALID_SRC_SLOT(destSlot)) { return; }

    // Compute checksum on source data
    add_save_block_signature(&gSaveBuffer.menuData[srcSlot], sizeof(gSaveBuffer.menuData[srcSlot]), MENU_DATA_MAGIC);

    // Copy source data to destination
    bcopy(&gSaveBuffer.menuData[srcSlot], &gSaveBuffer.menuData[destSlot], sizeof(gSaveBuffer.menuData[destSlot]));

    // Write destination data to EEPROM
    write_eeprom_menudata(destSlot, 1);
}

static void save_main_menu_data(void) {
    if (gMainMenuDataModified) {
        // Compute checksum
        add_save_block_signature(&gSaveBuffer.menuData[0], sizeof(gSaveBuffer.menuData[0]), MENU_DATA_MAGIC);

        // Back up data
        bcopy(&gSaveBuffer.menuData[0], &gSaveBuffer.menuData[1], sizeof(gSaveBuffer.menuData[1]));

        // Write to EEPROM
        write_eeprom_menudata(0, 2);

        gMainMenuDataModified = FALSE;
    }
}

UNUSED static void wipe_main_menu_data(void) {
    bzero(&gSaveBuffer.menuData[0], sizeof(gSaveBuffer.menuData[0]));

    // Set score ages for all courses to 3, 2, 1, and 0, respectively.
    gSaveBuffer.menuData[0].coinScoreAges[0] = 0x3FFFFFFF;
    gSaveBuffer.menuData[0].coinScoreAges[1] = 0x2AAAAAAA;
    gSaveBuffer.menuData[0].coinScoreAges[2] = 0x15555555;

    gMainMenuDataModified = TRUE;
    save_main_menu_data();
}

static s32 get_coin_score_age(s32 fileIndex, s32 courseIndex) {
    if (INVALID_FILE_INDEX(fileIndex)) { return 0; }
    return (gSaveBuffer.menuData[0].coinScoreAges[fileIndex] >> (2 * courseIndex)) & 0x3;
}

static void set_coin_score_age(s32 fileIndex, s32 courseIndex, s32 age) {
    if (INVALID_FILE_INDEX(fileIndex)) { return; }
    s32 mask = 0x3 << (2 * courseIndex);

    gSaveBuffer.menuData[0].coinScoreAges[fileIndex] &= ~mask;
    gSaveBuffer.menuData[0].coinScoreAges[fileIndex] |= age << (2 * courseIndex);
}

/**
 * Mark a coin score for a save file as the newest out of all save files.
 */
void touch_coin_score_age(s32 fileIndex, s32 courseIndex) {
    if (INVALID_FILE_INDEX(fileIndex)) { return; }
    s32 i;
    u32 age;
    u32 currentAge = get_coin_score_age(fileIndex, courseIndex);

    if (currentAge != 0) {
        for (i = 0; i < NUM_SAVE_FILES; i++) {
            age = get_coin_score_age(i, courseIndex);
            if (age < currentAge) {
                set_coin_score_age(i, courseIndex, age + 1);
            }
        }

        set_coin_score_age(fileIndex, courseIndex, 0);
        gMainMenuDataModified = TRUE;
    }
}

/**
 * Mark all coin scores for a save file as new.
 */
static void touch_high_score_ages(s32 fileIndex) {
    if (INVALID_FILE_INDEX(fileIndex)) { return; }
    s32 i;

    for (i = 0; i < 15; i++) {
        touch_coin_score_age(fileIndex, i);
    }
}

/**
 * Copy save file data from one backup slot to the other slot.
 */
UNUSED static void restore_save_file_data(s32 fileIndex, s32 srcSlot) {
    if (INVALID_FILE_INDEX(fileIndex)) { return; }
    if (INVALID_SRC_SLOT(srcSlot)) { return; }
    s32 destSlot = srcSlot ^ 1;
    if (INVALID_SRC_SLOT(destSlot)) { return; }

    // Compute checksum on source data
    add_save_block_signature(&gSaveBuffer.files[fileIndex][srcSlot],
                             sizeof(gSaveBuffer.files[fileIndex][srcSlot]), SAVE_FILE_MAGIC);

    // Copy source data to destination slot
    bcopy(&gSaveBuffer.files[fileIndex][srcSlot], &gSaveBuffer.files[fileIndex][destSlot],
          sizeof(gSaveBuffer.files[fileIndex][destSlot]));

    // Write destination data to EEPROM
    write_eeprom_savefile(fileIndex, destSlot, 1);
}

/**
 * Check if the 'EEPROM' save has different endianness (e.g. it's from an actual N64).
 */
static u8 save_file_need_bswap(const struct SaveBuffer *buf) {
    // check all signatures just in case
    for (s32 i = 0; i < 2; ++i) {
        if (buf->menuData[i].signature.magic == BSWAP16(MENU_DATA_MAGIC))
            return TRUE;
        for (s32 j = 0; j < NUM_SAVE_FILES; ++j) {
            if (buf->files[j][i].signature.magic == BSWAP16(SAVE_FILE_MAGIC))
                return TRUE;
        }
    }
    return FALSE;
}

/**
 * Byteswap all multibyte fields in a SaveBuffer.
 */
static void save_file_bswap(struct SaveBuffer *buf) {
    bswap_menudata(buf->menuData + 0);
    bswap_menudata(buf->menuData + 1);
    for (s32 i = 0; i < NUM_SAVE_FILES; ++i) {
        bswap_savefile(buf->files[i] + 0);
        bswap_savefile(buf->files[i] + 1);
    }
}

void save_file_do_save(s32 fileIndex, s8 forceSave) {
    if (INVALID_FILE_INDEX(fileIndex)) { return; }
    if (gNetworkType != NT_SERVER) {
        if (gNetworkType == NT_CLIENT) { network_send_save_file(fileIndex); return; }
        else if (gNetworkType == NT_NONE && !forceSave) { return; }
    }

    if (fileIndex < 0 || fileIndex >= NUM_SAVE_FILES)
        return;

    if (gSaveFileModified) {
        // Compute checksum
        add_save_block_signature(&gSaveBuffer.files[fileIndex][0],
                                 sizeof(gSaveBuffer.files[fileIndex][0]), SAVE_FILE_MAGIC);

        // Copy to backup slot
        //bcopy(&gSaveBuffer.files[fileIndex][0], &gSaveBuffer.files[fileIndex][1],
              //sizeof(gSaveBuffer.files[fileIndex][1]));

        // Write to EEPROM
        write_eeprom_savefile(fileIndex, 0, 2);

        gSaveFileModified = FALSE;
    }
    save_main_menu_data();
}

void save_file_erase(s32 fileIndex) {
    if (INVALID_FILE_INDEX(fileIndex)) { return; }

    touch_high_score_ages(fileIndex);
    bzero(&gSaveBuffer.files[fileIndex][0], sizeof(gSaveBuffer.files[fileIndex][0]));
    bzero(&gSaveBuffer.files[fileIndex][1], sizeof(gSaveBuffer.files[fileIndex][1]));

    gSaveFileModified = TRUE;
    save_file_do_save(fileIndex, TRUE);
}

void save_file_reload(u8 loadAll) {
    gSaveFileModified = TRUE;
    update_all_mario_stars();

    if (loadAll) {
        save_file_load_all(TRUE);
        save_file_do_save(gCurrSaveFileNum - 1, TRUE);
        update_all_mario_stars();
    }
}

void save_file_erase_current_backup_save(void) {
    if (INVALID_FILE_INDEX(gCurrSaveFileNum-1)) { return; }
    if (gNetworkType != NT_SERVER) { return; }

    bzero(&gSaveBuffer.files[gCurrSaveFileNum - 1][1], sizeof(gSaveBuffer.files[gCurrSaveFileNum - 1][1]));
    save_file_reload(FALSE);
    save_file_do_save(gCurrSaveFileNum - 1, TRUE);
}

//! Needs to be s32 to match on -O2, despite no return value.
BAD_RETURN(s32) save_file_copy(s32 srcFileIndex, s32 destFileIndex) {
    if (INVALID_FILE_INDEX(srcFileIndex)) { return; }
    if (INVALID_FILE_INDEX(destFileIndex)) { return; }

    touch_high_score_ages(destFileIndex);
    bcopy(&gSaveBuffer.files[srcFileIndex][0], &gSaveBuffer.files[destFileIndex][0],
          sizeof(gSaveBuffer.files[destFileIndex][0]));
    bcopy(&gSaveBuffer.files[srcFileIndex][1], &gSaveBuffer.files[destFileIndex][1],
          sizeof(gSaveBuffer.files[destFileIndex][1]));

    gSaveFileModified = TRUE;
    save_file_do_save(destFileIndex, TRUE);
}

void save_file_load_all(UNUSED u8 reload) {
    //s32 file;

    gMainMenuDataModified = FALSE;
    gSaveFileModified = FALSE;

    bzero(&gSaveBuffer, sizeof(gSaveBuffer));

    read_eeprom_data(&gSaveBuffer, sizeof(gSaveBuffer));

    if (save_file_need_bswap(&gSaveBuffer))
        save_file_bswap(&gSaveBuffer);

    // Verify the main menu data and create a backup copy if only one of the slots is valid.
    /* Disable this so the 'backup' slot can be used
    s32 validSlots;
    validSlots = verify_save_block_signature(&gSaveBuffer.menuData[0], sizeof(gSaveBuffer.menuData[0]), MENU_DATA_MAGIC);
    validSlots |= verify_save_block_signature(&gSaveBuffer.menuData[1], sizeof(gSaveBuffer.menuData[1]),MENU_DATA_MAGIC) << 1;
    switch (validSlots) {
        case 0: // Neither copy is correct
            wipe_main_menu_data();
            break;
        case 1: // Slot 0 is correct and slot 1 is incorrect
            restore_main_menu_data(0);
            break;
        case 2: // Slot 1 is correct and slot 0 is incorrect
            restore_main_menu_data(1);
            break;
    }

    for (file = 0; file < NUM_SAVE_FILES; file++) {
        // Verify the save file and create a backup copy if only one of the slots is valid.
        validSlots = verify_save_block_signature(&gSaveBuffer.files[file][0], sizeof(gSaveBuffer.files[file][0]), SAVE_FILE_MAGIC);
        validSlots |= verify_save_block_signature(&gSaveBuffer.files[file][1], sizeof(gSaveBuffer.files[file][1]), SAVE_FILE_MAGIC) << 1;
        switch (validSlots) {
            case 0: // Neither copy is correct
                save_file_erase(file);
                break;
            case 1: // Slot 0 is correct and slot 1 is incorrect
                restore_save_file_data(file, 0);
                break;
            case 2: // Slot 1 is correct and slot 0 is incorrect
                restore_save_file_data(file, 1);
                break;
        }
    }
    */
    stub_save_file_1();
}

/**
 * Update the current save file after collecting a star or a key.
 * If coin score is greater than the current high score, update it.
 */
void save_file_collect_star_or_key(s16 coinScore, s16 starIndex, u8 fromNetwork) {

    s32 fileIndex = gCurrSaveFileNum - 1;
    s32 courseIndex = gCurrCourseNum - 1;
    if (INVALID_FILE_INDEX(fileIndex)) { return; }
    if (INVALID_SRC_SLOT(gSaveFileUsingBackupSlot)) { return; }

    s32 starByte = (starIndex / 7) - 1;
    s32 starFlag = 1 << (gLevelValues.useGlobalStarIds ? (starIndex % 7) : starIndex);
    UNUSED s32 flags = save_file_get_flags();

    if (!fromNetwork) {
        gLastCompletedCourseNum = courseIndex + 1;
        gLastCompletedStarNum = starIndex + 1;
        sUnusedGotGlobalCoinHiScore = 0;
        gGotFileCoinHiScore = FALSE;
    }

    if (!INVALID_COURSE_COIN_INDEX(courseIndex) && !fromNetwork) {
        //! Compares the coin score as a 16 bit value, but only writes the 8 bit
        // truncation. This can allow a high score to decrease.

        if (coinScore > ((u16) save_file_get_max_coin_score(courseIndex) & 0xFFFF)) {
            sUnusedGotGlobalCoinHiScore = 1;
        }

        if (coinScore > save_file_get_course_coin_score(fileIndex, courseIndex)) {
            gSaveBuffer.files[fileIndex][gSaveFileUsingBackupSlot].courseCoinScores[courseIndex] = coinScore;
            touch_coin_score_age(fileIndex, courseIndex);

            gGotFileCoinHiScore = TRUE;
            gSaveFileModified = TRUE;
        }
    }

    s32 index = gLevelValues.useGlobalStarIds ? starByte : courseIndex;
    switch (gCurrLevelNum) {
        case LEVEL_BOWSER_1:
            if (!(save_file_get_flags() & (SAVE_FLAG_HAVE_KEY_1 | SAVE_FLAG_UNLOCKED_BASEMENT_DOOR))) {
                save_file_set_flags(SAVE_FLAG_HAVE_KEY_1);
            }
            break;

        case LEVEL_BOWSER_2:
            if (!(save_file_get_flags() & (SAVE_FLAG_HAVE_KEY_2 | SAVE_FLAG_UNLOCKED_UPSTAIRS_DOOR))) {
                save_file_set_flags(SAVE_FLAG_HAVE_KEY_2);
            }
            break;

        case LEVEL_BOWSER_3:
            break;

        default:
            if (!(save_file_get_star_flags(fileIndex, index) & starFlag)) {
                save_file_set_star_flags(fileIndex, index, starFlag);
            }
            break;
    }
}

s32 save_file_exists(s32 fileIndex) {
    if (INVALID_FILE_INDEX(fileIndex)) { return 0; }
    return (gSaveBuffer.files[fileIndex][0].flags & SAVE_FLAG_FILE_EXISTS) != 0;
}

/**
 * Get the maximum coin score across all files for a course. The lower 16 bits
 * of the returned value are the score, and the upper 16 bits are the file number
 * of the save file with this score.
 */
u32 save_file_get_max_coin_score(s32 courseIndex) {
    s32 fileIndex;
    s32 maxCoinScore = -1;
    s32 maxScoreAge = -1;
    s32 maxScoreFileNum = 0;

    for (fileIndex = 0; fileIndex < NUM_SAVE_FILES; fileIndex++) {
        if (save_file_get_star_flags(fileIndex, courseIndex) != 0) {
            s32 coinScore = save_file_get_course_coin_score(fileIndex, courseIndex);
            s32 scoreAge = get_coin_score_age(fileIndex, courseIndex);

            if (coinScore > maxCoinScore || (coinScore == maxCoinScore && scoreAge > maxScoreAge)) {
                maxCoinScore = coinScore;
                maxScoreAge = scoreAge;
                maxScoreFileNum = fileIndex + 1;
            }
        }
    }
    return (maxScoreFileNum << 16) + max(maxCoinScore, 0);
}

s32 save_file_get_course_star_count(s32 fileIndex, s32 courseIndex) {
    if (INVALID_FILE_INDEX(fileIndex)) { return 0; }
    s32 i;
    s32 count = 0;
    u8 flag = 1;
    u8 starFlags = save_file_get_star_flags(fileIndex, courseIndex);

    for (i = 0; i < 7; i++, flag <<= 1) {
        if (starFlags & flag) {
            count++;
        }
    }
    return count;
}

s32 save_file_get_total_star_count(s32 fileIndex, s32 minCourse, s32 maxCourse) {
    if (INVALID_FILE_INDEX(fileIndex)) { return 02; }
    s32 count = 0;

    if (minCourse < -1) { minCourse = -1; }
    if (maxCourse >= COURSE_COUNT) { maxCourse = COURSE_COUNT-1; }

    // Get standard course star count.
    for (; minCourse <= maxCourse; minCourse++) {
        count += save_file_get_course_star_count(fileIndex, minCourse);
    }

    // Add castle secret star count.
    return save_file_get_course_star_count(fileIndex, -1) + count;
}

void save_file_set_flags(u32 flags) {
    if (INVALID_FILE_INDEX(gCurrSaveFileNum - 1)) { return; }
    if (INVALID_SRC_SLOT(gSaveFileUsingBackupSlot)) { return; }
    // prevent saving any flag that would make the player hatless on level transition
    flags &= ~(SAVE_FLAG_CAP_ON_GROUND | SAVE_FLAG_CAP_ON_KLEPTO | SAVE_FLAG_CAP_ON_MR_BLIZZARD | SAVE_FLAG_CAP_ON_UKIKI);
    if (flags == 0) { return; }

    gSaveBuffer.files[gCurrSaveFileNum - 1][gSaveFileUsingBackupSlot].flags |= (flags | SAVE_FLAG_FILE_EXISTS);
    gSaveFileModified = TRUE;
    network_send_save_set_flag(gCurrSaveFileNum - 1, 0, 0, (flags | SAVE_FLAG_FILE_EXISTS));
}

void save_file_clear_flags(u32 flags) {
    if (INVALID_FILE_INDEX(gCurrSaveFileNum - 1)) { return; }
    if (INVALID_SRC_SLOT(gSaveFileUsingBackupSlot)) { return; }
    gSaveBuffer.files[gCurrSaveFileNum - 1][gSaveFileUsingBackupSlot].flags &= ~flags;
    gSaveBuffer.files[gCurrSaveFileNum - 1][gSaveFileUsingBackupSlot].flags |= SAVE_FLAG_FILE_EXISTS;
    gSaveFileModified = TRUE;
}

u32 save_file_get_flags(void) {
    if (INVALID_FILE_INDEX(gCurrSaveFileNum - 1)) { return 0; }
    if (INVALID_SRC_SLOT(gSaveFileUsingBackupSlot)) { return 0; }
    if (gCurrCreditsEntry != NULL || gCurrDemoInput != NULL) {
        return 0;
    }
    return gSaveBuffer.files[gCurrSaveFileNum - 1][gSaveFileUsingBackupSlot].flags;
}

/**
 * Return the bitset of obtained stars in the specified course.
 * If course is -1, return the bitset of obtained castle secret stars.
 */
u32 save_file_get_star_flags(s32 fileIndex, s32 courseIndex) {
    if (INVALID_FILE_INDEX(fileIndex)) { return 0; }
    if (INVALID_SRC_SLOT(gSaveFileUsingBackupSlot)) { return 0; }
    u32 starFlags = 0;

    if (courseIndex == -1) {
        starFlags = SAVE_FLAG_TO_STAR_FLAG(gSaveBuffer.files[fileIndex][gSaveFileUsingBackupSlot].flags);
    } else if (!INVALID_COURSE_STAR_INDEX(courseIndex)) {
        starFlags = gSaveBuffer.files[fileIndex][gSaveFileUsingBackupSlot].courseStars[courseIndex] & 0x7F;
    }

    return starFlags;
}

/**
 * Add to the bitset of obtained stars in the specified course.
 * If course is -1, add to the bitset of obtained castle secret stars.
 */
void save_file_set_star_flags(s32 fileIndex, s32 courseIndex, u32 starFlags) {
    if (INVALID_FILE_INDEX(fileIndex)) { return; }
    if (INVALID_SRC_SLOT(gSaveFileUsingBackupSlot)) { return; }
    if (courseIndex == -1) {
        gSaveBuffer.files[fileIndex][gSaveFileUsingBackupSlot].flags |= STAR_FLAG_TO_SAVE_FLAG(starFlags);
        network_send_save_set_flag(fileIndex, courseIndex, 0, (STAR_FLAG_TO_SAVE_FLAG(starFlags) | SAVE_FLAG_FILE_EXISTS));
    } else if (!INVALID_COURSE_STAR_INDEX(courseIndex)) {
        gSaveBuffer.files[fileIndex][gSaveFileUsingBackupSlot].courseStars[courseIndex] |= starFlags;
        network_send_save_set_flag(fileIndex, courseIndex, starFlags, SAVE_FLAG_FILE_EXISTS);
    }

    gSaveBuffer.files[fileIndex][gSaveFileUsingBackupSlot].flags |= SAVE_FLAG_FILE_EXISTS;
    gSaveFileModified = TRUE;
}

void save_file_remove_star_flags(s32 fileIndex, s32 courseIndex, u32 starFlagsToRemove) {
    if (INVALID_FILE_INDEX(fileIndex)) { return; }
    if (INVALID_SRC_SLOT(gSaveFileUsingBackupSlot)) { return; }

    if (courseIndex == -1) {
        gSaveBuffer.files[fileIndex][gSaveFileUsingBackupSlot].flags &= ~STAR_FLAG_TO_SAVE_FLAG(starFlagsToRemove);
        network_send_save_remove_flag(fileIndex, courseIndex, 0, STAR_FLAG_TO_SAVE_FLAG(starFlagsToRemove));
    }
    else if (!INVALID_COURSE_STAR_INDEX(courseIndex)) {
        gSaveBuffer.files[fileIndex][gSaveFileUsingBackupSlot].courseStars[courseIndex] &= ~starFlagsToRemove;
        network_send_save_remove_flag(fileIndex, courseIndex, starFlagsToRemove, 0);
    }

    gSaveFileModified = TRUE;
}

s32 save_file_get_course_coin_score(s32 fileIndex, s32 courseIndex) {
    if (INVALID_FILE_INDEX(fileIndex)) { return 0; }
    if (INVALID_SRC_SLOT(gSaveFileUsingBackupSlot)) { return 0; }
    if (INVALID_COURSE_COIN_INDEX(courseIndex)) { return 0; }
    u8 coinScore = gSaveBuffer.files[fileIndex][gSaveFileUsingBackupSlot].courseCoinScores[courseIndex];

    // sanity check - if we've collected 100 coin star... we have to have had at least 100
    if (coinScore < 100) {
        u8 stars = save_file_get_star_flags(fileIndex, courseIndex);
        if ((stars & (1 << 6))) {
            coinScore = 100;
        }
    }

    return coinScore;
}

void save_file_set_course_coin_score(s32 fileIndex, s32 courseIndex, u8 coinScore) {
    if (INVALID_FILE_INDEX(fileIndex)) { return; }
    if (INVALID_SRC_SLOT(gSaveFileUsingBackupSlot)) { return; }
    if (INVALID_COURSE_COIN_INDEX(courseIndex)) { return; }
    gSaveBuffer.files[fileIndex][gSaveFileUsingBackupSlot].courseCoinScores[courseIndex] = coinScore;
}

/**
 * Return TRUE if the cannon is unlocked in the current course.
 */
s32 save_file_is_cannon_unlocked(s32 fileIndex, s32 courseIndex) {
    if (INVALID_FILE_INDEX(fileIndex)) { return 0; }
    if (INVALID_SRC_SLOT(gSaveFileUsingBackupSlot)) { return 0; }
    if (INVALID_COURSE_STAR_INDEX(courseIndex)) { return 0; }
    return (gSaveBuffer.files[fileIndex][gSaveFileUsingBackupSlot].courseStars[courseIndex] & 0x80) != 0;
}

/**
 * Sets the cannon status to unlocked in the current course.
 */
void save_file_set_cannon_unlocked(void) {
    if (INVALID_FILE_INDEX(gCurrSaveFileNum - 1)) { return; }
    if (INVALID_SRC_SLOT(gSaveFileUsingBackupSlot)) { return; }
    if (INVALID_COURSE_STAR_INDEX(gCurrCourseNum)) { return; }
    gSaveBuffer.files[gCurrSaveFileNum - 1][gSaveFileUsingBackupSlot].courseStars[gCurrCourseNum] |= 0x80;
    gSaveBuffer.files[gCurrSaveFileNum - 1][gSaveFileUsingBackupSlot].flags |= SAVE_FLAG_FILE_EXISTS;
    gSaveFileModified = TRUE;
    network_send_save_set_flag(gCurrSaveFileNum - 1, gCurrCourseNum, 0x80, SAVE_FLAG_FILE_EXISTS);
}

void save_file_set_cap_pos(s16 x, s16 y, s16 z) {
    if (INVALID_FILE_INDEX(gCurrSaveFileNum - 1)) { return; }
    if (INVALID_SRC_SLOT(gSaveFileUsingBackupSlot)) { return; }
    struct SaveFile *saveFile = &gSaveBuffer.files[gCurrSaveFileNum - 1][gSaveFileUsingBackupSlot];

    saveFile->capLevel = gCurrLevelNum;
    saveFile->capArea = gCurrAreaIndex;
    vec3s_set(saveFile->capPos, x, y, z);
    save_file_set_flags(SAVE_FLAG_CAP_ON_GROUND);
}

s32 save_file_get_cap_pos(OUT Vec3s capPos) {
    if (INVALID_FILE_INDEX(gCurrSaveFileNum - 1)) { return 0; }
    if (INVALID_SRC_SLOT(gSaveFileUsingBackupSlot)) { return 0; }
    struct SaveFile *saveFile = &gSaveBuffer.files[gCurrSaveFileNum - 1][gSaveFileUsingBackupSlot];
    s32 flags = save_file_get_flags();

    if (saveFile->capLevel == gCurrLevelNum && saveFile->capArea == gCurrAreaIndex
        && (flags & SAVE_FLAG_CAP_ON_GROUND)) {
        vec3s_copy(capPos, saveFile->capPos);
        return TRUE;
    }
    return FALSE;
}

void save_file_set_sound_mode(u16 mode) {
    set_sound_mode(mode);
    gSaveBuffer.menuData[0].soundMode = mode;

    gMainMenuDataModified = TRUE;
    save_main_menu_data();
}

u16 save_file_get_sound_mode(void) {
    return gSaveBuffer.menuData[0].soundMode;
}

void save_file_move_cap_to_default_location(void) {
    if (INVALID_FILE_INDEX(gCurrSaveFileNum - 1)) { return; }
    if (INVALID_SRC_SLOT(gSaveFileUsingBackupSlot)) { return; }
    if (save_file_get_flags() & SAVE_FLAG_CAP_ON_GROUND || gMarioStates[0].cap == SAVE_FLAG_CAP_ON_GROUND) {
        switch (gSaveBuffer.files[gCurrSaveFileNum - 1][gSaveFileUsingBackupSlot].capLevel) {
            case LEVEL_SSL:
                gMarioStates[0].cap = SAVE_FLAG_CAP_ON_KLEPTO;
                break;
            case LEVEL_SL:
                gMarioStates[0].cap = SAVE_FLAG_CAP_ON_MR_BLIZZARD;
                break;
            case LEVEL_TTM:
                gMarioStates[0].cap = SAVE_FLAG_CAP_ON_UKIKI;
                break;
        }
        save_file_clear_flags(SAVE_FLAG_CAP_ON_GROUND);
    }
}

#ifdef VERSION_EU
void eu_set_language(u16 language) {
    gSaveBuffer.menuData[0].language = language;
    gMainMenuDataModified = TRUE;
    save_main_menu_data();
}

u16 eu_get_language(void) {
    // check if the language is in range, in case we loaded a US save with garbage padding or something
    if (gSaveBuffer.menuData[0].language >= LANGUAGE_MAX)
        eu_set_language(LANGUAGE_ENGLISH); // reset it to english if not
    return gSaveBuffer.menuData[0].language;
}
#endif

void disable_warp_checkpoint(void) {
    // check_warp_checkpoint() checks to see if gWarpCheckpoint.courseNum != COURSE_NONE
    gWarpCheckpoint.courseNum = COURSE_NONE;
}

/**
 * Checks the upper bit of the WarpNode->destLevel byte to see if the
 * game should set a warp checkpoint.
 */
void check_if_should_set_warp_checkpoint(struct WarpNode *warpNode) {
    if (warpNode->destLevel & 0x80) {
        // Overwrite the warp checkpoint variables.
        gWarpCheckpoint.actNum = gCurrActNum;
        gWarpCheckpoint.courseNum = gCurrCourseNum;
        gWarpCheckpoint.levelID = warpNode->destLevel & 0x7F;
        gWarpCheckpoint.areaNum = warpNode->destArea;
        gWarpCheckpoint.warpNode = warpNode->destNode;
    }
}

/**
 * Checks to see if a checkpoint is properly active or not. This will
 * also update the level, area, and destination node of the input WarpNode.
 * returns TRUE if input WarpNode was updated, and FALSE if not.
 */
s32 check_warp_checkpoint(struct WarpNode *warpNode) {
    s16 warpCheckpointActive = FALSE;
    s16 currCourseNum = get_level_course_num(warpNode->destLevel & 0x7F);

    // gSavedCourseNum is only used in this function.
    if (gWarpCheckpoint.courseNum != COURSE_NONE && gSavedCourseNum == currCourseNum
        && gWarpCheckpoint.actNum == gCurrActNum) {
        warpNode->destLevel = gWarpCheckpoint.levelID;
        warpNode->destArea = gWarpCheckpoint.areaNum;
        warpNode->destNode = gWarpCheckpoint.warpNode;
        warpCheckpointActive = TRUE;
    } else {
        // Disable the warp checkpoint just in case the other 2 conditions failed?
        gWarpCheckpoint.courseNum = COURSE_NONE;
    }

    return warpCheckpointActive;
}
