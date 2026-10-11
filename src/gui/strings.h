// USF Maker - user interface text (Japanese / English).
// Copyright (C) 2026 Nekokabu.  GPL-2.0-or-later, see LICENSE.
#pragma once

enum StrId {
    S_APP_TITLE,
    S_ROM_LABEL,
    S_ROM_OPEN,
    S_ROM_HINT,
    S_GAME_LABEL,
    S_ARTIST_LABEL,
    S_TIME_LABEL,
    S_TIME_1, S_TIME_3, S_TIME_5, S_TIME_10, S_TIME_30, S_TIME_FOREVER,
    S_PRESS_BUTTONS,
    S_SETTINGS,
    S_START,
    S_STOP,
    S_COL_TITLE, S_COL_LENGTH, S_COL_KIND, S_COL_LOOP, S_COL_FOUND, S_COL_RATE, S_COL_NOTE,
    S_KIND_LOOP, S_KIND_END, S_KIND_UNKNOWN,
    S_ORIGIN_IDLE, S_ORIGIN_EXPLORED, S_ORIGIN_IMPORT,
    S_NOTE_LLE, S_NOTE_STALL,
    S_PLAY, S_PLAY_STOP,
    S_RENAME, S_DELETE, S_SELECT_ALL,
    S_IMPORT,
    S_SAVE,
    S_HELP,
    S_STATUS_NO_ROM,
    S_STATUS_READY,
    S_STATUS_RUNNING,        // elapsed, budget, songs, candidates, duplicates, rejected, waiting
    S_STATUS_RUNNING_FOREVER,
    S_STATUS_FINISHING,
    S_STATUS_DONE,           // songs
    S_STATUS_STOPPED,        // songs
    S_STATUS_IMPORTING,      // file name
    S_STATUS_PLAYING,        // title
    S_ROM_INFO,              // name, code, cic, tv, size MB
    S_ERR_TITLE,
    S_ERR_ROM,               // reason
    S_ERR_BUSY,
    S_ERR_NO_SONGS,
    S_ERR_AUDIO,
    S_ERR_SAVE,              // reason
    S_ERR_STATE,             // file, reason
    S_ASK_REPLACE_ROM,
    S_SAVE_PICK_FOLDER,
    S_SAVE_DONE,             // files, folder, MB, rom MB kept, rom MB total
    S_SAVE_OPEN_FOLDER,
    S_IMPORT_FILTER,
    S_ROM_FILTER,
    S_IMPORT_DONE,           // added, failed
    S_LOG_SONG,              // title, length, kind
    S_SET_TITLE,
    S_SET_GROUP_EMU, S_SET_GROUP_SONG, S_SET_GROUP_OUT,
    S_SET_EXPANSION, S_SET_IDLE, S_SET_THREADS, S_SET_SEED,
    S_SET_LOOPS, S_SET_FADE, S_SET_DEFLEN, S_SET_MAXLEN,
    S_SET_FULLROM,
    S_BTN_OK, S_BTN_CANCEL,
    S_HELP_TEXT,
    S_CLOSE_WHILE_RUNNING,
    S_ROM_NONAME,
    S_LOG_START,             // ROM file name
    S_LOG_REPLACED,          // title
    S_LOG_STATE_CONVERTED,   // file name
    S_ERR_NEED_ROM,
    S_COUNT
};

// Selects the language: 1 = Japanese, 0 = English.
void set_language(int japanese);
const wchar_t* T(StrId id);
