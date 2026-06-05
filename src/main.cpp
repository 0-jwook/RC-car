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
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ESPAsyncWebServer.h>
#include <AsyncTCP.h>
#include <ArduinoJson.h>
#include "web_ui.h"

// =============================================================
// [사용자 설정] Wi-Fi
// =============================================================
// true  → AP 모드: ESP32가 직접 핫스팟 생성 (인터넷 불필요, 권장)
// false → STA 모드: 기존 공유기에 연결
#define WIFI_AP_MODE   false

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
// [사용자 설정] 폴링 설정
// =============================================================
#define POLL_URL  "https://nota.mieung.kr/direction"
#define POLL_MS   150    // 폴링 간격 (ms), 낮출수록 반응 빠름

// =============================================================
// [사용자 설정] 가감속 (10ms 업데이트 주기 기준)
// 값이 클수록 변화가 빠름 (255 기준)
// 예: ACCEL_STEP=8 → 0→최대 약 320ms
//     DECEL_STEP=16 → 최대→0 약 160ms
// =============================================================
#define ACCEL_STEP   8    // 가속 스텝
#define DECEL_STEP   16   // 감속 스텝 (가속보다 빠르게 멈춤)

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

// 채널별 DIR 핀 / 반전 배열 (updateMotors 루프에서 사용)
static const uint8_t kDirPin[4] = {M1_DIR_PIN, M2_DIR_PIN, M3_DIR_PIN, M4_DIR_PIN};
static const bool    kInvert[4] = {INVERT_M1,  INVERT_M2,  INVERT_M3,  INVERT_M4};

// =============================================================
// 전역 객체 / 변수
// =============================================================
AsyncWebServer server(80);
AsyncWebSocket ws("/ws");

volatile unsigned long lastCmdMs   = 0;
volatile bool          motorsOn    = false;

float targetV[4]  = {0, 0, 0, 0};   // 목표 출력 (-255~+255)
float currentV[4] = {0, 0, 0, 0};   // 현재 출력 (가감속 적용)
unsigned long lastMotorMs = 0;

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
// 전체 모터 즉시 정지 (안전 이벤트: 연결 끊김, 워치독)
// =============================================================
void stopAll() {
    for (int i = 0; i < 4; i++) { targetV[i] = 0; currentV[i] = 0; }
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

    // 3. 속도 스케일 적용 → targetV[] 에 저장 (실제 출력은 updateMotors()가 담당)
    float scale = (speed / 100.0f) * 255.0f / maxAbs;
    for (int i = 0; i < 4; i++) {
        targetV[i] = raw[i] * scale;
    }
    motorsOn = true;
}

// =============================================================
// 가감속 적용 후 모터 출력 (loop()에서 10ms마다 호출)
//
// currentV → targetV 방향으로 스텝씩 이동:
//   속력이 커지는 방향 → ACCEL_STEP
//   속력이 작아지는 방향 → DECEL_STEP (감속이 더 빠름)
// =============================================================
void updateMotors() {
    bool anyNonZero = false;

    for (int i = 0; i < 4; i++) {
        float diff    = targetV[i] - currentV[i];
        float absDiff = fabsf(diff);

        if (absDiff < 0.5f) {
            currentV[i] = targetV[i];   // 목표에 충분히 가까우면 즉시 고정
        } else {
            // 현재 속력보다 목표 속력이 크면 가속, 아니면 감속
            float rate = (fabsf(targetV[i]) >= fabsf(currentV[i]))
                         ? (float)ACCEL_STEP
                         : (float)DECEL_STEP;
            currentV[i] += (diff > 0) ? rate : -rate;
        }

        setMotor(i, kDirPin[i], (int)currentV[i], kInvert[i]);
        if ((int)currentV[i] != 0) anyNonZero = true;
    }

    motorsOn = anyNonZero;
}

// =============================================================
// 방향 문자열 → (vx, vy, w) 변환
//
// 지원 명령어 (대소문자 구분):
//   "F"   전진          "B"   후진
//   "L"   좌 스트레이프  "R"   우 스트레이프
//   "FL"  전진+좌대각    "FR"  전진+우대각
//   "BL"  후진+좌대각    "BR"  후진+우대각
//   "CW"  시계방향 회전  "CCW" 반시계방향 회전
//   "S"   정지
// =============================================================
bool dirToVector(const char* dir, float &vx, float &vy, float &w) {
    vx = 0; vy = 0; w = 0;
    // 팀원 프로토콜 (nota.mieung.kr)
    if      (strcmp(dir, "up")         == 0) {          vy= 1;  }
    else if (strcmp(dir, "down")       == 0) {          vy=-1;  }
    else if (strcmp(dir, "left")       == 0) { vx=-1;           }
    else if (strcmp(dir, "right")      == 0) { vx= 1;           }
    else if (strcmp(dir, "up-left")    == 0) { vx=-1; vy= 1;    }
    else if (strcmp(dir, "up-right")   == 0) { vx= 1; vy= 1;    }
    else if (strcmp(dir, "down-left")  == 0) { vx=-1; vy=-1;    }
    else if (strcmp(dir, "down-right") == 0) { vx= 1; vy=-1;    }
    else if (strcmp(dir, "center")     == 0) { /* 정지 */        }
    // 기존 단축 명령 (하위 호환)
    else if (strcmp(dir, "CW")  == 0) {               w= 1;     }
    else if (strcmp(dir, "CCW") == 0) {               w=-1;     }
    else if (strcmp(dir, "S")   == 0) { /* 모두 0 */             }
    else return false;
    return true;
}

