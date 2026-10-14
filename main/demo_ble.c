// TheGreatMe BLE companion page.
//
// GATT contract (compatible with zhaohuaxiaoy/folo-ai-passport-voice):
//   Service A2B0, CTRL A2B1 (write), EVENT A2B2 (notify), MIC A2B3 (notify),
//   SPEAKER A2B4 (write). The phone writes the board/chat snapshot to CTRL.
// Holding UP opens the chat page and routes microphone audio to HQ conversation.
// Holding DOWN routes the same audio to the iPhone home-task analyzer. HQ speech
// returns over SPEAKER for local playback.
#include "demo.h"
#include "demo_radio.h"
#include "adpcm.h"
#include "passport_protocol.h"
#include "passport_host.h"
#include "passport_theme.h"
#include "bsp_audio.h"
#include "ui_pixel.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "host/ble_att.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/ble_hs_mbuf.h"
#include "host/ble_store.h"
#include "host/util/util.h"
#include "lvgl.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

// ESP-IDF's NimBLE port provides the persistent store implementation but does
// not install its callbacks automatically. The encrypted GATT characteristics
// require these callbacks so the iPhone and Passport retain the same bond.
void ble_store_config_init(void);

static const char *TAG = "thegreatme_ble";
static const char *DEVICE_NAME = "AI Passport";
#define PASSPORT_UI_VERSION "0.2"
LV_FONT_DECLARE(lv_font_terminal_zh_16);
LV_FONT_DECLARE(lv_font_terminal_extra_16);

#define CTRL_CHARACTERISTIC_UUID 0xA2B1
#define EVENT_CHARACTERISTIC_UUID 0xA2B2
#define AUDIO_CHARACTERISTIC_UUID 0xA2B3
#define SPEAKER_CHARACTERISTIC_UUID 0xA2B4
#define AUDIO_CHUNK_HEADER_BYTES 2
#define AUDIO_CHUNK_LAST 0x80
#define AUDIO_CHUNK_INDEX_MASK 0x7F
#define NOTIFY_RETRY_BUDGET_MS 50
#define VOICE_BAR_COUNT 7
#define CONNECTION_SIGNAL_BAR_COUNT 4
#define VOICE_FEEDBACK_MS 1600
#define CHAT_ANSWER_HEIGHT 120
#define CHAT_ANSWER_RECORDING_HEIGHT 78
#define SPEAKER_CONTROL_SEQUENCE 0xFF
#define SPEAKER_CONTROL_START 0xA0
#define SPEAKER_CONTROL_END 0xA1
#define SPEAKER_QUEUE_DEPTH 6
#define SPEAKER_PREFILL_BLOCKS 3
#define SPEAKER_COMPACT_SAMPLE_RATE 8000
#define SPEAKER_COMPACT_BLOCK_SAMPLES 800
#define SPEAKER_COMPACT_BLOCK_BYTES \
    (ADPCM_HEADER_BYTES + (SPEAKER_COMPACT_BLOCK_SAMPLES / 2))
#define PASSPORT_SPEAKER_VOLUME 100
#define RECORDING_CUE_AMPLITUDE 5200
#define RECORDING_CUE_CHUNK_SAMPLES 160
// A second host must declare its role (H:P/S/A) soon after connecting.
#define CANDIDATE_DECLARE_TIMEOUT_MS 5000
// A refused host gets this long to read the busy error and disconnect itself.
#define CANDIDATE_REJECT_GRACE_MS 1500
// After refusing an app that predates role lines, stop inviting takeovers
// for a while so that app does not reconnect every second.
#define LEGACY_TAKEOVER_PAUSE_MS 30000
#define THEME_NVS_NAMESPACE "passport_ui"
// Theme changes are saved after a short pause so repeated key presses or a
// color being dragged in the app do not rewrite flash each time.
#define THEME_SAVE_DELAY_US (1500 * 1000)
// Hosts that predate the D: line are always the iPhone app.
#define DEFAULT_HOST_NAME "iPhone"

typedef enum {
    BLE_PAGE_OFF = 0,
    BLE_PAGE_STARTING,
    BLE_PAGE_ADVERTISING,
    BLE_PAGE_CONNECTED,
    BLE_PAGE_FAILED,
} ble_page_state_t;

typedef enum {
    COMPANION_PAGE_DASHBOARD = 0,
    COMPANION_PAGE_CONVERSATION,
    COMPANION_PAGE_TASK_DETAIL,
    COMPANION_PAGE_SETTINGS,
} companion_page_t;

typedef enum {
    PTT_ROUTE_NONE = 0,
    PTT_ROUTE_CONVERSATION,
    PTT_ROUTE_TASK,
} ptt_route_t;

typedef struct {
    bool end;
    uint16_t length;
    uint8_t block[ADPCM_BLOCK_BYTES];
} speaker_queue_item_t;

// All four skins use the same single-color terminal structure.
typedef struct {
    const char *name;
    uint32_t background;
    uint32_t surface;
    uint32_t primary;
    uint32_t secondary;
    uint32_t accent;
    uint32_t track;
    uint32_t border;
    uint8_t glow_opacity;
} companion_theme_t;

static const companion_theme_t companion_themes[] = {
    { "琥珀", 0x050200, 0x090400, 0xFF9D00, 0xA85800, 0xFFB000, 0x2B1000, 0xFF8A00, 48 },
    { "黑色", 0x030303, 0x080808, 0xEFE7D2, 0x77736B, 0xFFFFFF, 0x222222, 0xD8D0BE, 18 },
    { "暖白", 0xF4F0E8, 0xF9F6F0, 0x181716, 0x6A655F, 0x000000, 0xDDD6CB, 0x1C1A18, 0 },
    { "数码绿", 0x000600, 0x001000, 0x3CFF6B, 0x168A35, 0xA0FFB2, 0x06260F, 0x21D94F, 44 },
};
#define COMPANION_THEME_COUNT (sizeof(companion_themes) / sizeof(companion_themes[0]))

typedef struct {
    const char *app_name;
    const char *terminal_name;
    const char *theme_names[COMPANION_THEME_COUNT];
    const char *custom_theme_name;
    const char *goal_labels[3];
    const char *goal_placeholders[3];
    const char *tasks_title;
    const char *wait_tasks;
    const char *task_nav;
    const char *task_detail_controls;
    const char *footer;
    const char *user_fallback;
    const char *hq_name;
    const char *chat_hold;
    const char *chat_hq_speaking;
    const char *chat_transcribing;
    const char *chat_hq_typing;
    const char *chat_continue;
    const char *chat_disconnected;
    const char *settings_title;
    const char *connection_title;
    const char *waiting_host;
    const char *connected_host; // printf format: %s is the host device

    const char *firmware_title;
    const char *theme_title;
    const char *settings_controls;
    const char *voice_recording;
    const char *voice_task_recording;
    const char *voice_release_send;
    const char *voice_release_task; // may use %s for the host device
    const char *voice_connecting;
    const char *voice_wait_phone;
    const char *voice_release_cancel;
    const char *voice_phone_linking;
    const char *voice_task_sent;
    const char *voice_transcribing;
    const char *voice_task_analyzing; // may use %s for the host device
    const char *voice_no_host;
    const char *voice_no_phone;
    const char *voice_enable_hint;
} companion_copy_t;

