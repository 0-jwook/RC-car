// =============================================================
// ESP32 Mecanum Wheel Robot - Web Controller
// =============================================================
// 하드웨어:  ESP32 DevKit + Cytron MD10C × 4 + CHP 36GP-555 × 4
// 통신:      Wi-Fi (AP 또는 STA) + WebSocket JSON
// 웹 UI:     PROGMEM 내장 HTML (web_ui.h)
//
// JSON 프로토콜 (클라이언트 → 서버):
//   {"vx": ±1.0, "vy": ±1.0, "w": ±1.0, "speed": 0-100}
//
// Cytron MD10C 제어:
//   PWM 핀 → 속도 (0-255, LEDC 20 kHz)
//   DIR 핀 → 방향 (HIGH=정방향, LOW=역방향)
// =============================================================

#include <Arduino.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <AsyncTCP.h>
#include <ArduinoJson.h>
#include "web_ui.h"

// =============================================================
// [사용자 설정] Wi-Fi
// =============================================================
// true  → AP 모드: ESP32가 직접 핫스팟 생성 (인터넷 불필요, 권장)
// false → STA 모드: 기존 공유기에 연결
#define WIFI_AP_MODE   true

const char* AP_SSID     = "RCCAR_Robot";   // AP 모드 SSID
const char* AP_PASSWORD = "12345678";       // AP 모드 비밀번호 (최소 8자)

const char* STA_SSID    = "bssm_free";       // STA 모드: 공유기 SSID
const char* STA_PASS    = "bssm_free";   // STA 모드: 공유기 비밀번호

// =============================================================
// [사용자 설정] 모터 핀 번호
// 위에서 본 메카넘 배치:
//   [전방]
//   M1(FL) ↗   ↖ M2(FR)
//   M3(RL) ↖   ↗ M4(RR)
//   [후방]
// =============================================================
//                   PWM핀  DIR핀
#define M1_PWM_PIN   25     // Front-Left  (전방 좌측)
#define M1_DIR_PIN   26
#define M2_PWM_PIN   27     // Front-Right (전방 우측)
#define M2_DIR_PIN   14
#define M3_PWM_PIN   32     // Rear-Left   (후방 좌측)
#define M3_DIR_PIN   33
#define M4_PWM_PIN   18     // Rear-Right  (후방 우측)
#define M4_DIR_PIN   19

// =============================================================
// [사용자 설정] LEDC PWM 파라미터
// =============================================================
#define PWM_FREQ_HZ  20000  // 20 kHz: 모터 노이즈 가청 주파수 초과
#define PWM_BITS     8      // 8비트 분해능: 0~255
// LEDC 채널 (ESP32: 채널 0~15)
#define CH_M1        0
#define CH_M2        1
#define CH_M3        2
#define CH_M4        3

// =============================================================
// [사용자 설정] 워치독 타임아웃
// =============================================================
#define WATCHDOG_MS  500    // 500ms 이상 명령 없으면 자동 정지

// =============================================================
// [사용자 설정] 모터 방향 반전 보정
// 모터가 반대로 돌면 해당 값을 true 로 변경
// 예: 전방 좌측이 반대로 돌면 INVERT_M1 = true
// (물리 배선 변경 없이 소프트웨어로 보정)
// =============================================================
#define INVERT_M1    true   // Front-Left  (좌측 반전 보정)
#define INVERT_M2    false  // Front-Right
#define INVERT_M3    true   // Rear-Left   (좌측 반전 보정)
#define INVERT_M4    false  // Rear-Right

// =============================================================
// 전역 객체 / 변수
// =============================================================
AsyncWebServer server(80);
AsyncWebSocket ws("/ws");

// 워치독: 마지막 명령 수신 시각 (loop()와 WS 콜백 양쪽에서 접근)
volatile unsigned long lastCmdMs  = 0;
volatile bool          motorsOn   = false;