// =============================================================
// WebSocket 이벤트 핸들러
//
// 두 가지 프로토콜을 동시 지원:
//
// [1] 방향 명령 (팀원용 단순 프로토콜)
//     {"dir": "F"}                    ← 기본 속도(80%)로 전진
//     {"dir": "FL", "speed": 60}      ← 속도 지정 가능
//     또는 JSON 없이 문자열만: "F"
//
// [2] 벡터 명령 (웹 UI / 정밀 제어)
//     {"vx": 0.5, "vy": 0.8, "w": 0.0, "speed": 80}
//
// ※ 워치독(500ms): 명령을 계속 보내야 동작 유지됨.
//    정지시키려면 "S" 또는 연결을 끊으면 됨.
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
            stopAll();
            break;

        case WS_EVT_DATA: {
            AwsFrameInfo* info = (AwsFrameInfo*)arg;
            if (info->final && info->index == 0 &&
                info->len == len && info->opcode == WS_TEXT)
            {
                if (len == 0 || len >= 128) return;

                char buf[128];
                memcpy(buf, data, len);
                buf[len] = '\0';

                float vx = 0, vy = 0, w = 0, speed = 50.0f;

                StaticJsonDocument<128> doc;
                DeserializationError err = deserializeJson(doc, buf);

                if (!err && doc.containsKey("direction")) {
                    // ── [1] 방향 명령 JSON (팀원 프로토콜) ──────
                    const char* dir = doc["direction"] | "S";
                    if (!dirToVector(dir, vx, vy, w)) return;
                    speed = constrain((float)(doc["speed"] | 50.0f), 0.0f, 100.0f);
                    Serial.printf("[CMD] direction=%s  speed=%.0f\n", dir, speed);

                } else if (!err && doc.containsKey("vx")) {
                    // ── [2] 벡터 명령 JSON (웹 UI) ────────────
                    vx    = constrain((float)(doc["vx"]    | 0.0f), -1.0f, 1.0f);
                    vy    = constrain((float)(doc["vy"]    | 0.0f), -1.0f, 1.0f);
                    w     = constrain((float)(doc["w"]     | 0.0f), -1.0f, 1.0f);
                    speed = constrain((float)(doc["speed"] | 50.0f), 0.0f, 100.0f);
                    Serial.printf("[WS]  vx=%.2f  vy=%.2f  w=%.2f  speed=%.0f\n", vx, vy, w, speed);

                } else {
                    // ── [3] 평문 방향 문자열: "F", "BL" 등 ────
                    if (!dirToVector(buf, vx, vy, w)) return;
                    Serial.printf("[WS]  plain=%s\n", buf);
                }

                driveRobot(vx, vy, w, speed);
                lastCmdMs = millis();
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
// nota.mieung.kr/direction 폴링 태스크
// loop()를 막지 않도록 FreeRTOS 별도 태스크로 실행
// =============================================================
void pollTask(void* param) {
    WiFiClientSecure client;
    client.setInsecure();   // 인증서 검증 생략 (HTTPS 사용)

    for (;;) {
        if (WiFi.status() == WL_CONNECTED) {
            HTTPClient http;
            if (http.begin(client, POLL_URL)) {
                http.setTimeout(400);
                int code = http.GET();

                if (code == HTTP_CODE_OK) {
                    String body = http.getString();

                    StaticJsonDocument<128> doc;
                    if (deserializeJson(doc, body) == DeserializationError::Ok &&
                        doc.containsKey("direction"))
                    {
                        const char* dir = doc["direction"] | "center";
                        float vx, vy, w;
                        if (dirToVector(dir, vx, vy, w)) {
                            driveRobot(vx, vy, w, 50.0f);
                            lastCmdMs = millis();
                        }
                        Serial.printf("[POLL] direction=%s\n", dir);
                    }
                } else {
                    Serial.printf("[POLL] HTTP error: %d\n", code);
                }
                http.end();
            }
        } else {
            Serial.println("[POLL] WiFi 연결 끊김 - 재연결 대기중");
        }

        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
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
        Serial.println("--------------------------------------------");
        Serial.println("[WiFi] AP Mode - 핫스팟 시작됨");
        Serial.printf("  SSID     : %s\n", AP_SSID);
        Serial.printf("  Password : %s\n", AP_PASSWORD);
        Serial.printf("  IP Addr  : %s\n", WiFi.softAPIP().toString().c_str());
        Serial.println("  브라우저에서 위 IP 주소로 접속하세요");
        Serial.println("--------------------------------------------");

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
        Serial.println("--------------------------------------------");
        Serial.println("[WiFi] STA Mode - 연결 성공!");
        Serial.printf("  IP Addr  : %s\n", WiFi.localIP().toString().c_str());
        Serial.println("  브라우저에서 위 IP 주소로 접속하세요");
        Serial.println("--------------------------------------------");
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
    Serial.println("[HTTP] Web server started");

    // ── nota.mieung.kr 폴링 태스크 시작 (Core 0, WiFi와 같은 코어) ──
    xTaskCreatePinnedToCore(pollTask, "poll", 8192, nullptr, 1, nullptr, 0);
    Serial.printf("[POLL] 폴링 시작: %s  간격: %dms\n", POLL_URL, POLL_MS);
}

// =============================================================
// loop()
// =============================================================
void loop() {
    unsigned long now = millis();

    // ── 가감속 모터 업데이트 (100 Hz) ───────────────────────
    if (now - lastMotorMs >= 10) {
        lastMotorMs = now;
        updateMotors();
    }

    // ── 워치독 ──────────────────────────────────────────────
    if (motorsOn && (now - lastCmdMs > WATCHDOG_MS)) {
        stopAll();
        Serial.println("[WD] Watchdog triggered: motors stopped");
    }

    // ── WebSocket 클라이언트 정리 ────────────────────────────
    ws.cleanupClients();
}