static const companion_copy_t companion_copies[] = {
    [PASSPORT_LANGUAGE_ZH_HANS] = {
        "伟大的我", "特工终端", { "琥珀", "黑色", "暖白", "数码绿" }, "自定义",
        { "本周目标", "本月目标", "年度目标" },
        { "等待本周目标", "等待本月目标", "等待年度目标" },
        "今日任务", "等待同步今日任务", "确定:主题  双击:设置",
        "上下切换  双击:设置",
        "长按上:对话  长按下:任务", "你", "总部", "长按上键说话", "总部说话中",
        "听写中", "总部输入中", "长按上键继续", "连接中断", "系统设置", "连接状态",
        "[等待 App 连接]", "● 已连接 %s", "固件版本", "主题选择",
        "上键返回    确定键切换主题", "● 录音中", "● 正在整理任务", "松开发送",
        "松开下键，交给 %s 分析", "连接中", "● 等待 App 联动", "松开取消",
        "联动中，松开可取消", "√ 已发送到任务分析", "听写中", "%s 正在拆解任务",
        "未连接 App", "！ 尚未连接 App", "请先在 App 开启护照联动",
    },
    [PASSPORT_LANGUAGE_ZH_HANT] = {
        "偉大的我", "特工終端", { "琥珀", "黑色", "暖白", "數碼綠" }, "自選",
        { "本週目標", "本月目標", "年度目標" },
        { "等待本週目標", "等待本月目標", "等待年度目標" },
        "今日任務", "等待同步今日任務", "確定:主題  雙擊:設定",
        "上下切換  雙擊:設定",
        "長按上:對話  長按下:任務", "你", "總部", "長按上鍵說話", "總部說話中",
        "聽寫中", "總部輸入中", "長按上鍵繼續", "連線中斷", "系統設定", "連線狀態",
        "[等待 App 連線]", "● 已連線 %s", "韌體版本", "主題選擇",
        "上鍵返回    確定鍵切換主題", "● 錄音中", "● 正在整理任務", "鬆開傳送",
        "鬆開下鍵，交給 %s 分析", "連線中", "● 等待 App 連動", "鬆開取消",
        "連動中，鬆開可取消", "√ 已傳送到任務分析", "聽寫中", "%s 正在拆解任務",
        "未連線 App", "！ 尚未連線 App", "請先在 App 開啟護照連動",
    },
    [PASSPORT_LANGUAGE_EN] = {
        "THE GREAT ME", "AGENT TERMINAL", { "AMBER", "BLACK", "WARM WHITE", "DIGITAL GREEN" }, "CUSTOM",
        { "WEEKLY GOAL", "MONTHLY GOAL", "ANNUAL GOAL" },
        { "Waiting for weekly goal", "Waiting for monthly goal", "Waiting for annual goal" },
        "TODAY'S TASKS", "Waiting for today's tasks", "OK: Theme  OKx2: Settings",
        "UP/DOWN: Switch  OKx2: Settings",
        "Hold UP: chat  DOWN: task", "YOU", "HQ", "Hold UP to talk", "HQ speaking",
        "Transcribing", "HQ typing", "Hold UP to continue", "Connection lost", "SYSTEM SETTINGS",
        "CONNECTION", "[WAITING FOR APP]", "● %s CONNECTED", "FIRMWARE", "THEME",
        "UP: Back    OK: Change theme", "● RECORDING", "● TASK RECORDING", "Release to send",
        "Release DOWN for %s", "Connecting", "● WAITING FOR APP", "Release to cancel",
        "Linking; release to cancel", "√ SENT FOR TASK ANALYSIS", "Transcribing",
        "%s analyzing task", "No app connected", "! APP NOT CONNECTED", "Enable AI Passport in the app",
    },
    [PASSPORT_LANGUAGE_ES] = {
        "MI GRAN YO", "TERMINAL DE AGENTE", { "ÁMBAR", "NEGRO", "BLANCO", "VERDE DIGITAL" }, "PERSONAL",
        { "OBJETIVO SEMANAL", "OBJETIVO MENSUAL", "OBJETIVO ANUAL" },
        { "Esperando objetivo semanal", "Esperando objetivo mensual", "Esperando objetivo anual" },
        "TAREAS DE HOY", "Esperando tareas de hoy", "OK: Tema  OKx2: Ajustes",
        "UP/DOWN: Cambiar  OKx2: Ajustes",
        "Mantén UP: chat  DOWN: tarea", "TÚ", "HQ", "Mantén UP para hablar", "HQ está hablando",
        "Transcribiendo", "HQ está escribiendo", "Mantén UP para seguir", "Conexión perdida",
        "AJUSTES", "CONEXIÓN", "[ESPERANDO LA APP]", "● %s CONECTADO", "FIRMWARE", "TEMA",
        "UP: Volver    OK: Cambiar tema", "● GRABANDO", "● GRABANDO TAREA", "Suelta para enviar",
        "Suelta DOWN para analizar", "Conectando", "● ESPERANDO LA APP", "Suelta para cancelar",
        "Conectando; suelta para cancelar", "√ ENVIADO PARA ANÁLISIS", "Transcribiendo",
        "%s analiza la tarea", "Sin app conectada", "! APP SIN CONEXIÓN", "Activa AI Passport en la app",
    },
    [PASSPORT_LANGUAGE_FR] = {
        "LE GRAND MOI", "TERMINAL D'AGENT", { "AMBRE", "NOIR", "BLANC CHAUD", "VERT NUMÉRIQUE" }, "PERSO",
        { "OBJECTIF SEMAINE", "OBJECTIF MENSUEL", "OBJECTIF ANNUEL" },
        { "Objectif semaine en attente", "Objectif mensuel en attente", "Objectif annuel en attente" },
        "TÂCHES DU JOUR", "Tâches du jour en attente", "OK: Thème  OKx2: Réglages",
        "UP/DOWN: Changer  OKx2: Réglages",
        "Maintenir UP: chat DOWN: tâche", "VOUS", "HQ", "Maintenir UP pour parler", "HQ parle",
        "Transcription", "HQ écrit", "Maintenir UP pour continuer", "Connexion perdue", "RÉGLAGES",
        "CONNEXION", "[APP EN ATTENTE]", "● %s CONNECTÉ", "FIRMWARE", "THÈME",
        "UP: Retour    OK: Changer thème", "● ENREGISTREMENT", "● NOTE DE TÂCHE", "Relâcher pour envoyer",
        "Relâcher DOWN pour analyser", "Connexion", "● APP EN ATTENTE", "Relâcher pour annuler",
        "Connexion; relâcher pour annuler", "√ ENVOYÉ À L'ANALYSE", "Transcription",
        "%s analyse la tâche", "Aucune app", "! APP NON CONNECTÉE", "Activez AI Passport dans l'app",
    },
    [PASSPORT_LANGUAGE_DE] = {
        "MEIN GROSSES ICH", "AGENTENTERMINAL", { "BERNSTEIN", "SCHWARZ", "WARMWEISS", "DIGITALGRÜN" }, "EIGENE",
        { "WOCHENZIEL", "MONATSZIEL", "JAHRESZIEL" },
        { "Warte auf Wochenziel", "Warte auf Monatsziel", "Warte auf Jahresziel" },
        "HEUTIGE AUFGABEN", "Warte auf heutige Aufgaben", "OK: Thema  2xOK: Einst.",
        "UP/DOWN: Wechsel  2xOK: Einst.",
        "UP halten: Chat DOWN: Aufgabe", "DU", "HQ", "UP halten zum Sprechen", "HQ spricht",
        "Transkription", "HQ schreibt", "UP halten zum Fortsetzen", "Verbindung getrennt", "EINSTELLUNGEN",
        "VERBINDUNG", "[WARTE AUF APP]", "● %s VERBUNDEN", "FIRMWARE", "THEMA",
        "UP: Zurück    OK: Thema", "● AUFNAHME", "● AUFGABE AUFNEHMEN", "Loslassen zum Senden",
        "DOWN loslassen: Analyse", "Verbinden", "● WARTE AUF APP", "Loslassen: Abbruch",
        "Verbindung; loslassen: Abbruch", "√ ZUR ANALYSE GESENDET", "Transkription",
        "%s analysiert Aufgabe", "Keine App", "! APP NICHT VERBUNDEN", "AI Passport in der App aktivieren",
    },
    [PASSPORT_LANGUAGE_KO] = {
        "위대한 나", "요원 단말기", { "호박색", "검정", "따뜻한 흰색", "디지털 그린" }, "내 색상",
        { "주간 목표", "월간 목표", "연간 목표" },
        { "주간 목표 대기 중", "월간 목표 대기 중", "연간 목표 대기 중" },
        "오늘의 작업", "오늘의 작업 동기화 대기", "OK: 테마  OKx2: 설정",
        "UP/DOWN: 전환  OKx2: 설정",
        "UP 길게: 대화 DOWN: 작업", "나", "HQ", "UP을 길게 눌러 말하기", "HQ 말하는 중",
        "받아쓰기 중", "HQ 입력 중", "UP을 길게 눌러 계속", "연결 끊김", "시스템 설정", "연결 상태",
        "[APP 대기]", "● %s 연결됨", "펌웨어 버전", "테마 선택",
        "UP: 돌아가기    OK: 테마 변경", "● 녹음 중", "● 작업 녹음 중", "놓아서 보내기",
        "DOWN을 놓아 분석", "연결 중", "● APP 대기", "놓아서 취소",
        "연결 중; 놓아서 취소", "√ 작업 분석으로 전송됨", "받아쓰기 중", "%s 작업 분석 중",
        "APP 미연결", "! APP 미연결", "APP에서 AI Passport 켜기",
    },
    [PASSPORT_LANGUAGE_JA] = {
        "偉大な私", "エージェント端末", { "アンバー", "ブラック", "ウォーム白", "デジタル緑" }, "カスタム",
        { "今週の目標", "今月の目標", "年間目標" },
        { "今週の目標を待機", "今月の目標を待機", "年間目標を待機" },
        "今日のタスク", "今日のタスクを待機", "OK: テーマ  OKx2: 設定",
        "UP/DOWN: 切替  OKx2: 設定",
        "UP長押し: 会話 DOWN: タスク", "あなた", "HQ", "UP長押しで話す", "HQが発話中",
        "文字起こし中", "HQが入力中", "UP長押しで続ける", "接続が切れました", "システム設定", "接続状態",
        "[APP待機]", "● %s接続済み", "ファームウェア", "テーマ選択",
        "UP: 戻る    OK: テーマ変更", "● 録音中", "● タスク録音中", "離して送信",
        "DOWNを離して分析", "接続中", "● APP待機", "離してキャンセル",
        "接続中; 離してキャンセル", "√ タスク分析へ送信", "文字起こし中", "%sがタスクを分析中",
        "APP未接続", "! APP未接続", "APPでAI Passportを有効化",
    },
};

static const companion_copy_t *copy_for_language(passport_language_t language)
{
    if (language < PASSPORT_LANGUAGE_ZH_HANS || language > PASSPORT_LANGUAGE_JA) {
        language = PASSPORT_LANGUAGE_ZH_HANS;
    }
    return &companion_copies[language];
}

static const ble_uuid16_t service_uuid = BLE_UUID16_INIT(0xA2B0);

static lv_obj_t *s_screen;
static lv_obj_t *s_header_box;
static lv_obj_t *s_brand_label;
static lv_obj_t *s_status_label;
static lv_obj_t *s_connection_signal_bars[CONNECTION_SIGNAL_BAR_COUNT];
static lv_obj_t *s_goal_card;
static lv_obj_t *s_goal_caption;
static lv_obj_t *s_goal_label;
static lv_obj_t *s_progress_bar;
static lv_obj_t *s_progress_label;
static lv_obj_t *s_nav_label;
static lv_obj_t *s_tasks_card;
static lv_obj_t *s_tasks_caption;
static lv_obj_t *s_task_labels[PASSPORT_BOARD_TASK_COUNT];
static lv_obj_t *s_footer_label;
static lv_obj_t *s_task_detail_card;
static lv_obj_t *s_task_detail_caption;
static lv_obj_t *s_task_detail_status;
static lv_obj_t *s_task_detail_text;
static lv_obj_t *s_task_detail_controls;
static lv_obj_t *s_voice_panel;
static lv_obj_t *s_voice_title;
static lv_obj_t *s_voice_hint;
static lv_obj_t *s_voice_bars[VOICE_BAR_COUNT];
static lv_obj_t *s_chat_card;
static lv_obj_t *s_chat_user_name_label;
static lv_obj_t *s_chat_user_label;
static lv_obj_t *s_chat_hq_name_label;
static lv_obj_t *s_chat_answer_view;
static lv_obj_t *s_chat_answer_label;
static lv_obj_t *s_chat_status_label;
static lv_obj_t *s_settings_card;
static lv_obj_t *s_settings_title_label;
static lv_obj_t *s_settings_connection_title_label;
static lv_obj_t *s_settings_theme_label;
static lv_obj_t *s_settings_ble_label;
static lv_obj_t *s_settings_firmware_title_label;
static lv_obj_t *s_settings_theme_title_label;
static lv_obj_t *s_settings_controls_label;
static lv_timer_t *s_timer;
static unsigned s_theme_index;
// A color pushed by the app becomes one extra skin after the presets.
static companion_theme_t s_custom_theme = { "自定义", 0, 0, 0, 0, 0, 0, 0, 0 };
static bool s_has_custom_theme;
static uint32_t s_custom_color;
static esp_timer_handle_t s_theme_save_timer;
static char s_host_name[PASSPORT_HOST_NAME_CAP + 1];
static unsigned s_scope_index;
static unsigned s_task_index;
static unsigned s_voice_anim_frame;
static passport_language_t s_ui_language;
static uint32_t s_voice_feedback_until;
static bool s_voice_feedback_sent;
static int8_t s_rendered_connection_signal;
static companion_page_t s_companion_page;
static companion_page_t s_settings_return_page;
static bool s_up_long_active;
static bool s_down_long_active;

static SemaphoreHandle_t s_host_stopped;
static volatile ble_page_state_t s_page_state;
static volatile int s_error;
static volatile bool s_page_active;
static volatile bool s_event_subscribed;
static volatile bool s_audio_subscribed;
static volatile bool s_ptt_active;
static volatile ptt_route_t s_ptt_route;
static volatile bool s_recording;
static volatile uint8_t s_audio_level;
static volatile bool s_conversation_open_pending;
static volatile bool s_speaker_active;
static volatile bool s_speaker_abort;
static volatile bool s_speaker_prefill_pending;
static volatile bool s_speaker_end_received;
static bool s_initialized;
static uint8_t s_address_type;
// The active host receives voice events and audio and may write the board.
static uint16_t s_connection = BLE_HS_CONN_HANDLE_NONE;
// A second link that has not yet declared whether it may take over.
static uint16_t s_candidate = BLE_HS_CONN_HANDLE_NONE;
static volatile passport_host_role_t s_active_role = PASSPORT_HOST_ROLE_UNKNOWN;
static bool s_candidate_event_subscribed;
static bool s_candidate_audio_subscribed;
static bool s_takeover_paused;
static bool s_callouts_ready;
static struct ble_npl_callout s_candidate_callout;
static struct ble_npl_callout s_takeover_callout;
static uint16_t s_ctrl_handle;
static uint16_t s_event_handle;
static uint16_t s_audio_handle;
static uint16_t s_speaker_handle;
static uint8_t s_audio_sequence;

static uint8_t s_speaker_block[ADPCM_BLOCK_BYTES];
static size_t s_speaker_block_length;
static uint8_t s_speaker_block_sequence;
static uint8_t s_speaker_expected_chunk;
static bool s_speaker_block_invalid;

static portMUX_TYPE s_board_lock = portMUX_INITIALIZER_UNLOCKED;
static passport_board_t s_board;
static volatile uint32_t s_board_revision;
static uint32_t s_rendered_revision;

static TaskHandle_t s_audio_task;
static TaskHandle_t s_speaker_task;
static QueueHandle_t s_speaker_queue;

static int gap_event(struct ble_gap_event *event, void *arg);
static void advertise(void);
static int handle_host_role(uint16_t connection, passport_host_role_t role);
static void apply_language(passport_language_t language);
static void apply_theme(void);

static unsigned theme_count(void)
{
    return COMPANION_THEME_COUNT + (s_has_custom_theme ? 1U : 0U);
}

static const companion_theme_t *current_theme(void)
{
    if (s_theme_index >= COMPANION_THEME_COUNT) {
        return s_has_custom_theme ? &s_custom_theme : &companion_themes[0];
    }
    return &companion_themes[s_theme_index];
}

static void set_custom_theme(uint32_t color)
{
    passport_palette_t palette;
    passport_palette_from_color(color, &palette);
    s_custom_theme.background = palette.background;
    s_custom_theme.surface = palette.surface;
    s_custom_theme.primary = palette.primary;
    s_custom_theme.secondary = palette.secondary;
    s_custom_theme.accent = palette.accent;
    s_custom_theme.track = palette.track;
    s_custom_theme.border = palette.border;
    s_custom_theme.glow_opacity = palette.glow_opacity;
    s_custom_color = color & 0xFFFFFFU;
    s_has_custom_theme = true;
}

static const char *host_name(void)
{
    return s_host_name[0] ? s_host_name : DEFAULT_HOST_NAME;
}