// =============================================================
// 모터 초기화
// =============================================================
void motorInit() {
    // LEDC 채널 초기화 (주파수, 분해능)
    ledcSetup(CH_M1, PWM_FREQ_HZ, PWM_BITS);
    ledcSetup(CH_M2, PWM_FREQ_HZ, PWM_BITS);
    ledcSetup(CH_M3, PWM_FREQ_HZ, PWM_BITS);
    ledcSetup(CH_M4, PWM_FREQ_HZ, PWM_BITS);

    // PWM 핀 ↔ LEDC 채널 연결
    ledcAttachPin(M1_PWM_PIN, CH_M1);
    ledcAttachPin(M2_PWM_PIN, CH_M2);
    ledcAttachPin(M3_PWM_PIN, CH_M3);
    ledcAttachPin(M4_PWM_PIN, CH_M4);

    // DIR 핀 출력 설정 및 초기값 LOW
    pinMode(M1_DIR_PIN, OUTPUT); digitalWrite(M1_DIR_PIN, LOW);
    pinMode(M2_DIR_PIN, OUTPUT); digitalWrite(M2_DIR_PIN, LOW);
    pinMode(M3_DIR_PIN, OUTPUT); digitalWrite(M3_DIR_PIN, LOW);
    pinMode(M4_DIR_PIN, OUTPUT); digitalWrite(M4_DIR_PIN, LOW);

    // 초기 정지
    ledcWrite(CH_M1, 0);
    ledcWrite(CH_M2, 0);
    ledcWrite(CH_M3, 0);
    ledcWrite(CH_M4, 0);
}

// =============================================================
// 단일 모터 구동
//   ch      : LEDC 채널 번호
//   dirPin  : 방향 GPIO 핀
//   value   : -255 ~ +255 (음수 = 역방향)
//   invert  : 방향 반전 여부 (INVERT_Mx 상수)
// =============================================================
static inline void setMotor(uint8_t ch, uint8_t dirPin, int value, bool invert) {
    if (invert) value = -value;

    if (value >= 0) {
        digitalWrite(dirPin, HIGH);                             // 정방향
        ledcWrite(ch, (uint8_t)constrain(value, 0, 255));
    } else {
        digitalWrite(dirPin, LOW);                              // 역방향
        ledcWrite(ch, (uint8_t)constrain(-value, 0, 255));
    }
}

// =============================================================
// 전체 모터 즉시 정지
// =============================================================
void stopAll() {
    ledcWrite(CH_M1, 0);
    ledcWrite(CH_M2, 0);
    ledcWrite(CH_M3, 0);
    ledcWrite(CH_M4, 0);
    motorsOn = false;
}

// =============================================================
// 메카넘 운동학 계산 및 모터 구동
//
// 입력 범위:
//   vx    -1.0 ~ +1.0   (횡이동: + 우측, - 좌측)
//   vy    -1.0 ~ +1.0   (전후:   + 전진, - 후진)
//   w     -1.0 ~ +1.0   (회전:   + CW,   - CCW)
//   speed  0   ~ 100    (속도 %)
//
// 메카넘 운동학 (위에서 본 X자 롤러 배치 기준):
//   M1(FL) = Vy + Vx + ω
//   M2(FR) = Vy - Vx - ω
//   M3(RL) = Vy - Vx + ω
//   M4(RR) = Vy + Vx - ω
//
// 정규화: 최대 절댓값이 1.0 초과 시 비율 유지하며 스케일 다운
// 최종 출력: -255 ~ +255 정수
// =============================================================
void driveRobot(float vx, float vy, float w, float speed) {
    // 1. 메카넘 운동학
    float raw[4];
    raw[0] = vy + vx + w;   // M1: Front-Left
    raw[1] = vy - vx - w;   // M2: Front-Right
    raw[2] = vy - vx + w;   // M3: Rear-Left
    raw[3] = vy + vx - w;   // M4: Rear-Right

    // 2. 정규화 (최대값 ≤ 1.0 유지)
    float maxAbs = 1.0f;
    for (int i = 0; i < 4; i++) {
        float a = fabsf(raw[i]);
        if (a > maxAbs) maxAbs = a;
    }

    // 3. 속도 스케일 적용 → -255 ~ +255
    float scale = (speed / 100.0f) * 255.0f / maxAbs;

    int v[4];
    bool anyNonZero = false;
    for (int i = 0; i < 4; i++) {
        v[i] = (int)(raw[i] * scale);
        if (v[i] != 0) anyNonZero = true;
    }

    // 4. 각 모터 출력
    setMotor(CH_M1, M1_DIR_PIN, v[0], INVERT_M1);
    setMotor(CH_M2, M2_DIR_PIN, v[1], INVERT_M2);
    setMotor(CH_M3, M3_DIR_PIN, v[2], INVERT_M3);
    setMotor(CH_M4, M4_DIR_PIN, v[3], INVERT_M4);

    motorsOn = anyNonZero;
}

