// USF Maker - user interface text (Japanese / English).
// Copyright (C) 2026 Nekokabu.  GPL-2.0-or-later, see LICENSE.
#include "strings.h"

namespace {

struct Entry {
    const wchar_t* ja;
    const wchar_t* en;
};

int g_japanese = 1;

const Entry kText[S_COUNT] = {
    /* S_APP_TITLE */ { L"USF Maker", L"USF Maker" },
    /* S_ROM_LABEL */ { L"ROM:", L"ROM:" },
    /* S_ROM_OPEN */ { L"開く(&O)...", L"&Open..." },
    /* S_ROM_HINT */ { L"N64 の ROM ファイル (.z64 / .n64 / .v64) をこのウィンドウにドラッグ＆ドロップするか、[開く] で選んでください。",
                       L"Drop an N64 ROM file (.z64 / .n64 / .v64) on this window, or pick one with Open." },
    /* S_GAME_LABEL */ { L"ゲーム名:", L"Game:" },
    /* S_ARTIST_LABEL */ { L"作曲者:", L"Artist:" },
    /* S_TIME_LABEL */ { L"探索時間:", L"Search time:" },
    /* S_TIME_1 */ { L"1 分", L"1 minute" },
    /* S_TIME_3 */ { L"3 分", L"3 minutes" },
    /* S_TIME_5 */ { L"5 分", L"5 minutes" },
    /* S_TIME_10 */ { L"10 分", L"10 minutes" },
    /* S_TIME_30 */ { L"30 分", L"30 minutes" },
    /* S_TIME_FOREVER */ { L"停止するまで", L"Until stopped" },
    /* S_PRESS_BUTTONS */ { L"自動でボタンを押して先へ進む", L"Press buttons automatically" },
    /* S_SETTINGS */ { L"詳細設定(&T)...", L"Se&ttings..." },
    /* S_START */ { L"開始(&S)", L"&Start" },
    /* S_STOP */ { L"停止", L"Stop" },
    /* S_COL_TITLE */ { L"タイトル", L"Title" },
    /* S_COL_LENGTH */ { L"長さ", L"Length" },
    /* S_COL_KIND */ { L"種類", L"Type" },
    /* S_COL_LOOP */ { L"イントロ + ループ", L"Intro + loop" },
    /* S_COL_FOUND */ { L"見つけた場面", L"Found" },
    /* S_COL_RATE */ { L"レート", L"Rate" },
    /* S_COL_NOTE */ { L"備考", L"Notes" },
    /* S_KIND_LOOP */ { L"ループ", L"Loops" },
    /* S_KIND_END */ { L"終了する", L"Ends" },
    /* S_KIND_UNKNOWN */ { L"不明", L"Unknown" },
    /* S_ORIGIN_IDLE */ { L"起動後 %ls（無操作）", L"%ls after power-on (untouched)" },
    /* S_ORIGIN_EXPLORED */ { L"起動後 %ls（自動操作）", L"%ls after power-on (auto-play)" },
    /* S_ORIGIN_IMPORT */ { L"セーブステート", L"Save state" },
    /* S_NOTE_LLE */ { L"プレイヤーの HLE 音声では正しく鳴らない可能性", L"May need LLE audio in the player" },
    /* S_NOTE_STALL */ { L"この長さでゲーム側の音声が止まります", L"The game stops producing sound here" },
    /* S_PLAY */ { L"試聴(&P)", L"&Play" },
    /* S_PLAY_STOP */ { L"試聴停止(&P)", L"Sto&p" },
    /* S_RENAME */ { L"名前の変更(&R)", L"&Rename" },
    /* S_DELETE */ { L"削除(&D)", L"&Delete" },
    /* S_SELECT_ALL */ { L"すべて選択 / 解除", L"Check all / none" },
    /* S_IMPORT */ { L"セーブステートから追加(&A)...", L"&Add from save state..." },
    /* S_SAVE */ { L"USF を保存(&U)...", L"Save &USF set..." },
    /* S_HELP */ { L"使い方(&H)", L"&Help" },
    /* S_STATUS_NO_ROM */ { L"ROM を読み込んでください。", L"Load a ROM to begin." },
    /* S_STATUS_READY */ { L"準備完了。[開始] を押すと曲を探します。", L"Ready. Press Start to look for music." },
    /* S_STATUS_RUNNING */ { L"探索中 %ls / %ls ― 曲 %d、候補 %d（重複 %d・不採用 %d・解析待ち %d）",
                             L"Searching %ls / %ls - %d songs, %d candidates (%d duplicates, %d rejected, %d waiting)" },
    /* S_STATUS_RUNNING_FOREVER */ { L"探索中 %ls ― 曲 %d、候補 %d（重複 %d・不採用 %d・解析待ち %d）",
                                     L"Searching %ls - %d songs, %d candidates (%d duplicates, %d rejected, %d waiting)" },
    /* S_STATUS_FINISHING */ { L"見つかった曲を解析して仕上げています…", L"Finishing the analysis of what was found..." },
    /* S_STATUS_DONE */ { L"完了: %d 曲見つかりました。確認して [USF を保存] を押してください。", L"Done: %d songs found. Check them and press Save USF set." },
    /* S_STATUS_STOPPED */ { L"停止しました: %d 曲。", L"Stopped: %d songs." },
    /* S_STATUS_IMPORTING */ { L"セーブステートを解析中: %ls", L"Analysing save state: %ls" },
    /* S_STATUS_PLAYING */ { L"試聴中: %ls", L"Playing: %ls" },
    /* S_ROM_INFO */ { L"内部名「%ls」 %ls　CIC %ls　%ls　%.1f MB", L"Internal name \"%ls\" %ls   CIC %ls   %ls   %.1f MB" },
    /* S_ERR_TITLE */ { L"USF Maker", L"USF Maker" },
    /* S_ERR_ROM */ { L"この ROM は読み込めませんでした。\n\n%ls", L"This ROM could not be loaded.\n\n%ls" },
    /* S_ERR_BUSY */ { L"処理中です。終わるまでお待ちください。", L"Busy. Please wait until the current job is finished." },
    /* S_ERR_NO_SONGS */ { L"保存する曲にチェックを入れてください。", L"Check the songs you want to save." },
    /* S_ERR_AUDIO */ { L"試聴を開始できませんでした（オーディオデバイスを開けないか、この曲を再生できません）。",
                        L"Could not start playback (no audio device, or the song does not play)." },
    /* S_ERR_SAVE */ { L"保存に失敗しました。\n\n%ls", L"Saving failed.\n\n%ls" },
    /* S_ERR_STATE */ { L"%ls: %ls", L"%ls: %ls" },
    /* S_ASK_REPLACE_ROM */ { L"別の ROM を読み込むと、いま見つかっている曲の一覧は消えます。よろしいですか？",
                              L"Loading another ROM discards the songs found so far. Continue?" },
    /* S_SAVE_PICK_FOLDER */ { L"USF セットの保存先フォルダーを選んでください", L"Choose the folder for the USF set" },
    /* S_SAVE_DONE */ { L"%d 個のファイルを保存しました。\n\n%ls\n\n合計 %.1f MB（ROM %.1f MB のうち、再生に必要な %.1f MB だけを収録）\n\nフォルダーを開きますか？",
                        L"%d files saved.\n\n%ls\n\n%.1f MB in total (of the %.1f MB ROM, only the %.1f MB needed for playback are included)\n\nOpen the folder?" },
    /* S_SAVE_OPEN_FOLDER */ { L"フォルダーを開く", L"Open folder" },
    /* S_IMPORT_FILTER */ { L"Project64 セーブステート (*.pj;*.zip;*.pj?;*.st?)|*.pj;*.zip;*.pj?;*.st?|すべてのファイル (*.*)|*.*|",
                            L"Project64 save states (*.pj;*.zip;*.pj?;*.st?)|*.pj;*.zip;*.pj?;*.st?|All files (*.*)|*.*|" },
    /* S_ROM_FILTER */ { L"N64 ROM (*.z64;*.n64;*.v64;*.rom;*.bin;*.u64;*.zip)|*.z64;*.n64;*.v64;*.rom;*.bin;*.u64;*.zip|すべてのファイル (*.*)|*.*|",
                         L"N64 ROM (*.z64;*.n64;*.v64;*.rom;*.bin;*.u64;*.zip)|*.z64;*.n64;*.v64;*.rom;*.bin;*.u64;*.zip|All files (*.*)|*.*|" },
    /* S_IMPORT_DONE */ { L"セーブステートから %d 曲を追加しました（使えなかったもの: %d）。", L"Songs added from save states: %d (could not be used: %d)." },
    /* S_LOG_SONG */ { L"♪ %ls　%ls　%ls", L"+ %ls  %ls  %ls" },
    /* S_SET_TITLE */ { L"詳細設定", L"Settings" },
    /* S_SET_GROUP_EMU */ { L"探索", L"Search" },
    /* S_SET_GROUP_SONG */ { L"曲の長さ", L"Song length" },
    /* S_SET_GROUP_OUT */ { L"出力", L"Output" },
    /* S_SET_EXPANSION */ { L"拡張パックあり（メモリー 8 MB）で動かす", L"Run with Expansion Pak (8 MB of memory)" },
    /* S_SET_IDLE */ { L"起動後、無操作で様子を見る時間（秒）", L"Seconds to watch the untouched game" },
    /* S_SET_THREADS */ { L"同時に動かす数（0 = 自動）", L"Worker threads (0 = automatic)" },
    /* S_SET_SEED */ { L"自動操作のパターン番号", L"Pattern number for auto-play" },
    /* S_SET_LOOPS */ { L"ループ曲を繰り返す回数", L"Times a looping song is played" },
    /* S_SET_FADE */ { L"ループ曲のフェードアウト（秒）", L"Fade-out for looping songs (seconds)" },
    /* S_SET_DEFLEN */ { L"ループも終わりも不明な曲の長さ（秒）", L"Length when neither loop nor end is found (s)" },
    /* S_SET_MAXLEN */ { L"ループを探すために再生する最大時間（秒）", L"Longest time to render when looking for a loop (s)" },
    /* S_SET_FULLROM */ { L"ROM 全体を usflib に入れる（通常は再生に必要な部分だけを入れます）",
                          L"Put the whole ROM into the usflib (normally only what playback needs is included)" },
    /* S_BTN_OK */ { L"OK", L"OK" },
    /* S_BTN_CANCEL */ { L"キャンセル", L"Cancel" },
    /* S_HELP_TEXT */ {
        L"USF Maker は、N64 の ROM から USF（Ultra 64 Sound Format）を自動で作ります。\n"
        L"\n"
        L"【使い方】\n"
        L"1. ROM ファイル (.z64 / .n64 / .v64、zip 圧縮も可) をウィンドウにドロップします。\n"
        L"2. [開始] を押します。ゲームを画面なしで高速に動かし、自動でボタンを押しながら、音楽が鳴り始めた瞬間を集めます。\n"
        L"3. 見つかった曲が一覧に出ます。[試聴] で確認し、タイトルは F2 キーで変更できます。\n"
        L"4. [USF を保存] で、.usflib と曲ごとの .miniusf を書き出します。\n"
        L"\n"
        L"【全曲は見つからないとき】\n"
        L"自動操作でたどり着けない場面の曲は見つけられません。その場合は Project64 で目的の曲が鳴り始める直前にセーブステートを作り、"
        L"[セーブステートから追加] で読み込んでください（.pj / .pj.zip に対応。ステートファイルのドロップでも可）。\n"
        L"\n"
        L"【サウンドテストがあるゲーム】\n"
        L"ゲーム内にサウンドテスト（曲を選んで聴ける画面）がある場合は、その画面で Project64 のセーブステートを作り、"
        L"「ステートはサウンドテスト画面」にチェックを入れてから読み込んでください。曲を切り替えるボタンと再生するボタンを自動で探し、"
        L"一覧の曲を順番に再生して取り込みます。\n"
        L"\n"
        L"【再生について】\n"
        L"作成したファイルは foobar2000 (foo_input_usf) などの lazyusf2 系プレイヤーで再生できます。\n"
        L"\n"
        L"エミュレーションコア: lazyusf2 (mupen64plus 由来、GPL v2 以降)",
        L"USF Maker builds USF (Ultra 64 Sound Format) sets from N64 ROMs automatically.\n"
        L"\n"
        L"How to use\n"
        L"1. Drop a ROM file (.z64 / .n64 / .v64, zipped is fine) on the window.\n"
        L"2. Press Start. The game is run at high speed without video while buttons are pressed automatically, and every moment music starts is collected.\n"
        L"3. The songs appear in the list. Use Play to listen; press F2 to rename one.\n"
        L"4. Save USF set writes the .usflib and one .miniusf per song.\n"
        L"\n"
        L"When not everything is found\n"
        L"Music in places the automatic player can not reach is not found. In that case make a save state in Project64 just before the tune starts "
        L"and load it with Add from save state (.pj and .pj.zip; dropping the state file works too).\n"
        L"\n"
        L"Games with a sound test\n"
        L"If the game has a sound test (a screen where tunes can be selected and played), make a Project64 save state on that screen, "
        L"tick \"Save states are sound test screens\" and load the state. The keys that select and play a tune are found automatically "
        L"and every entry is played and captured in turn.\n"
        L"\n"
        L"Playback\n"
        L"The files play in lazyusf2 based players such as foobar2000 with foo_input_usf.\n"
        L"\n"
        L"Emulation core: lazyusf2 (derived from mupen64plus, GPL v2 or later)" },
    /* S_CLOSE_WHILE_RUNNING */ { L"探索中です。中止して終了しますか？", L"A search is running. Stop it and quit?" },
    /* S_ROM_NONAME */ { L"（なし）", L"(none)" },
    /* S_LOG_START */ { L"探索を開始しました: %ls", L"Search started: %ls" },
    /* S_LOG_REPLACED */ { L"「%ls」は、同じ曲のより良い取り込みに置き換えました。", L"\"%ls\" was replaced by a better capture of the same tune." },
    /* S_LOG_STATE_CONVERTED */ { L"%ls: 新しい Project64 のステート形式から変換しました。", L"%ls: converted from the newer Project64 save state layout." },
    /* S_ERR_NEED_ROM */ { L"このファイルは N64 の ROM として読み込めませんでした。\nセーブステートを追加する場合は、先にそのゲームの ROM を開いてください。",
                           L"This file could not be read as an N64 ROM.\nTo add save states, open the game's ROM first." },
    /* S_SOUNDTEST_CHECK */ { L"ステートはサウンドテスト画面（全曲を自動で取り込む）", L"Save states are sound test screens (capture every entry)" },
    /* S_STATUS_ST_PROBE */ { L"サウンドテスト画面の操作方法を調べています…（どのボタンで曲が切り替わり、再生されるか）",
                              L"Finding out how the sound test screen is operated..." },
    /* S_STATUS_ST_WALK */ { L"サウンドテストを巡回中: %d 項目め（操作: %ls）― 曲 %d、解析待ち %d",
                             L"Walking the sound test: entry %d (keys: %ls) - songs %d, waiting %d" },
    /* S_LOG_ST_START */ { L"サウンドテスト画面のステートとして読み込みます: %ls", L"Treating as a sound test screen: %ls" },
    /* S_LOG_ST_KEYS */ { L"操作方法: %ls ― %d 項目を試し、%d 項目を曲の候補として解析しました（同じ曲や短い効果音は一覧に入りません）。",
                          L"Keys: %ls - %d entries tried, %d analysed as possible songs (repeats and short sound effects are left out)." },
    /* S_LOG_ST_FAILED */ { L"%ls: サウンドテスト画面として操作できませんでした（曲を切り替えて再生するボタンが見つかりません）。通常のステートとして追加します。",
                            L"%ls: could not operate this as a sound test screen (no key found that selects and plays another tune). Adding it as an ordinary save state." },
    /* S_ORIGIN_SOUNDTEST */ { L"サウンドテスト %d 項目め", L"Sound test, entry %d" },
};

} // namespace

void set_language(int japanese)
{
    g_japanese = japanese ? 1 : 0;
}

bool is_japanese()
{
    return g_japanese != 0;
}

const wchar_t* T(StrId id)
{
    if (id < 0 || id >= S_COUNT)
        return L"";
    return g_japanese ? kText[id].ja : kText[id].en;
}