static void save_theme_preferences(void)
{
    nvs_handle_t handle;
    if (nvs_open(THEME_NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) {
        ESP_LOGW(TAG, "主题设置无法保存");
        return;
    }
    nvs_set_u8(handle, "theme_idx", (uint8_t)s_theme_index);
    if (s_has_custom_theme) {
        nvs_set_u32(handle, "theme_rgb", s_custom_color);
    } else {
        nvs_erase_key(handle, "theme_rgb");
    }
    nvs_commit(handle);
    nvs_close(handle);
}

static void theme_save_timer_fired(void *argument)
{
    (void)argument;
    save_theme_preferences();
}

// Runs from the LVGL task (keys and the board tick), so the flash write is
// deferred to the esp_timer task instead of blocking input handling.
static void schedule_theme_save(void)
{
    if (!s_theme_save_timer) {
        const esp_timer_create_args_t args = {
            .callback = theme_save_timer_fired,
            .name = "passport_theme_save",
        };
        if (esp_timer_create(&args, &s_theme_save_timer) != ESP_OK) return;
    }
    esp_timer_stop(s_theme_save_timer);
    esp_timer_start_once(s_theme_save_timer, THEME_SAVE_DELAY_US);
}

static void load_theme_preferences(void)
{
    if (demo_radio_nvs_prepare() != ESP_OK) return;
    nvs_handle_t handle;
    if (nvs_open(THEME_NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) return;
    uint32_t color;
    if (nvs_get_u32(handle, "theme_rgb", &color) == ESP_OK) set_custom_theme(color);
    uint8_t index;
    if (nvs_get_u8(handle, "theme_idx", &index) == ESP_OK && index < theme_count()) {
        s_theme_index = index;
    }
    nvs_close(handle);
}

static void board_store(const passport_board_t *board)
{
    portENTER_CRITICAL(&s_board_lock);
    s_board = *board;
    s_board_revision++;
    portEXIT_CRITICAL(&s_board_lock);
}

static passport_board_t board_load(uint32_t *revision)
{
    passport_board_t board;
    portENTER_CRITICAL(&s_board_lock);
    board = s_board;
    if (revision) *revision = s_board_revision;
    portEXIT_CRITICAL(&s_board_lock);
    return board;
}

static int gatt_access(
    uint16_t connection,
    uint16_t attribute,
    struct ble_gatt_access_ctxt *context,
    void *argument
)
{
    (void)attribute;
    (void)argument;
    if (context->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return BLE_ATT_ERR_UNLIKELY;

    uint16_t length = OS_MBUF_PKTLEN(context->om);
    if (length == 0 || length > PASSPORT_BOARD_PAYLOAD_CAP) {
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }

    uint8_t payload[PASSPORT_BOARD_PAYLOAD_CAP];
    if (os_mbuf_copydata(context->om, 0, length, payload) != 0) {
        return BLE_ATT_ERR_UNLIKELY;
    }

    passport_host_role_t role;
    if (passport_host_parse_role(payload, length, &role)) {
        return handle_host_role(connection, role);
    }
    // Only the active host may change what the Passport shows.
    if (connection != s_connection) return BLE_ATT_ERR_WRITE_NOT_PERMITTED;

    passport_board_t next = board_load(NULL);
    if (!passport_board_apply(&next, payload, length)) {
        ESP_LOGW(TAG, "拒绝无效看板载荷(%u B)", (unsigned)length);
        return BLE_ATT_ERR_WRITE_NOT_PERMITTED;
    }
    board_store(&next);
    return 0;
}

static void reset_speaker_reassembly(void)
{
    s_speaker_block_length = 0;
    s_speaker_block_sequence = 0;
    s_speaker_expected_chunk = 0;
    s_speaker_block_invalid = false;
}

static void abort_speaker_playback(void)
{
    s_speaker_abort = true;
    s_speaker_active = false;
    s_speaker_prefill_pending = false;
    s_speaker_end_received = false;
    if (s_speaker_queue) xQueueReset(s_speaker_queue);
    reset_speaker_reassembly();
}

static void complete_speaker_playback(void)
{
    s_speaker_active = false;
    s_speaker_abort = false;
    s_speaker_prefill_pending = false;
    s_speaker_end_received = false;
    ESP_LOGI(TAG, "总部语音播放完成");
}

static int speaker_access(
    uint16_t connection,
    uint16_t attribute,
    struct ble_gatt_access_ctxt *context,
    void *argument
)
{
    (void)attribute;
    (void)argument;
    if (context->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return BLE_ATT_ERR_UNLIKELY;
    if (connection != s_connection) return BLE_ATT_ERR_WRITE_NOT_PERMITTED;

    uint16_t length = OS_MBUF_PKTLEN(context->om);
    if (length < AUDIO_CHUNK_HEADER_BYTES || length > 252) {
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }
    uint8_t packet[252];
    if (os_mbuf_copydata(context->om, 0, length, packet) != 0) {
        return BLE_ATT_ERR_UNLIKELY;
    }

    if (packet[0] == SPEAKER_CONTROL_SEQUENCE) {
        if (packet[1] == SPEAKER_CONTROL_START) {
            if (s_speaker_queue) xQueueReset(s_speaker_queue);
            reset_speaker_reassembly();
            s_speaker_abort = false;
            s_speaker_active = true;
            s_speaker_prefill_pending = true;
            s_speaker_end_received = false;
            return 0;
        }
        if (packet[1] == SPEAKER_CONTROL_END) {
            s_speaker_end_received = true;
            speaker_queue_item_t item = { .end = true };
            if (!s_speaker_queue || xQueueSend(s_speaker_queue, &item, 0) != pdTRUE) {
                ESP_LOGW(TAG, "总部语音结束标记队列已满,等待播放任务收尾");
            }
            return 0;
        }
        return BLE_ATT_ERR_WRITE_NOT_PERMITTED;
    }

    uint8_t sequence = packet[0];
    uint8_t marker = packet[1];
    uint8_t chunk_index = marker & AUDIO_CHUNK_INDEX_MASK;
    bool is_last = (marker & AUDIO_CHUNK_LAST) != 0;
    if (s_speaker_block_length == 0 || sequence != s_speaker_block_sequence) {
        s_speaker_block_sequence = sequence;
        s_speaker_expected_chunk = 0;
        s_speaker_block_invalid = chunk_index != 0;
        s_speaker_block_length = 0;
    }
    if (chunk_index != s_speaker_expected_chunk) s_speaker_block_invalid = true;
    size_t body_length = length - AUDIO_CHUNK_HEADER_BYTES;
    if (s_speaker_block_length + body_length > sizeof(s_speaker_block)) {
        s_speaker_block_invalid = true;
    } else if (!s_speaker_block_invalid) {
        memcpy(
            s_speaker_block + s_speaker_block_length,
            packet + AUDIO_CHUNK_HEADER_BYTES,
            body_length
        );
        s_speaker_block_length += body_length;
    }
    s_speaker_expected_chunk++;

    if (is_last) {
        bool supported_length = s_speaker_block_length == ADPCM_BLOCK_BYTES
                             || s_speaker_block_length == SPEAKER_COMPACT_BLOCK_BYTES;
        if (!s_speaker_block_invalid && supported_length) {
            speaker_queue_item_t item = {
                .end = false,
                .length = (uint16_t)s_speaker_block_length,
            };
            memcpy(item.block, s_speaker_block, s_speaker_block_length);
            if (!s_speaker_queue || xQueueSend(s_speaker_queue, &item, 0) != pdTRUE) {
                ESP_LOGW(TAG, "总部语音队列已满,丢弃音频块");
                reset_speaker_reassembly();
                return BLE_ATT_ERR_INSUFFICIENT_RES;
            }
        }
        reset_speaker_reassembly();
    }
    return 0;
}

static const struct ble_gatt_svc_def services[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &service_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = BLE_UUID16_DECLARE(CTRL_CHARACTERISTIC_UUID),
                .access_cb = gatt_access,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP
                       | BLE_GATT_CHR_F_WRITE_ENC,
                .val_handle = &s_ctrl_handle,
            },
            {
                .uuid = BLE_UUID16_DECLARE(EVENT_CHARACTERISTIC_UUID),
                .access_cb = gatt_access,
                .flags = BLE_GATT_CHR_F_NOTIFY | BLE_GATT_CHR_F_NOTIFY_INDICATE_ENC,
                .val_handle = &s_event_handle,
            },
            {
                .uuid = BLE_UUID16_DECLARE(AUDIO_CHARACTERISTIC_UUID),
                .access_cb = gatt_access,
                .flags = BLE_GATT_CHR_F_NOTIFY | BLE_GATT_CHR_F_NOTIFY_INDICATE_ENC,
                .val_handle = &s_audio_handle,
            },
            {
                .uuid = BLE_UUID16_DECLARE(SPEAKER_CHARACTERISTIC_UUID),
                .access_cb = speaker_access,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP
                       | BLE_GATT_CHR_F_WRITE_ENC,
                .val_handle = &s_speaker_handle,
            },
            { 0 },
        },
    },
    { 0 },
};

static bool notify_one(uint16_t handle, const void *data, size_t length)
{
    if (s_connection == BLE_HS_CONN_HANDLE_NONE) return false;
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(NOTIFY_RETRY_BUDGET_MS);
    for (;;) {
        struct os_mbuf *buffer = ble_hs_mbuf_from_flat(data, length);
        if (buffer) {
            int result = ble_gatts_notify_custom(s_connection, handle, buffer);
            if (result == 0) return true;
            if (result != BLE_HS_ENOMEM) return false;
        }
        if ((int32_t)(xTaskGetTickCount() - deadline) >= 0) return false;
        vTaskDelay(pdMS_TO_TICKS(2));
    }
}

static bool notify_event(const char *line)
{
    if (!line || !s_event_subscribed) return false;
    size_t length = strlen(line);
    uint16_t mtu = ble_att_mtu(s_connection);
    if (mtu < 23) mtu = 23;
    size_t chunk_size = (size_t)mtu - 3;
    for (size_t offset = 0; offset < length; offset += chunk_size) {
        size_t chunk_length = length - offset;
        if (chunk_length > chunk_size) chunk_length = chunk_size;
        if (!notify_one(s_event_handle, line + offset, chunk_length)) return false;
    }
    return true;
}

static bool notify_audio(const uint8_t *block, size_t length)
{
    if (!block || length == 0 || !s_audio_subscribed) return false;
    uint16_t mtu = ble_att_mtu(s_connection);
    if (mtu < 23) mtu = 23;
    size_t body_capacity = (size_t)mtu - 3 - AUDIO_CHUNK_HEADER_BYTES;
    if (body_capacity > 250) body_capacity = 250;
    if (body_capacity == 0) return false;

    uint8_t packet[252];
    uint8_t sequence = s_audio_sequence++;
    size_t offset = 0;
    unsigned index = 0;
    bool complete = true;
    while (offset < length) {
        size_t body_length = length - offset;
        if (body_length > body_capacity) body_length = body_capacity;
        bool last = offset + body_length >= length;
        packet[0] = sequence;
        packet[1] = (uint8_t)((index & AUDIO_CHUNK_INDEX_MASK) | (last ? AUDIO_CHUNK_LAST : 0));
        memcpy(packet + AUDIO_CHUNK_HEADER_BYTES, block + offset, body_length);
        if (!notify_one(s_audio_handle, packet, body_length + AUDIO_CHUNK_HEADER_BYTES)) {
            complete = false;
        }
        offset += body_length;
        index++;
    }
    return complete;
}

static bool write_recording_cue_segment(
    int16_t *pcm,
    size_t capacity,
    unsigned frequency_hz,
    unsigned duration_ms
)
{
    if (!pcm || capacity == 0 || duration_ms == 0) return false;
    size_t remaining = 16000U * duration_ms / 1000U;
    unsigned period = frequency_hz ? 16000U / frequency_hz : 0;
    unsigned phase = 0;

    while (remaining > 0) {
        size_t count = remaining < capacity ? remaining : capacity;
        if (period == 0) {
            memset(pcm, 0, count * sizeof(*pcm));
        } else {
            for (size_t i = 0; i < count; i++) {
                pcm[i] = phase < period / 2U
                    ? RECORDING_CUE_AMPLITUDE
                    : -RECORDING_CUE_AMPLITUDE;
                phase++;
                if (phase >= period) phase = 0;
            }
        }
        if (bsp_audio_write(pcm, count * sizeof(*pcm)) != ESP_OK) return false;
        remaining -= count;
    }
    return true;
}

static void play_recording_start_cue(int16_t *pcm, size_t capacity)
{
    bsp_audio_set_volume(PASSPORT_SPEAKER_VOLUME);
    bool played = write_recording_cue_segment(pcm, capacity, 1000, 45)
               && write_recording_cue_segment(pcm, capacity, 0, 20)
               && write_recording_cue_segment(pcm, capacity, 1600, 55)
               && write_recording_cue_segment(pcm, capacity, 0, 40);
    if (!played) ESP_LOGW(TAG, "录音启动提示音播放失败");
}

static void audio_task(void *argument)
{
    (void)argument;
    static int16_t pcm[ADPCM_BLOCK_SAMPLES];
    static uint8_t encoded[ADPCM_BLOCK_BYTES];
    adpcm_state_t adpcm;

    for (;;) {
        bool ready = s_page_active && s_ptt_active && !s_speaker_active
                  && s_event_subscribed && s_audio_subscribed;
        if (!ready) {
            if (s_recording) {
                s_recording = false;
                s_audio_level = 0;
                notify_event("{\"event\":\"voice.end\"}\n");
                ESP_LOGI(TAG, "PTT 松开,音频结束");
            }
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        if (!s_recording) {
            if (bsp_audio_set_format(16000, 16, 1) != ESP_OK) {
                ESP_LOGE(TAG, "麦克风 16kHz 配置失败");
                s_ptt_active = false;
                continue;
            }
            play_recording_start_cue(pcm, RECORDING_CUE_CHUNK_SAMPLES);
            if (!s_page_active || !s_ptt_active
                || !s_event_subscribed || !s_audio_subscribed) {
                continue;
            }
            adpcm_state_reset(&adpcm);
            s_audio_sequence = 0;
            if (s_conversation_open_pending) {
                notify_event("{\"event\":\"conversation.open\"}\n");
                s_conversation_open_pending = false;
            }
            const char *route = s_ptt_route == PTT_ROUTE_TASK ? "task" : "conversation";
            char event[96];
            snprintf(
                event,
                sizeof(event),
                "{\"event\":\"voice.start\",\"route\":\"%s\",\"audio\":\"ima_adpcm\"}\n",
                route
            );
            if (!notify_event(event)) {
                s_ptt_active = false;
                continue;
            }
            s_recording = true;
            s_audio_level = 8;
            ESP_LOGI(TAG, "PTT 按下,开始 16kHz ADPCM 音频,路由=%s", route);
        }

        if (bsp_audio_read(pcm, sizeof(pcm)) != ESP_OK) {
            ESP_LOGE(TAG, "麦克风读取失败");
            s_ptt_active = false;
            continue;
        }
        int32_t peak = 0;
        for (size_t i = 0; i < ADPCM_BLOCK_SAMPLES; i++) {
            int32_t sample = pcm[i];
            if (sample < 0) sample = -sample;
            if (sample > peak) peak = sample;
        }
        uint32_t level = (uint32_t)peak * 100U / 32767U;
        if (level > 100U) level = 100U;
        s_audio_level = (uint8_t)level;
        size_t encoded_length = adpcm_encode_block(
            &adpcm,
            pcm,
            ADPCM_BLOCK_SAMPLES,
            encoded,
            sizeof(encoded)
        );
        if (encoded_length != ADPCM_BLOCK_BYTES || !notify_audio(encoded, encoded_length)) {
            ESP_LOGW(TAG, "BLE 音频块发送不完整");
        }
    }
}

static void speaker_task(void *argument)
{
    (void)argument;
    static int16_t pcm[ADPCM_BLOCK_SAMPLES];
    speaker_queue_item_t item;

    for (;;) {
        if (s_speaker_prefill_pending && s_speaker_queue) {
            while (s_speaker_prefill_pending
                   && !s_speaker_abort
                   && s_page_active
                   && s_speaker_active
                   && !s_speaker_end_received
                   && uxQueueMessagesWaiting(s_speaker_queue) < SPEAKER_PREFILL_BLOCKS) {
                vTaskDelay(pdMS_TO_TICKS(5));
            }
            s_speaker_prefill_pending = false;
        }

        if (!s_speaker_queue
            || xQueueReceive(s_speaker_queue, &item, pdMS_TO_TICKS(20)) != pdTRUE) {
            if (s_speaker_active && s_speaker_end_received) {
                complete_speaker_playback();
            }
            continue;
        }
        if (item.end) {
            complete_speaker_playback();
            continue;
        }
        if (s_speaker_abort || !s_page_active) continue;

        uint32_t sample_rate = 16000;
        size_t samples = ADPCM_BLOCK_SAMPLES;
        if (item.length == SPEAKER_COMPACT_BLOCK_BYTES) {
            sample_rate = SPEAKER_COMPACT_SAMPLE_RATE;
            samples = SPEAKER_COMPACT_BLOCK_SAMPLES;
        } else if (item.length != ADPCM_BLOCK_BYTES) {
            ESP_LOGW(TAG, "总部语音块长度无效:%u", (unsigned)item.length);
            continue;
        }

        if (bsp_audio_set_format(sample_rate, 16, 1) != ESP_OK) {
            ESP_LOGE(TAG, "扬声器 %lukHz 配置失败", (unsigned long)(sample_rate / 1000));
            abort_speaker_playback();
            continue;
        }
        size_t decoded = adpcm_decode_block(
            item.block,
            item.length,
            samples,
            pcm,
            ADPCM_BLOCK_SAMPLES
        );
        if (decoded != samples) {
            ESP_LOGW(TAG, "总部语音块解码失败");
            continue;
        }
        bsp_audio_set_volume(PASSPORT_SPEAKER_VOLUME);
        if (bsp_audio_write(pcm, samples * sizeof(*pcm)) != ESP_OK) {
            ESP_LOGW(TAG, "总部语音块播放失败");
        }
    }
}

static void start_advertising(bool takeover)
{
    // Restarting replaces the interval when switching between plain and
    // takeover advertising; starting twice would fail with EALREADY.
    if (ble_gap_adv_active()) ble_gap_adv_stop();

    struct ble_hs_adv_fields advertisement = { 0 };
    advertisement.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    // CoreBluetooth service-filtered scans match the primary advertising
    // packet. Keep A2B0 here and move the human-readable name to scan response.
    advertisement.uuids16 = (ble_uuid16_t *)&service_uuid;
    advertisement.num_uuids16 = 1;
    advertisement.uuids16_is_complete = 1;
    int result = ble_gap_adv_set_fields(&advertisement);
    if (result != 0) goto fail;

    struct ble_hs_adv_fields response = { 0 };
    response.name = (const uint8_t *)DEVICE_NAME;
    response.name_len = strlen(DEVICE_NAME);
    response.name_is_complete = 1;
    result = ble_gap_adv_rsp_set_fields(&response);
    if (result != 0) goto fail;

    struct ble_gap_adv_params parameters = { 0 };
    parameters.conn_mode = BLE_GAP_CONN_MODE_UND;
    parameters.disc_mode = BLE_GAP_DISC_MODE_GEN;
    // While a host is connected the advertising only has to be found by the
    // preferred host's background scan, so it runs at a lower duty cycle.
    parameters.itvl_min = BLE_GAP_ADV_ITVL_MS(takeover ? 400 : 100);
    parameters.itvl_max = BLE_GAP_ADV_ITVL_MS(takeover ? 500 : 150);
    result = ble_gap_adv_start(
        s_address_type,
        NULL,
        BLE_HS_FOREVER,
        &parameters,
        gap_event,
        NULL
    );
    if (result != 0) goto fail;
    if (!takeover) s_page_state = BLE_PAGE_ADVERTISING;
    return;

fail:
    if (takeover) {
        ESP_LOGW(TAG, "接管广播启动失败:%d", result);
        return;
    }
    s_error = result;
    s_page_state = BLE_PAGE_FAILED;
}

static void advertise(void)
{
    start_advertising(false);
}

// Keep advertising while a secondary host holds the Passport, so the user's
// preferred host can still find it and take over; otherwise stay silent.
static void update_takeover_advertising(void)
{
    if (!s_page_active || s_connection == BLE_HS_CONN_HANDLE_NONE) return;
    if (s_candidate != BLE_HS_CONN_HANDLE_NONE) return;
    bool wanted = !s_takeover_paused && passport_host_accepts_takeover(s_active_role);
    if (wanted && !ble_gap_adv_active()) {
        start_advertising(true);
    } else if (!wanted && ble_gap_adv_active()) {
        ble_gap_adv_stop();
    }
}

// A new active host names itself with D:; until then show the default, not
// the previous host's name.
static void forget_host_name(void)
{
    passport_board_t board = board_load(NULL);
    if (!board.host_name[0]) return;
    board.host_name[0] = '\0';
    board_store(&board);
}

static void configure_link(uint16_t connection)
{
    ble_gap_set_prefered_le_phy(
        connection,
        BLE_GAP_LE_PHY_2M_MASK,
        BLE_GAP_LE_PHY_2M_MASK,
        0
    );
    const struct ble_gap_upd_params parameters = {
        .itvl_min = 12,
        .itvl_max = 24,
        .latency = 0,
        .supervision_timeout = 400,
        .min_ce_len = 0,
        .max_ce_len = 0,
    };
    ble_gap_update_params(connection, &parameters);
    ble_gattc_exchange_mtu(connection, NULL, NULL);
}

static void announce_capabilities(void)
{
    if (!s_event_subscribed || !s_audio_subscribed) return;
    char capability[96];
    snprintf(
        capability,
        sizeof(capability),
        "{\"event\":\"speaker.config\",\"sample_rate\":%u,\"block_samples\":%u}\n",
        (unsigned)SPEAKER_COMPACT_SAMPLE_RATE,
        (unsigned)SPEAKER_COMPACT_BLOCK_SAMPLES
    );
    if (!notify_event(capability)) {
        ESP_LOGW(TAG, "扬声器压缩能力通知失败,手机将回退 16kHz");
    }
    // Apps only send the D: and K: lines after seeing this, because older
    // firmware rejects unknown board lines.
    notify_event("{\"event\":\"features\",\"host_name\":true,\"theme_color\":true}\n");
}

static void schedule_candidate_timeout(uint32_t milliseconds)
{
    if (!s_callouts_ready) return;
    ble_npl_callout_reset(&s_candidate_callout, ble_npl_time_ms_to_ticks32(milliseconds));
}

static void candidate_timeout(struct ble_npl_event *event)
{
    (void)event;
    if (s_candidate == BLE_HS_CONN_HANDLE_NONE) return;
    ESP_LOGW(TAG, "第二台主机未让出,断开(handle=%u)", s_candidate);
    ble_gap_terminate(s_candidate, BLE_ERR_REM_USER_CONN_TERM);
}

static void takeover_resume(struct ble_npl_event *event)
{
    (void)event;
    s_takeover_paused = false;
    update_takeover_advertising();
}

// An app that predates role lines subscribed without declaring itself; it
// would grab the Passport the old way, so refuse it and pause the invitation.
static void drop_legacy_candidate(void)
{
    uint16_t candidate = s_candidate;
    ESP_LOGW(TAG, "第二台主机未声明优先级(旧版 App),断开(handle=%u)", candidate);
    s_takeover_paused = true;
    if (s_callouts_ready) {
        ble_npl_callout_reset(
            &s_takeover_callout,
            ble_npl_time_ms_to_ticks32(LEGACY_TAKEOVER_PAUSE_MS)
        );
    }
    ble_gap_terminate(candidate, BLE_ERR_REM_USER_CONN_TERM);
}

static void promote_candidate(passport_host_role_t role)
{
    uint16_t previous = s_connection;
    // Tell the previous host why it is losing the link, so its app treats the
    // drop as a handoff rather than a failure.
    notify_event("{\"event\":\"host.yield\"}\n");
    s_ptt_active = false;
    abort_speaker_playback();

    s_connection = s_candidate;
    s_candidate = BLE_HS_CONN_HANDLE_NONE;
    s_active_role = role;
    forget_host_name();
    s_event_subscribed = s_candidate_event_subscribed;
    s_audio_subscribed = s_candidate_audio_subscribed;
    s_candidate_event_subscribed = false;
    s_candidate_audio_subscribed = false;
    if (s_callouts_ready) ble_npl_callout_stop(&s_candidate_callout);
    s_page_state = BLE_PAGE_CONNECTED;
    ESP_LOGI(TAG, "优先主机接管(handle=%u),断开原主机(handle=%u)", s_connection, previous);

    if (previous != BLE_HS_CONN_HANDLE_NONE) {
        ble_gap_terminate(previous, BLE_ERR_REM_USER_CONN_TERM);
    }
    announce_capabilities();
    update_takeover_advertising();
}

static int handle_host_role(uint16_t connection, passport_host_role_t role)
{
    if (connection == s_connection) {
        s_active_role = role;
        ESP_LOGI(TAG, "当前主机声明优先级:%d", (int)role);
        update_takeover_advertising();
        return 0;
    }
    if (connection != s_candidate) return PASSPORT_HOST_BUSY_ATT_ERROR;
    if (passport_host_arbitrate(s_active_role, role) == PASSPORT_HOST_PROMOTE_CANDIDATE) {
        promote_candidate(role);
        return 0;
    }
    // Refuse with a dedicated error the app recognizes, then disconnect it if
    // it does not leave on its own.
    ESP_LOGI(TAG, "第二台主机无权接管(角色=%d),拒绝", (int)role);
    schedule_candidate_timeout(CANDIDATE_REJECT_GRACE_MS);
    return PASSPORT_HOST_BUSY_ATT_ERROR;
}

static int gap_event(struct ble_gap_event *event, void *argument)
{
    (void)argument;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status != 0) {
            if (s_connection == BLE_HS_CONN_HANDLE_NONE) {
                advertise();
            } else {
                update_takeover_advertising();
            }
            break;
        }
        configure_link(event->connect.conn_handle);
        if (s_connection == BLE_HS_CONN_HANDLE_NONE) {
            s_connection = event->connect.conn_handle;
            s_active_role = PASSPORT_HOST_ROLE_UNKNOWN;
            forget_host_name();
            s_page_state = BLE_PAGE_CONNECTED;
            ESP_LOGI(TAG, "主机已连接(handle=%u)", s_connection);
        } else if (s_candidate == BLE_HS_CONN_HANDLE_NONE) {
            s_candidate = event->connect.conn_handle;
            s_candidate_event_subscribed = false;
            s_candidate_audio_subscribed = false;
            schedule_candidate_timeout(CANDIDATE_DECLARE_TIMEOUT_MS);
            ESP_LOGI(TAG, "第二台主机接入,等待声明优先级(handle=%u)", s_candidate);
        } else {
            ble_gap_terminate(event->connect.conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        }
        break;

    case BLE_GAP_EVENT_DISCONNECT: {
        uint16_t handle = event->disconnect.conn.conn_handle;
        ESP_LOGI(TAG, "连接断开(handle=%u reason=%d)", handle, event->disconnect.reason);
        if (handle == s_candidate) {
            s_candidate = BLE_HS_CONN_HANDLE_NONE;
            s_candidate_event_subscribed = false;
            s_candidate_audio_subscribed = false;
            if (s_callouts_ready) ble_npl_callout_stop(&s_candidate_callout);
            update_takeover_advertising();
            break;
        }
        // A host replaced by the preferred one: already handed over.
        if (handle != s_connection) break;

        s_ptt_active = false;
        abort_speaker_playback();
        s_event_subscribed = false;
        s_audio_subscribed = false;
        s_connection = BLE_HS_CONN_HANDLE_NONE;
        s_active_role = PASSPORT_HOST_ROLE_UNKNOWN;
        if (s_candidate != BLE_HS_CONN_HANDLE_NONE) {
            // The waiting host becomes the active one; it still declares its
            // role, which is then accepted as the active host's own.
            s_connection = s_candidate;
            s_candidate = BLE_HS_CONN_HANDLE_NONE;
            forget_host_name();
            s_event_subscribed = s_candidate_event_subscribed;
            s_audio_subscribed = s_candidate_audio_subscribed;
            s_candidate_event_subscribed = false;
            s_candidate_audio_subscribed = false;
            if (s_callouts_ready) ble_npl_callout_stop(&s_candidate_callout);
            s_page_state = BLE_PAGE_CONNECTED;
            announce_capabilities();
        } else if (s_page_active) {
            advertise();
        }
        break;
    }

    case BLE_GAP_EVENT_SUBSCRIBE: {
        uint16_t handle = event->subscribe.conn_handle;
        bool is_event = event->subscribe.attr_handle == s_event_handle;
        bool is_audio = event->subscribe.attr_handle == s_audio_handle;
        if (handle == s_candidate) {
            if (event->subscribe.reason == BLE_GAP_SUBSCRIBE_REASON_WRITE
                && event->subscribe.cur_notify) {
                drop_legacy_candidate();
                break;
            }
            // Bonded hosts get their subscriptions restored on encryption;
            // keep them in case this host is promoted.
            if (is_event) s_candidate_event_subscribed = event->subscribe.cur_notify;
            if (is_audio) s_candidate_audio_subscribed = event->subscribe.cur_notify;
            break;
        }
        if (handle != s_connection) break;
        if (is_event) {
            s_event_subscribed = event->subscribe.cur_notify;
        } else if (is_audio) {
            s_audio_subscribed = event->subscribe.cur_notify;
        }
        ESP_LOGI(
            TAG,
            "订阅状态:event=%d audio=%d mtu=%u",
            s_event_subscribed,
            s_audio_subscribed,
            ble_att_mtu(s_connection)
        );
        announce_capabilities();
        break;
    }

    case BLE_GAP_EVENT_REPEAT_PAIRING: {
        // The iPhone can forget its system bond while the Passport still has
        // the old keys in NVS. Remove only that peer's stale bond and continue
        // this pairing attempt instead of entering a connect/fail retry loop.
        struct ble_gap_conn_desc descriptor;
        int result = ble_gap_conn_find(event->repeat_pairing.conn_handle, &descriptor);
        if (result != 0) {
            ESP_LOGE(TAG, "无法读取重复配对连接:%d", result);
            return BLE_GAP_REPEAT_PAIRING_IGNORE;
        }
        result = ble_store_util_delete_peer(&descriptor.peer_id_addr);
        if (result != 0) {
            ESP_LOGE(TAG, "无法清除失效配对:%d", result);
            return BLE_GAP_REPEAT_PAIRING_IGNORE;
        }
        ESP_LOGW(TAG, "主机配对已失效,已清除旧密钥并重新配对");
        return BLE_GAP_REPEAT_PAIRING_RETRY;
    }

    default:
        break;
    }
    return 0;
}

static void on_reset(int reason)
{
    s_error = reason;
    s_page_state = BLE_PAGE_FAILED;
}

static void on_sync(void)
{
    int result = ble_hs_util_ensure_addr(0);
    if (result == 0) result = ble_hs_id_infer_auto(0, &s_address_type);
    if (result == 0 && s_page_active) advertise();
    if (result != 0) {
        s_error = result;
        s_page_state = BLE_PAGE_FAILED;
    }
}

static void host_task(void *argument)
{
    (void)argument;
    nimble_port_run();
    if (s_host_stopped) xSemaphoreGive(s_host_stopped);
    nimble_port_freertos_deinit();
}

static esp_err_t ble_start(void)
{
    if (s_initialized) return ESP_ERR_INVALID_STATE;
    s_page_state = BLE_PAGE_STARTING;
    esp_err_t error = demo_radio_nvs_prepare();
    if (error != ESP_OK) return error;
    error = nimble_port_init();
    if (error != ESP_OK) return error;

    s_initialized = true;
    // Callouts run on the host task's event queue, alongside GAP and GATT
    // callbacks, so the arbitration state needs no extra locking.
    ble_npl_callout_init(&s_candidate_callout, nimble_port_get_dflt_eventq(), candidate_timeout, NULL);
    ble_npl_callout_init(&s_takeover_callout, nimble_port_get_dflt_eventq(), takeover_resume, NULL);
    s_callouts_ready = true;
    s_host_stopped = xSemaphoreCreateBinary();
    if (!s_host_stopped) return ESP_ERR_NO_MEM;

    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    ble_hs_cfg.sm_io_cap = BLE_HS_IO_NO_INPUT_OUTPUT;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_sc = 1;
    // iOS reconnects from a private address that changes after Bluetooth is
    // toggled and roughly every 15 minutes. Exchanging identity keys lets the
    // controller resolve it to the bonded iPhone; without them the Passport
    // sees a new peer, the bond lookup fails, and iOS asks to pair again.
    ble_hs_cfg.sm_our_key_dist |= BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist |= BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_svc_gap_device_name_set(DEVICE_NAME);
    int result = ble_gatts_count_cfg(services);
    if (result == 0) result = ble_gatts_add_svcs(services);
    if (result != 0) {
        s_error = result;
        s_page_state = BLE_PAGE_FAILED;
        return ESP_FAIL;
    }
    ble_store_config_init();
    nimble_port_freertos_init(host_task);
    return ESP_OK;
}

static void ble_stop(void)
{
    if (!s_initialized) return;
    s_ptt_active = false;
    abort_speaker_playback();
    ble_gap_adv_stop();
    if (s_callouts_ready) {
        ble_npl_callout_stop(&s_candidate_callout);
        ble_npl_callout_stop(&s_takeover_callout);
    }
    int result = nimble_port_stop();
    if (result == 0 && s_host_stopped) {
        xSemaphoreTake(s_host_stopped, portMAX_DELAY);
    }
    if (result == 0) {
        if (s_callouts_ready) {
            ble_npl_callout_deinit(&s_candidate_callout);
            ble_npl_callout_deinit(&s_takeover_callout);
            s_callouts_ready = false;
        }
        nimble_port_deinit();
        s_initialized = false;
    } else {
        ESP_LOGE(TAG, "NimBLE 停止失败:%d", result);
    }
    if (!s_initialized && s_host_stopped) {
        vSemaphoreDelete(s_host_stopped);
        s_host_stopped = NULL;
    }
    s_event_subscribed = false;
    s_audio_subscribed = false;
    s_connection = BLE_HS_CONN_HANDLE_NONE;
    s_candidate = BLE_HS_CONN_HANDLE_NONE;
    s_active_role = PASSPORT_HOST_ROLE_UNKNOWN;
    s_candidate_event_subscribed = false;
    s_candidate_audio_subscribed = false;
    s_takeover_paused = false;
    s_page_state = BLE_PAGE_OFF;
}

static void render_board(const passport_board_t *board)
{
    const companion_copy_t *copy = copy_for_language(board->language);
    const char *goals[] = { board->week_goal, board->month_goal, board->year_goal };
    const companion_theme_t *theme = current_theme();

    lv_label_set_text_fmt(s_goal_caption, "%s %u/3", copy->goal_labels[s_scope_index], s_scope_index + 1);
    lv_label_set_text(
        s_goal_label,
        goals[s_scope_index][0] ? goals[s_scope_index] : copy->goal_placeholders[s_scope_index]
    );
    lv_bar_set_value(s_progress_bar, board->progress, LV_ANIM_ON);
    lv_label_set_text_fmt(s_progress_label, "%u%%", board->progress);

    unsigned completed = 0;
    for (uint8_t i = 0; i < board->task_count; i++) {
        if (board->tasks[i].status == PASSPORT_TASK_DONE) completed++;
    }
    if (board->task_count == 0) {
        s_task_index = 0;
    } else if (s_task_index >= board->task_count) {
        s_task_index = board->task_count - 1;
    }
    lv_label_set_text_fmt(s_tasks_caption, "%s %u/%u", copy->tasks_title, completed, board->task_count);

    for (uint8_t i = 0; i < PASSPORT_BOARD_TASK_COUNT; i++) {
        if (i >= board->task_count || !board->tasks[i].text[0]) {
            lv_label_set_text(s_task_labels[i], "");
            continue;
        }
        const char *marker = "□";
        uint32_t color = theme->primary;
        if (board->tasks[i].status == PASSPORT_TASK_DONE) {
            marker = "√";
            color = theme->secondary;
        } else if (board->tasks[i].status == PASSPORT_TASK_BLOCKED) {
            marker = "！";
            color = theme->accent;
        }
        bool selected = i == s_task_index;
        lv_label_set_text_fmt(
            s_task_labels[i],
            "%s%s %s",
            selected ? ">" : " ",
            marker,
            board->tasks[i].text
        );
        if (selected) color = theme->accent;
        lv_obj_set_style_text_color(s_task_labels[i], lv_color_hex(color), 0);
    }
    if (board->task_count == 0) {
        lv_label_set_text(s_task_labels[0], copy->wait_tasks);
        lv_obj_set_style_text_color(s_task_labels[0], lv_color_hex(theme->secondary), 0);
    }

    if (board->task_count == 0) {
        lv_label_set_text(s_task_detail_caption, copy->tasks_title);
        lv_label_set_text(s_task_detail_status, "");
        lv_label_set_text(s_task_detail_text, copy->wait_tasks);
        return;
    }

    const passport_task_t *task = &board->tasks[s_task_index];
    const char *marker = "□";
    uint32_t color = theme->primary;
    if (task->status == PASSPORT_TASK_DONE) {
        marker = "√";
        color = theme->secondary;
    } else if (task->status == PASSPORT_TASK_BLOCKED) {
        marker = "！";
        color = theme->accent;
    }
    lv_label_set_text_fmt(
        s_task_detail_caption,
        "%s  %u/%u",
        copy->tasks_title,
        s_task_index + 1,
        board->task_count
    );
    lv_label_set_text(s_task_detail_status, marker);
    lv_obj_set_style_text_color(s_task_detail_status, lv_color_hex(color), 0);
    lv_label_set_text(s_task_detail_text, task->text);
}

static void scroll_chat_answer_to_bottom(lv_anim_enable_t anim)
{
    lv_obj_update_layout(s_chat_answer_view);
    lv_obj_scroll_to_y(s_chat_answer_view, LV_COORD_MAX, anim);
}

static void set_chat_answer_height(int32_t height)
{
    if (lv_obj_get_height(s_chat_answer_view) == height) return;
    lv_obj_set_height(s_chat_answer_view, height);
    scroll_chat_answer_to_bottom(LV_ANIM_OFF);
}

static void render_chat(const passport_board_t *board)
{
    const companion_copy_t *copy = copy_for_language(board->language);
    lv_label_set_text_fmt(
        s_chat_user_name_label,
        "%s >",
        board->user_name[0] ? board->user_name : copy->user_fallback
    );
    lv_label_set_text(
        s_chat_user_label,
        board->dialogue_user[0] ? board->dialogue_user : ""
    );
    const char *answer = board->dialogue_answer[0] ? board->dialogue_answer : "";
    if (strcmp(lv_label_get_text(s_chat_answer_label), answer) != 0) {
        lv_label_set_text(s_chat_answer_label, answer);
        scroll_chat_answer_to_bottom(
            s_companion_page == COMPANION_PAGE_CONVERSATION ? LV_ANIM_ON : LV_ANIM_OFF
        );
    }
    const char *status = copy->chat_hold;
    if (s_speaker_active) {
        status = copy->chat_hq_speaking;
    } else {
        switch (board->dialogue_status) {
        case PASSPORT_DIALOGUE_TRANSCRIBING:
            status = copy->chat_transcribing;
            break;
        case PASSPORT_DIALOGUE_THINKING:
            status = copy->chat_hq_typing;
            break;
        case PASSPORT_DIALOGUE_ANSWERED:
            status = copy->chat_continue;
            break;
        case PASSPORT_DIALOGUE_ERROR:
            status = copy->chat_disconnected;
            break;
        default:
            break;
        }
    }
    const char *cursor = ((xTaskGetTickCount() / pdMS_TO_TICKS(450)) & 1U) ? "_" : " ";
    lv_label_set_text_fmt(s_chat_status_label, "%s%s", status, cursor);
}

static bool companion_connected(void)
{
    return s_connection != BLE_HS_CONN_HANDLE_NONE
        && s_event_subscribed
        && s_audio_subscribed;
}

static void render_connection_signal(void)
{
    const int8_t connected = companion_connected() ? 1 : 0;
    if (connected == s_rendered_connection_signal) return;

    const companion_theme_t *theme = current_theme();
    const uint32_t color = connected ? theme->accent : theme->track;
    for (unsigned i = 0; i < CONNECTION_SIGNAL_BAR_COUNT; i++) {
        lv_obj_set_style_bg_color(s_connection_signal_bars[i], lv_color_hex(color), 0);
    }
    s_rendered_connection_signal = connected;
}

static void render_settings(void)
{
    const companion_copy_t *copy = copy_for_language(s_ui_language);
    lv_label_set_text_fmt(
        s_settings_theme_label,
        "[%s]  %u/%u",
        s_theme_index >= COMPANION_THEME_COUNT
            ? copy->custom_theme_name
            : copy->theme_names[s_theme_index],
        s_theme_index + 1,
        theme_count()
    );
    if (companion_connected()) {
        lv_label_set_text_fmt(s_settings_ble_label, copy->connected_host, host_name());
    } else {
        lv_label_set_text(s_settings_ble_label, copy->waiting_host);
    }
}

static void show_companion_page(companion_page_t page)
{
    s_companion_page = page;
    if (page == COMPANION_PAGE_SETTINGS || page == COMPANION_PAGE_TASK_DETAIL) {
        lv_obj_add_flag(s_header_box, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(s_header_box, LV_OBJ_FLAG_HIDDEN);
    }
    if (page == COMPANION_PAGE_DASHBOARD) {
        lv_obj_remove_flag(s_goal_card, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_tasks_card, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_chat_card, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_task_detail_card, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_settings_card, LV_OBJ_FLAG_HIDDEN);
    } else if (page == COMPANION_PAGE_CONVERSATION) {
        lv_obj_add_flag(s_goal_card, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_tasks_card, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_chat_card, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_task_detail_card, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_settings_card, LV_OBJ_FLAG_HIDDEN);
        passport_board_t board = board_load(NULL);
        render_chat(&board);
        scroll_chat_answer_to_bottom(LV_ANIM_OFF);
    } else if (page == COMPANION_PAGE_TASK_DETAIL) {
        lv_obj_add_flag(s_goal_card, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_tasks_card, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_chat_card, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_task_detail_card, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_settings_card, LV_OBJ_FLAG_HIDDEN);
        passport_board_t board = board_load(NULL);
        render_board(&board);
    } else {
        lv_obj_add_flag(s_goal_card, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_tasks_card, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_chat_card, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_task_detail_card, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_settings_card, LV_OBJ_FLAG_HIDDEN);
        render_settings();
    }
}

static bool voice_feedback_active(uint32_t now)
{
    return s_voice_feedback_until != 0 && (int32_t)(s_voice_feedback_until - now) > 0;
}

static void render_voice_panel(uint32_t now)
{
    const companion_copy_t *copy = copy_for_language(s_ui_language);
    bool feedback = voice_feedback_active(now);
    bool visible = s_companion_page != COMPANION_PAGE_SETTINGS
                && (s_ptt_active || s_recording || feedback);
    if (!visible) {
        lv_obj_add_flag(s_voice_panel, LV_OBJ_FLAG_HIDDEN);
        if (s_companion_page == COMPANION_PAGE_CONVERSATION) {
            set_chat_answer_height(CHAT_ANSWER_HEIGHT);
            lv_obj_remove_flag(s_chat_status_label, LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }

    const companion_theme_t *theme = current_theme();
    bool inline_conversation = s_companion_page == COMPANION_PAGE_CONVERSATION
                            && s_ptt_route == PTT_ROUTE_CONVERSATION;
    lv_obj_remove_flag(s_voice_panel, LV_OBJ_FLAG_HIDDEN);
    if (inline_conversation) {
        set_chat_answer_height(CHAT_ANSWER_RECORDING_HEIGHT);
        lv_obj_add_flag(s_chat_status_label, LV_OBJ_FLAG_HIDDEN);
        // Keep the recording state in the user's right-hand lane. The HQ reply
        // viewport contracts to end at y=241, above the recording lane.
        lv_obj_set_pos(s_voice_panel, 48, 245);
        lv_obj_set_size(s_voice_panel, 176, 65);
        lv_obj_set_style_bg_opa(s_voice_panel, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(s_voice_panel, 0, 0);
        lv_obj_set_style_shadow_width(s_voice_panel, 0, 0);
        lv_obj_set_pos(s_voice_title, 0, 0);
        lv_obj_set_size(s_voice_title, 176, 20);
        lv_obj_set_style_text_align(s_voice_title, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_set_pos(s_voice_hint, 0, 47);
        lv_obj_set_size(s_voice_hint, 176, 18);
        lv_obj_set_style_text_align(s_voice_hint, LV_TEXT_ALIGN_RIGHT, 0);
    } else {
        if (s_companion_page == COMPANION_PAGE_CONVERSATION) {
            set_chat_answer_height(CHAT_ANSWER_HEIGHT);
        }
        lv_obj_set_pos(s_voice_panel, 12, 176);
        lv_obj_set_size(s_voice_panel, 216, 128);
        lv_obj_set_style_bg_color(s_voice_panel, lv_color_hex(theme->surface), 0);
        lv_obj_set_style_bg_opa(s_voice_panel, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(s_voice_panel, lv_color_hex(theme->border), 0);
        lv_obj_set_style_border_width(s_voice_panel, 1, 0);
        lv_obj_set_style_shadow_color(s_voice_panel, lv_color_hex(theme->accent), 0);
        lv_obj_set_style_shadow_width(s_voice_panel, theme->glow_opacity ? 5 : 0, 0);
        lv_obj_set_style_shadow_opa(s_voice_panel, theme->glow_opacity, 0);
        lv_obj_set_pos(s_voice_title, 10, 8);
        lv_obj_set_size(s_voice_title, 194, 26);
        lv_obj_set_style_text_align(s_voice_title, LV_TEXT_ALIGN_LEFT, 0);
        lv_obj_set_pos(s_voice_hint, 10, 100);
        lv_obj_set_size(s_voice_hint, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_style_text_align(s_voice_hint, LV_TEXT_ALIGN_LEFT, 0);
    }
    bool routes_to_task = s_ptt_route == PTT_ROUTE_TASK;
    if (s_recording) {
        lv_label_set_text(s_voice_title, routes_to_task ? copy->voice_task_recording : copy->voice_recording);
        if (routes_to_task) {
            lv_label_set_text_fmt(s_voice_hint, copy->voice_release_task, host_name());
        } else {
            lv_label_set_text(s_voice_hint, copy->voice_release_send);
        }
        lv_obj_set_style_text_color(s_voice_title, lv_color_hex(theme->accent), 0);
    } else if (s_ptt_active) {
        lv_label_set_text(s_voice_title, inline_conversation ? copy->voice_connecting : copy->voice_wait_phone);
        lv_label_set_text(s_voice_hint, inline_conversation ? copy->voice_release_cancel : copy->voice_phone_linking);
        lv_obj_set_style_text_color(s_voice_title, lv_color_hex(theme->primary), 0);
    } else if (s_voice_feedback_sent) {
        lv_label_set_text(s_voice_title, routes_to_task ? copy->voice_task_sent : copy->voice_transcribing);
        if (routes_to_task) {
            lv_label_set_text_fmt(s_voice_hint, copy->voice_task_analyzing, host_name());
        } else {
            lv_label_set_text(s_voice_hint, "");
        }
        lv_obj_set_style_text_color(s_voice_title, lv_color_hex(theme->accent), 0);
    } else {
        lv_label_set_text(s_voice_title, inline_conversation ? copy->voice_no_host : copy->voice_no_phone);
        lv_label_set_text(s_voice_hint, inline_conversation ? "" : copy->voice_enable_hint);
        lv_obj_set_style_text_color(s_voice_title, lv_color_hex(theme->accent), 0);
    }

    uint8_t level = s_recording ? s_audio_level : (s_ptt_active ? 18 : 8);
    if (level < 8) level = 8;
    // In conversation mode the title, waveform and release hint occupy three
    // separate vertical bands, even when the waveform reaches full height.
    int bar_baseline = inline_conversation ? 43 : 78;
    int max_bar_height = inline_conversation ? 20 : 46;
    for (unsigned i = 0; i < VOICE_BAR_COUNT; i++) {
        unsigned pulse = (s_voice_anim_frame * 13U + i * 23U) % 37U;
        int height = 5 + (int)level * (45 + (int)pulse) / 190;
        if (height > max_bar_height) height = max_bar_height;
        lv_obj_set_x(s_voice_bars[i], inline_conversation ? 20 + (int)i * 20 : 13 + (int)i * 27);
        lv_obj_set_y(s_voice_bars[i], bar_baseline - height);
        lv_obj_set_height(s_voice_bars[i], height);
    }
    s_voice_anim_frame++;
}

// Follow a color the app pushed. Only a change switches the skin, so a
// preset the user picked on the device survives the app resending its color
// on every connection.
static void apply_board_theme_color(const passport_board_t *board)
{
    switch (board->theme_color_state) {
    case PASSPORT_THEME_COLOR_UNSET:
        return;
    case PASSPORT_THEME_COLOR_NONE:
        if (!s_has_custom_theme) return;
        s_has_custom_theme = false;
        if (s_theme_index >= COMPANION_THEME_COUNT) s_theme_index = 0;
        break;
    case PASSPORT_THEME_COLOR_CUSTOM:
        if (s_has_custom_theme && s_custom_color == (board->theme_color & 0xFFFFFFU)) return;
        set_custom_theme(board->theme_color);
        s_theme_index = COMPANION_THEME_COUNT;
        break;
    }
    apply_theme();
    schedule_theme_save();
}

static void tick(lv_timer_t *timer)
{
    (void)timer;
    uint32_t revision;
    passport_board_t board = board_load(&revision);
    if (board.language != s_ui_language) {
        apply_language(board.language);
    }
    memcpy(s_host_name, board.host_name, sizeof(s_host_name));
    apply_board_theme_color(&board);
    uint32_t now = (uint32_t)xTaskGetTickCount();
    render_voice_panel(now);
    render_connection_signal();

    if (s_companion_page == COMPANION_PAGE_SETTINGS) render_settings();
    if (revision == s_rendered_revision) {
        if (s_companion_page == COMPANION_PAGE_CONVERSATION) render_chat(&board);
        return;
    }
    s_rendered_revision = revision;
    render_board(&board);
    render_chat(&board);
}

static lv_obj_t *make_terminal_box(lv_obj_t *parent, int x, int y, int width, int height)
{
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(box, x, y);
    lv_obj_set_size(box, width, height);
    lv_obj_set_style_radius(box, 0, 0);
    lv_obj_set_style_border_width(box, 1, 0);
    lv_obj_set_style_pad_all(box, 0, 0);
    return box;
}

static lv_obj_t *make_terminal_label(lv_obj_t *parent, const char *text, int x, int y)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_pos(label, x, y);
    lv_obj_set_style_text_font(label, &lv_font_terminal_extra_16, 0);
    lv_label_set_text(label, text);
    return label;
}

static lv_obj_t *make_value_label(lv_obj_t *parent, int x, int y, int width, int height)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_pos(label, x, y);
    lv_obj_set_size(label, width, height);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(label, &lv_font_terminal_extra_16, 0);
    lv_obj_set_style_text_line_space(label, 3, 0);
    return label;
}

static void apply_language(passport_language_t language)
{
    const companion_copy_t *copy = copy_for_language(language);
    s_ui_language = language;
    lv_label_set_text(s_brand_label, copy->app_name);
    lv_label_set_text(s_status_label, copy->terminal_name);
    lv_label_set_text(s_nav_label, copy->task_nav);
    lv_label_set_text(s_footer_label, copy->footer);
    lv_label_set_text(s_task_detail_controls, copy->task_detail_controls);
    lv_label_set_text_fmt(s_chat_hq_name_label, "%s >", copy->hq_name);
    lv_label_set_text(s_settings_title_label, copy->settings_title);
    lv_label_set_text(s_settings_connection_title_label, copy->connection_title);
    lv_label_set_text(s_settings_firmware_title_label, copy->firmware_title);
    lv_label_set_text(s_settings_theme_title_label, copy->theme_title);
    lv_label_set_text(s_settings_controls_label, copy->settings_controls);
    render_settings();
    s_rendered_revision = UINT32_MAX;
}

static void apply_theme(void)
{
    const companion_theme_t *theme = current_theme();
    lv_obj_set_style_bg_color(s_screen, lv_color_hex(theme->background), 0);
    lv_obj_set_style_text_color(s_brand_label, lv_color_hex(theme->primary), 0);
    lv_obj_set_style_text_color(s_status_label, lv_color_hex(theme->primary), 0);
    lv_obj_set_style_text_color(s_goal_caption, lv_color_hex(theme->secondary), 0);
    lv_obj_set_style_text_color(s_progress_label, lv_color_hex(theme->primary), 0);
    lv_obj_set_style_text_color(s_goal_label, lv_color_hex(theme->primary), 0);
    lv_obj_set_style_text_color(s_nav_label, lv_color_hex(theme->secondary), 0);
    lv_obj_set_style_text_color(s_tasks_caption, lv_color_hex(theme->secondary), 0);
    for (uint8_t i = 0; i < PASSPORT_BOARD_TASK_COUNT; i++) {
        lv_obj_set_style_text_color(s_task_labels[i], lv_color_hex(theme->primary), 0);
    }
    lv_obj_set_style_text_color(s_footer_label, lv_color_hex(theme->secondary), 0);
    lv_obj_set_style_text_color(s_task_detail_caption, lv_color_hex(theme->secondary), 0);
    lv_obj_set_style_text_color(s_task_detail_text, lv_color_hex(theme->primary), 0);
    lv_obj_set_style_text_color(s_task_detail_controls, lv_color_hex(theme->secondary), 0);
    lv_obj_set_style_text_color(s_voice_title, lv_color_hex(theme->accent), 0);
    lv_obj_set_style_text_color(s_voice_hint, lv_color_hex(theme->secondary), 0);
    lv_obj_set_style_text_color(s_chat_card, lv_color_hex(theme->primary), 0);
    lv_obj_set_style_text_color(s_settings_card, lv_color_hex(theme->primary), 0);
    lv_obj_set_style_text_color(s_chat_user_name_label, lv_color_hex(theme->accent), 0);
    lv_obj_set_style_text_color(s_chat_user_label, lv_color_hex(theme->primary), 0);
    lv_obj_set_style_text_color(s_chat_answer_label, lv_color_hex(theme->primary), 0);
    lv_obj_set_style_text_color(s_chat_status_label, lv_color_hex(theme->accent), 0);
    lv_obj_set_style_text_color(s_settings_theme_label, lv_color_hex(theme->primary), 0);
    lv_obj_set_style_text_color(s_settings_ble_label, lv_color_hex(theme->accent), 0);
    for (unsigned i = 0; i < VOICE_BAR_COUNT; i++) {
        lv_obj_set_style_bg_color(s_voice_bars[i], lv_color_hex(theme->accent), 0);
    }

    lv_obj_t *boxes[] = {
        s_header_box,
        s_goal_card,
        s_tasks_card,
        s_task_detail_card,
        s_settings_card,
        s_voice_panel,
    };
    for (size_t i = 0; i < sizeof(boxes) / sizeof(boxes[0]); i++) {
        lv_obj_set_style_bg_color(boxes[i], lv_color_hex(theme->surface), 0);
        lv_obj_set_style_border_color(boxes[i], lv_color_hex(theme->border), 0);
        lv_obj_set_style_shadow_color(boxes[i], lv_color_hex(theme->accent), 0);
        lv_obj_set_style_shadow_width(boxes[i], theme->glow_opacity ? 5 : 0, 0);
        lv_obj_set_style_shadow_opa(boxes[i], theme->glow_opacity, 0);
    }
    lv_obj_set_style_bg_color(s_progress_bar, lv_color_hex(theme->track), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_progress_bar, lv_color_hex(theme->accent), LV_PART_INDICATOR);
    s_rendered_connection_signal = -1;
    render_connection_signal();
    s_rendered_revision = UINT32_MAX;
    if (s_companion_page == COMPANION_PAGE_SETTINGS) render_settings();
    ESP_LOGI(TAG, "界面主题:%s", theme->name);
}

void demo_ble_enter(void)
{
    s_page_active = true;
    s_ptt_active = false;
    s_conversation_open_pending = false;
    s_speaker_active = false;
    s_speaker_abort = false;
    s_speaker_prefill_pending = false;
    s_speaker_end_received = false;
    s_audio_level = 0;
    s_scope_index = 0;
    s_task_index = 0;
    s_companion_page = COMPANION_PAGE_DASHBOARD;
    s_settings_return_page = COMPANION_PAGE_DASHBOARD;
    s_up_long_active = false;
    s_down_long_active = false;
    s_ptt_route = PTT_ROUTE_NONE;
    s_voice_anim_frame = 0;
    s_voice_feedback_until = 0;
    s_voice_feedback_sent = false;
    s_rendered_connection_signal = -1;
    s_rendered_revision = UINT32_MAX;
    bsp_audio_set_volume(PASSPORT_SPEAKER_VOLUME);
    if (!s_audio_task) {
        xTaskCreate(audio_task, "passport_audio", 4096, NULL, 5, &s_audio_task);
    }
    if (!s_speaker_queue) {
        s_speaker_queue = xQueueCreate(SPEAKER_QUEUE_DEPTH, sizeof(speaker_queue_item_t));
    } else {
        xQueueReset(s_speaker_queue);
    }
    if (s_speaker_queue && !s_speaker_task) {
        xTaskCreate(speaker_task, "passport_speaker", 4096, NULL, 5, &s_speaker_task);
    }

    s_screen = lv_obj_create(NULL);
    lv_obj_remove_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_border_width(s_screen, 0, 0);
    lv_obj_set_style_pad_all(s_screen, 0, 0);

    s_header_box = make_terminal_box(s_screen, 8, 8, 224, 40);
    s_brand_label = make_terminal_label(s_header_box, "伟大的我", 5, 1);
    s_status_label = make_terminal_label(s_header_box, "特工终端", 5, 20);
    for (unsigned i = 0; i < CONNECTION_SIGNAL_BAR_COUNT; i++) {
        const int height = 4 + (int)i * 3;
        s_connection_signal_bars[i] = lv_obj_create(s_header_box);
        lv_obj_remove_flag(s_connection_signal_bars[i], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_size(s_connection_signal_bars[i], 4, height);
        lv_obj_set_pos(s_connection_signal_bars[i], 192 + (int)i * 7, 17 - height);
        lv_obj_set_style_border_width(s_connection_signal_bars[i], 0, 0);
        lv_obj_set_style_radius(s_connection_signal_bars[i], 0, 0);
        lv_obj_set_style_pad_all(s_connection_signal_bars[i], 0, 0);
    }

    s_goal_card = make_terminal_box(s_screen, 8, 51, 224, 104);
    s_goal_caption = make_terminal_label(s_goal_card, "本周目标 1/3", 7, 3);
    s_goal_label = make_value_label(s_goal_card, 7, 23, 210, 39);
    lv_label_set_text(s_goal_label, "等待本周目标");
    s_progress_bar = lv_bar_create(s_goal_card);
    lv_obj_set_size(s_progress_bar, 166, 7);
    lv_obj_set_pos(s_progress_bar, 7, 70);
    lv_obj_set_style_radius(s_progress_bar, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(s_progress_bar, 0, LV_PART_INDICATOR);
    lv_bar_set_range(s_progress_bar, 0, 100);
    s_progress_label = make_value_label(s_goal_card, 179, 62, 38, 20);
    lv_obj_set_style_text_align(s_progress_label, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_text(s_progress_label, "0%");
    s_nav_label = make_terminal_label(s_goal_card, "短按上下键切换今日任务", 7, 82);

    s_tasks_card = make_terminal_box(s_screen, 8, 158, 224, 154);
    s_tasks_caption = make_terminal_label(s_tasks_card, "今日任务 0/0", 7, 3);
    for (uint8_t i = 0; i < PASSPORT_BOARD_TASK_COUNT; i++) {
        s_task_labels[i] = make_value_label(s_tasks_card, 7, 24 + i * 21, 210, 20);
        lv_label_set_long_mode(s_task_labels[i], LV_LABEL_LONG_MODE_DOTS);
    }
    lv_label_set_text(s_task_labels[0], "等待同步今日任务");
    s_footer_label = make_terminal_label(s_tasks_card, "长按上:对话  长按下:任务", 7, 133);

    s_task_detail_card = make_terminal_box(s_screen, 0, 0, 240, 320);
    s_task_detail_caption = make_terminal_label(s_task_detail_card, "今日任务", 10, 8);
    s_task_detail_status = make_terminal_label(s_task_detail_card, "□", 10, 54);
    s_task_detail_text = make_value_label(s_task_detail_card, 10, 83, 220, 180);
    s_task_detail_controls = make_value_label(s_task_detail_card, 10, 276, 220, 36);
    lv_label_set_text(s_task_detail_controls, "上下切换  双击确定返回");
    lv_obj_add_flag(s_task_detail_card, LV_OBJ_FLAG_HIDDEN);

    s_chat_card = lv_obj_create(s_screen);
    lv_obj_remove_flag(s_chat_card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(s_chat_card, 8, 51);
    lv_obj_set_size(s_chat_card, 224, 261);
    lv_obj_set_style_bg_opa(s_chat_card, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_chat_card, 0, 0);
    lv_obj_set_style_pad_all(s_chat_card, 0, 0);
    s_chat_user_name_label = make_terminal_label(s_chat_card, "你 >", 48, 3);
    lv_obj_set_size(s_chat_user_name_label, 176, 20);
    lv_label_set_long_mode(s_chat_user_name_label, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_align(s_chat_user_name_label, LV_TEXT_ALIGN_RIGHT, 0);
    s_chat_user_label = make_value_label(s_chat_card, 48, 24, 176, 54);
    lv_obj_set_style_text_align(s_chat_user_label, LV_TEXT_ALIGN_RIGHT, 0);
    s_chat_hq_name_label = make_terminal_label(s_chat_card, "总部 >", 0, 91);
    s_chat_answer_view = lv_obj_create(s_chat_card);
    lv_obj_set_pos(s_chat_answer_view, 0, 112);
    lv_obj_set_size(s_chat_answer_view, 176, CHAT_ANSWER_HEIGHT);
    lv_obj_set_scroll_dir(s_chat_answer_view, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_chat_answer_view, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_bg_opa(s_chat_answer_view, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_chat_answer_view, 0, 0);
    lv_obj_set_style_pad_all(s_chat_answer_view, 0, 0);
    s_chat_answer_label = make_value_label(s_chat_answer_view, 0, 0, 176, LV_SIZE_CONTENT);
    s_chat_status_label = make_terminal_label(s_chat_card, "长按上键说话_ ", 0, 239);
    lv_obj_add_flag(s_chat_card, LV_OBJ_FLAG_HIDDEN);

    s_settings_card = make_terminal_box(s_screen, 0, 0, 240, 320);
    s_settings_title_label = make_terminal_label(s_settings_card, "系统设置", 10, 8);
    s_settings_connection_title_label = make_terminal_label(s_settings_card, "连接状态", 10, 52);
    s_settings_ble_label = make_terminal_label(s_settings_card, "[等待 App 连接]", 10, 80);
    s_settings_firmware_title_label = make_terminal_label(s_settings_card, "固件版本", 10, 126);
    make_terminal_label(s_settings_card, "AI Passport " PASSPORT_UI_VERSION, 10, 154);
    s_settings_theme_title_label = make_terminal_label(s_settings_card, "主题选择", 10, 200);
    s_settings_theme_label = make_terminal_label(s_settings_card, "[琥珀]  1/4", 10, 228);
    s_settings_controls_label = make_terminal_label(s_settings_card, "上键返回    确定键切换主题", 10, 289);
    lv_obj_add_flag(s_settings_card, LV_OBJ_FLAG_HIDDEN);

    s_voice_panel = make_terminal_box(s_screen, 12, 176, 216, 128);
    s_voice_title = make_value_label(s_voice_panel, 10, 8, 194, 26);
    lv_label_set_text(s_voice_title, "● 正在录音");
    for (unsigned i = 0; i < VOICE_BAR_COUNT; i++) {
        s_voice_bars[i] = lv_obj_create(s_voice_panel);
        lv_obj_remove_flag(s_voice_bars[i], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_size(s_voice_bars[i], 12, 5);
        lv_obj_set_pos(s_voice_bars[i], 13 + (int)i * 27, 73);
        lv_obj_set_style_border_width(s_voice_bars[i], 0, 0);
        lv_obj_set_style_radius(s_voice_bars[i], 0, 0);
        lv_obj_set_style_pad_all(s_voice_bars[i], 0, 0);
    }
    s_voice_hint = make_terminal_label(s_voice_panel, "松开按键发送", 10, 100);
    lv_obj_add_flag(s_voice_panel, LV_OBJ_FLAG_HIDDEN);

    apply_language(PASSPORT_LANGUAGE_ZH_HANS);
    load_theme_preferences();
    apply_theme();
    show_companion_page(COMPANION_PAGE_DASHBOARD);
    s_timer = lv_timer_create(tick, 100, NULL);
    lv_screen_load(s_screen);
    if (ble_start() != ESP_OK) {
        s_page_state = BLE_PAGE_FAILED;
    }
}

void demo_ble_exit(void)
{
    s_page_active = false;
    s_ptt_active = false;
    s_ptt_route = PTT_ROUTE_NONE;
    s_up_long_active = false;
    s_down_long_active = false;
    s_conversation_open_pending = false;
    abort_speaker_playback();
    for (int i = 0; i < 15 && s_recording; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (s_timer) {
        lv_timer_delete(s_timer);
        s_timer = NULL;
    }
    if (s_theme_save_timer && esp_timer_is_active(s_theme_save_timer)) {
        esp_timer_stop(s_theme_save_timer);
        save_theme_preferences();
    }
    ble_stop();
    if (s_screen) {
        lv_obj_delete(s_screen);
        s_screen = NULL;
        s_header_box = NULL;
        s_brand_label = NULL;
        s_status_label = NULL;
        for (unsigned i = 0; i < CONNECTION_SIGNAL_BAR_COUNT; i++) {
            s_connection_signal_bars[i] = NULL;
        }
        s_goal_card = NULL;
        s_goal_caption = NULL;
        s_goal_label = NULL;
        s_progress_bar = NULL;
        s_progress_label = NULL;
        s_nav_label = NULL;
        s_tasks_card = NULL;
        s_tasks_caption = NULL;
        for (uint8_t i = 0; i < PASSPORT_BOARD_TASK_COUNT; i++) {
            s_task_labels[i] = NULL;
        }
        s_footer_label = NULL;
        s_task_detail_card = NULL;
        s_task_detail_caption = NULL;
        s_task_detail_status = NULL;
        s_task_detail_text = NULL;
        s_task_detail_controls = NULL;
        s_voice_panel = NULL;
        s_voice_title = NULL;
        s_voice_hint = NULL;
        for (unsigned i = 0; i < VOICE_BAR_COUNT; i++) {
            s_voice_bars[i] = NULL;
        }
        s_chat_card = NULL;
        s_chat_user_name_label = NULL;
        s_chat_user_label = NULL;
        s_chat_hq_name_label = NULL;
        s_chat_answer_view = NULL;
        s_chat_answer_label = NULL;
        s_chat_status_label = NULL;
        s_settings_card = NULL;
        s_settings_title_label = NULL;
        s_settings_connection_title_label = NULL;
        s_settings_theme_label = NULL;
        s_settings_ble_label = NULL;
        s_settings_firmware_title_label = NULL;
        s_settings_theme_title_label = NULL;
        s_settings_controls_label = NULL;
    }
}

static void move_task_selection(int direction)
{
    passport_board_t board = board_load(NULL);
    if (board.task_count == 0) return;
    if (s_task_index >= board.task_count) s_task_index = board.task_count - 1;
    if (direction < 0) {
        s_task_index = (s_task_index + board.task_count - 1) % board.task_count;
    } else {
        s_task_index = (s_task_index + 1) % board.task_count;
    }
    s_rendered_revision = UINT32_MAX;
    ESP_LOGI(TAG, "今日任务切换:%u/%u", s_task_index + 1, (unsigned)board.task_count);
}

void demo_ble_key(bsp_btn_t button, bsp_btn_ev_t event)
{
    uint32_t now = (uint32_t)xTaskGetTickCount();

    if (button == BSP_BTN_UP && event == BSP_BTN_CLICK && s_speaker_active) {
        abort_speaker_playback();
        ESP_LOGI(TAG, "上键短按,停止总部语音播放");
        return;
    }

    if (button == BSP_BTN_UP && event == BSP_BTN_LONG
        && s_companion_page != COMPANION_PAGE_SETTINGS) {
        s_up_long_active = true;
        abort_speaker_playback();
        show_companion_page(COMPANION_PAGE_CONVERSATION);
        s_ptt_route = PTT_ROUTE_CONVERSATION;
        s_ptt_active = true;
        s_audio_level = 0;
        s_voice_feedback_until = 0;
        s_voice_feedback_sent = false;
        s_conversation_open_pending = true;
        ESP_LOGI(TAG, "上键长按,进入总部对话并请求录音");
        return;
    }

    if (button == BSP_BTN_UP && event == BSP_BTN_RELEASE && s_up_long_active) {
        s_voice_feedback_sent = s_recording;
        s_ptt_active = false;
        s_up_long_active = false;
        s_conversation_open_pending = false;
        s_voice_feedback_until = now + pdMS_TO_TICKS(VOICE_FEEDBACK_MS);
        ESP_LOGI(TAG, "上键松开,录音%s", s_voice_feedback_sent ? "已发送" : "未发送");
        return;
    }

    if (button == BSP_BTN_DOWN && event == BSP_BTN_LONG
        && s_companion_page != COMPANION_PAGE_SETTINGS) {
        s_down_long_active = true;
        abort_speaker_playback();
        show_companion_page(COMPANION_PAGE_DASHBOARD);
        s_ptt_route = PTT_ROUTE_TASK;
        s_ptt_active = true;
        s_audio_level = 0;
        s_voice_feedback_until = 0;
        s_voice_feedback_sent = false;
        s_conversation_open_pending = false;
        ESP_LOGI(TAG, "下键长按,请求主机任务整理录音");
        return;
    }

    if (button == BSP_BTN_DOWN && event == BSP_BTN_RELEASE && s_down_long_active) {
        s_voice_feedback_sent = s_recording;
        s_ptt_active = false;
        s_down_long_active = false;
        s_voice_feedback_until = now + pdMS_TO_TICKS(VOICE_FEEDBACK_MS);
        ESP_LOGI(TAG, "下键松开,任务录音%s", s_voice_feedback_sent ? "已发送" : "未发送");
        return;
    }

    if (button == BSP_BTN_UP && event == BSP_BTN_CLICK) {
        if (s_companion_page == COMPANION_PAGE_SETTINGS) {
            show_companion_page(s_settings_return_page);
            return;
        }
        if (s_companion_page == COMPANION_PAGE_CONVERSATION) {
            show_companion_page(COMPANION_PAGE_DASHBOARD);
            ESP_LOGI(TAG, "上键短按,返回任务主页");
            return;
        }
        if (s_companion_page == COMPANION_PAGE_DASHBOARD
            || s_companion_page == COMPANION_PAGE_TASK_DETAIL) {
            move_task_selection(-1);
        }
        return;
    }

    if (button == BSP_BTN_DOWN && event == BSP_BTN_CLICK) {
        if (s_companion_page == COMPANION_PAGE_CONVERSATION) {
            show_companion_page(COMPANION_PAGE_DASHBOARD);
        } else if (s_companion_page == COMPANION_PAGE_DASHBOARD
                   || s_companion_page == COMPANION_PAGE_TASK_DETAIL) {
            move_task_selection(1);
        }
        return;
    }

    if (button == BSP_BTN_OK && event == BSP_BTN_DOUBLE) {
        if (s_companion_page != COMPANION_PAGE_SETTINGS) {
            s_settings_return_page = s_companion_page;
            show_companion_page(COMPANION_PAGE_SETTINGS);
            ESP_LOGI(TAG, "双击确定键,进入全屏设置");
        }
        return;
    }

    if (button == BSP_BTN_OK && event == BSP_BTN_CLICK) {
        s_theme_index = (s_theme_index + 1) % theme_count();
        apply_theme();
        schedule_theme_save();
    }
}