// =============================================================
// WebSocket 이벤트 핸들러
// (ESPAsyncWebServer는 내부적으로 Core 0의 네트워크 태스크에서 실행)
// =============================================================
void onWsEvent(AsyncWebSocket* srv, AsyncWebSocketClient* client,
               AwsEventType type, void* arg, uint8_t* data, size_t len)
{
    switch (type) {

        case WS_EVT_CONNECT:
            Serial.printf("[WS] Client #%u connected  IP: %s\n",
                          client->id(),
                          client->remoteIP().toString().c_str());
            break;

        case WS_EVT_DISCONNECT:
            Serial.printf("[WS] Client #%u disconnected\n", client->id());
            stopAll();                  // 연결 끊기면 즉시 정지
            break;

        case WS_EVT_DATA: {
            AwsFrameInfo* info = (AwsFrameInfo*)arg;
            // 텍스트 프레임이 단일 조각(fragmentation 없음)인 경우만 처리
            if (info->final && info->index == 0 &&
                info->len == len && info->opcode == WS_TEXT)
            {
                if (len == 0 || len >= 128) return;   // 비정상 길이 무시

                // null-terminate 후 JSON 파싱
                char buf[128];
                memcpy(buf, data, len);
                buf[len] = '\0';

                StaticJsonDocument<128> doc;
                if (deserializeJson(doc, buf) != DeserializationError::Ok) return;

                float vx    = constrain((float)(doc["vx"]    | 0.0f), -1.0f, 1.0f);
                float vy    = constrain((float)(doc["vy"]    | 0.0f), -1.0f, 1.0f);
                float w     = constrain((float)(doc["w"]     | 0.0f), -1.0f, 1.0f);
                float speed = constrain((float)(doc["speed"] | 50.0f), 0.0f, 100.0f);

                driveRobot(vx, vy, w, speed);
                lastCmdMs = millis();   // 워치독 타이머 리셋
            }
            break;
        }

        case WS_EVT_ERROR:
            Serial.printf("[WS] Error on client #%u: %s\n",
                          client->id(), (char*)data);
            break;

        default:
            break;
    }
}

// =============================================================
// Wi-Fi 초기화
// =============================================================
void wifiSetup() {
    if (WIFI_AP_MODE) {
        // ── AP 모드 ──────────────────────────────────────────
        WiFi.mode(WIFI_AP);
        WiFi.softAP(AP_SSID, AP_PASSWORD);
        Serial.println("[WiFi] AP Mode");
        Serial.printf("  SSID    : %s\n", AP_SSID);
        Serial.printf("  Password: %s\n", AP_PASSWORD);
        Serial.printf("  IP Addr : %s\n", WiFi.softAPIP().toString().c_str());

    } else {
        // ── STA 모드 ─────────────────────────────────────────
        WiFi.mode(WIFI_STA);
        WiFi.begin(STA_SSID, STA_PASS);
        Serial.printf("[WiFi] STA Mode  Connecting to '%s'", STA_SSID);

        unsigned long t0 = millis();
        while (WiFi.status() != WL_CONNECTED) {
            if (millis() - t0 > 15000UL) {
                Serial.println("\n[WiFi] Timeout → Falling back to AP mode");
                WiFi.softAP(AP_SSID, AP_PASSWORD);
                Serial.printf("[WiFi] AP IP: %s\n",
                              WiFi.softAPIP().toString().c_str());
                return;
            }
            delay(300);
            Serial.print(".");
        }
        Serial.printf("\n[WiFi] Connected! IP: %s\n",
                      WiFi.localIP().toString().c_str());
    }
}

// =============================================================
// setup()
// =============================================================
void setup() {
    Serial.begin(115200);
    Serial.println();
    Serial.println("============================================");
    Serial.println("  ESP32 Mecanum Wheel Robot v1.0");
    Serial.println("============================================");

    motorInit();
    wifiSetup();

    // ── WebSocket ──
    ws.onEvent(onWsEvent);
    server.addHandler(&ws);

    // ── 메인 페이지: PROGMEM 내장 HTML 서빙 ──
    server.on("/", HTTP_GET, [](AsyncWebServerRequest* req) {
        req->send(200, "text/html", index_html);
    });

    // ── 404 핸들러 ──
    server.onNotFound([](AsyncWebServerRequest* req) {
        req->send(404, "text/plain", "Not Found");
    });

    server.begin();
    Serial.println("[HTTP] Web server started  →  Open browser to robot IP");
}

// =============================================================
// loop()
// =============================================================
void loop() {
    // ── 워치독 ──────────────────────────────────────────────
    // 모터가 동작 중인데 일정 시간 명령이 없으면 자동 정지
    if (motorsOn && (millis() - lastCmdMs > WATCHDOG_MS)) {
        stopAll();
        Serial.println("[WD] Watchdog triggered: motors stopped");
    }

    // ── WebSocket 클라이언트 정리 (메모리 누수 방지) ──────
    ws.cleanupClients();

    delay(10);
}
