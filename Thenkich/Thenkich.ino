ESP32:
// === LIBRARIES ===
//
#include <TaskScheduler.h>
#include <ESP32Servo.h>
#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <esp_system.h>

//
// === WIFI CONFIG ===
//  (Maskirati vjerodajnice u objavljenoj verziji)
//
const char* ssid = "<SSID>";
const char* password = "<PASSWORD>";
bool systemStarted = false;   // Kapija: automatika aktivna/stop

//
// === SERVER / SCHEDULER ===
//
AsyncWebServer server(80);
AsyncWebSocket ws("/ws");     // WebSocket za dvosmjernu poruku "kut,udaljenost,PIR" i komande
Scheduler runner;             // TaskScheduler za kooperativno izvršavanje

//
// === PINS (ESP32) ===
// Mapiranje pinova (ESP32 DevKit v1):
// - Kotači (kontinuirani servoi, 50 Hz): lijevi=GPIO14, desni=GPIO27
// - Radar servo (0–180°): GPIO19
// - HC-SR04: TRIG=GPIO25 (OUTPUT), ECHO=GPIO33 (INPUT-ONLY, 3V3 tolerant)
// - PIR (HC-SR501): GPIO32 (INPUT-ONLY)
//
const int leftWheelPin  = 14;
const int rightWheelPin = 27;
const int radarServoPin = 19;

const int trigPin = 25;
const int echoPin = 33; // INPUT-ONLY (spustiti 5 V ECHO na 3V3!)
const int pirPin  = 32; // INPUT-ONLY

//
// === SERVOS ===
//
Servo leftWheel, rightWheel, radar;

//
// === STATE & PRAGOVI ===
//
bool  motionDetected = false;  // PIR status
float distanceCm     = 0;      // zadnje izmjerena udaljenost (cm)
int   radarAngle     = 0;      // 0..180 (servo kut)
int   actualAngle    = 0;      // 10..170 (za UI prikaz)
int   radarDir       = 1;      // +1 ili -1 (smjer rada skeniranja)

// mjerenje spremno? (štiti logiku od reakcije bez valjanih podataka)
bool  haveMeasure = false;

// TURNING & post-turn lock (sprječava "drhtanje" odmah nakon skretanja)
unsigned long turnStartTime = 0;
const unsigned long TURN_TIME   = 300;  // ms trajanje skretanja (burst)
const unsigned long FWD_LOCK_MS = 400;  // ms forsiraj ravno nakon skretanja
unsigned long afterTurnForwardUntil = 0;

//
// === HTML UI ===
// Ugrađeno sučelje: radar canvas + gumbi (Start/Reset/Uslikaj).
// "Uslikaj" otvara ESP32-CAM IP (po potrebi promijeniti IP adresu).
//
const char index_html[] PROGMEM = R"HTML(
<!DOCTYPE html><html><head><meta charset="UTF-8"><title>Radar</title>
<meta name="viewport" content="width=device-width,initial-scale=1" />
<style>
 body{background:#000;margin:0;display:flex;flex-direction:column;align-items:center;justify-content:center;min-height:100vh;font-family:Arial,Helvetica,sans-serif}
 canvas{background:#000;border:2px solid lime;box-shadow:0 0 20px lime;max-width:92vw;height:auto}
 .row{display:flex;gap:10px;margin:10px 0}
 button{padding:10px 20px;background:#000;color:lime;border:2px solid lime;font-size:16px;cursor:pointer;box-shadow:0 0 10px lime}
 button:hover{background:lime;color:#000}
</style></head><body>
<div class="row">
  <button id="start-btn">Start</button>
  <button id="get-still">Uslikaj</button>
  <button id="reset-btn">Reset</button>
</div>
<canvas id="radar" width="700" height="700"></canvas>
<script>
let canvas=document.getElementById("radar"),ctx=canvas.getContext("2d");
let trail=document.createElement("canvas"); trail.width=canvas.width; trail.height=canvas.height;
let tctx=trail.getContext("2d");
let lastAngle=null, sweepDir=1, socket;

function connect(){
  socket=new WebSocket("ws://"+location.host+"/ws");
  socket.onmessage=(e)=>{
    let[a,d,m]=e.data.split(","),A=parseInt(a),D=parseInt(d),M=(m==="1");
    drawRadar(A,D,M);
  };
  socket.onclose=()=>setTimeout(connect,2000);
}
connect();

function drawGrid(){
  ctx.save(); ctx.translate(canvas.width/2,canvas.height/2);
  ctx.strokeStyle="rgba(0,255,0,0.2)"; ctx.lineWidth=1;
  for(let r=50;r<=canvas.width/2;r+=50){ctx.beginPath();ctx.arc(0,0,r,0,2*Math.PI);ctx.stroke();}
  for(let i=0;i<360;i+=30){
    let rad=i*Math.PI/180, x=Math.cos(rad)*(canvas.width/2), y=Math.sin(rad)*(canvas.height/2);
    ctx.beginPath();ctx.moveTo(0,0);ctx.lineTo(x,y);ctx.stroke();
  }
  ctx.restore();
}
function drawRadar(angle,dist,motion){
  if(lastAngle!==null){let nd=angle>lastAngle?1:-1; if(nd!==sweepDir){sweepDir=nd; tctx.clearRect(0,0,canvas.width,canvas.height);}}
  lastAngle=angle;
  tctx.save(); tctx.translate(canvas.width/2,canvas.height/2); tctx.strokeStyle="lime"; tctx.lineWidth=2;
  let rad=-angle*Math.PI/180, scale=6, x=dist*scale*Math.cos(rad), y=dist*scale*Math.sin(rad);
  tctx.beginPath(); tctx.moveTo(0,0); tctx.lineTo(x,y); tctx.stroke();
  if(dist>0 && dist<50){
    tctx.fillStyle=motion?"yellow":"red";
    tctx.beginPath(); tctx.arc(x,y,5,0,2*Math.PI); tctx.fill();
  }
  tctx.restore();
  ctx.clearRect(0,0,canvas.width,canvas.height); ctx.drawImage(trail,0,0); drawGrid();
}
document.getElementById("get-still").onclick=()=>window.open("http://192.168.1.90/","_blank"); // TODO: prilagodi IP
document.getElementById("reset-btn").onclick=()=>socket&&socket.readyState===1&&socket.send("reset");
document.getElementById("start-btn").onclick=()=>socket&&socket.readyState===1&&socket.send("start");
</script></body></html>
)HTML";

//
// === TASKOVI (TaskScheduler) ===
// Dva zadatka u pozadini (kooperativno):
// - tDistance (~120 ms): TRIG/ECHO mjerenje, PIR očitanje, sweep radara, slanje WebSocket poruke "kut,udaljenost,PIR"
// - tMovement (~100 ms): stroj stanja kretanja (FWD/TURNING) + odluke prema distance/PIR/kutu
//
void measureDistance();
Task tDistance(120, TASK_FOREVER, &measureDistance);
void movementLogic();
Task tMovement(100, TASK_FOREVER, &movementLogic);

//
// === KRETANJE: pomoćne funkcije ===
// Servo write(°) kao "gas/kočnica" za kontinuirane servoe.
// Vrijednosti (70..110) su kalibrirane za tvoj model.
//
enum MovementState { IDLE, TURNING, MOVING_FORWARD };
MovementState movementState = IDLE;

void moveForwards(){ leftWheel.write(110); rightWheel.write(70);  Serial.println("[MOVE] FWD"); }
void moveBackwards(){leftWheel.write(70);  rightWheel.write(110); Serial.println("[MOVE] BACK");}
void moveLeft(){     leftWheel.write(75);  rightWheel.write(75);  Serial.println("[MOVE] LEFT");}
void moveRight(){    leftWheel.write(105); rightWheel.write(105); Serial.println("[MOVE] RIGHT");}
void stopMovement(){ leftWheel.write(90);  rightWheel.write(90);  Serial.println("[MOVE] STOP"); }

//
// === measureDistance() ===
// - Ako systemStarted=false → preskoči
// - Okidanje TRIG (10 µs), mjerenje ECHO s timeout=25 ms (~4.3 m)
// - Ako duration>0 → distanceCm = duration * 0.0343 / 2; PIR očitanje; haveMeasure=true
// - Sweep radara 0↔180°, zapis servo kuta; svakih 3° pošalji "kut,udaljenost,PIR"
//
void measureDistance() {
  if (!systemStarted) return;

  digitalWrite(trigPin, LOW); delayMicroseconds(2);
  digitalWrite(trigPin, HIGH); delayMicroseconds(10);
  digitalWrite(trigPin, LOW);

  long duration = pulseIn(echoPin, HIGH, 25000); // max 25 ms ~ 4.3 m

  if (duration > 0) {                 // valjano mjerenje
    haveMeasure   = true;
    distanceCm    = duration * 0.0343f / 2.0f;  // 0..~400 cm (idealno)
    motionDetected= digitalRead(pirPin);
  } else {
    haveMeasure = false;              // spriječi reakcije bez svježeg mjerenja
  }

  // radarski sweep 0↔180°
  radarAngle += radarDir;
  if (radarAngle >= 180) { radarAngle = 180; radarDir = -1; }
  else if (radarAngle <= 0) { radarAngle = 0; radarDir = 1; }

  radar.write(radarAngle);
  actualAngle = map(radarAngle, 0, 180, 0, 160) + 10; // prikaz 10..170° u UI

  // throttling slanja prema klijentima (svaka ~3°)
  if (radarAngle % 3 == 0) {
    String msg = String(actualAngle) + "," + String((int)distanceCm) + "," + String(motionDetected);
    ws.textAll(msg);
  }
}

//
// === movementLogic() ===
// - State machine: IDLE / TURNING / MOVING_FORWARD
// - TURNING traje TURN_TIME, zatim kratki lock FWD_LOCK_MS (izbjegava titranje)
// - Ako PIR=1 i distance<50 → poravnaj (L/D prema kutu), stop na <=20 cm i
//   postavi systemStarted=false (korisnička kapija: Uslikaj/Start/Reset)
// - Ako PIR=0 i vrlo blizu prepreka (<=20 cm) → jedan "burst" udesno (osnovno izbjegavanje)
// - Inače: default naprijed
//
void movementLogic() {
  if (!systemStarted) return;

  // Dok skrećemo – ništa drugo
  if (movementState == TURNING) {
    if (millis() - turnStartTime >= TURN_TIME) {
      stopMovement(); delay(100);
      moveForwards();
      movementState = MOVING_FORWARD;
      afterTurnForwardUntil = millis() + FWD_LOCK_MS; // kratko forsiraj ravno
    }
    return;
  }

  // Bez prvog mjerenja -> ne reagiraj (sprječava "desno" nakon start/reset)
  if (!haveMeasure) return;

  // Odmah nakon skretanja, ignoriraj prepreke kratko (lock)
  if (millis() < afterTurnForwardUntil) return;

  // META → prilazi i dotjeraj smjer
  if (motionDetected && distanceCm < 50) {
    if (distanceCm <= 20) {
      stopMovement();
      int sendAngle = map(radarAngle, 0, 180, 10, 170);
      String lastMsg = String(sendAngle) + "," + String((int)distanceCm) + ",1";
      ws.textAll(lastMsg);
      systemStarted = false;  // Kapija: UI (Uslikaj/Start/Reset)
      Serial.println("[LOGIC] Target reached → waiting for user");
      return;
    }
    if (radarAngle < 80)  { moveRight(); movementState = TURNING; turnStartTime = millis(); return; }
    if (radarAngle > 100) { moveLeft();  movementState = TURNING; turnStartTime = millis(); return; }
    moveForwards(); movementState = MOVING_FORWARD; return;
  }

  // PREPREKA blizu (bez PIR) → jedno skretanje desno
  if (!motionDetected && distanceCm > 0 && distanceCm <= 20) {
    moveRight(); movementState = TURNING; turnStartTime = millis();
    Serial.println("[LOGIC] Obstacle → right");
    return;
  }

  // DEFAULT → naprijed
  if (movementState != MOVING_FORWARD) {
    moveForwards();
    movementState = MOVING_FORWARD;
  }
}

//
// === setup() ===
// - Serijska dijagnostika + ispis reset razloga
// - PWM timere (4×) i servo period 50 Hz; attach servo pinova
// - PinMode za TRIG/ECHO/PIR
// - Wi-Fi spajanje, WebSocket onEvent (obrada "start"/"reset")
// - Pokretanje WebServera (HTML UI)
// - Inicijalizacija TaskScheduler-a i enable zadataka
//
void setup() {
  Serial.begin(115200);
  Serial.printf("[BOOT] Reset reason: %d\n", esp_reset_reason());

  ESP32PWM::allocateTimer(0);
  ESP32PWM::allocateTimer(1);
  ESP32PWM::allocateTimer(2);
  ESP32PWM::allocateTimer(3);
  leftWheel.setPeriodHertz(50);
  rightWheel.setPeriodHertz(50);
  radar.setPeriodHertz(50);
  leftWheel.attach(leftWheelPin);
  rightWheel.attach(rightWheelPin);
  radar.attach(radarServoPin);

  pinMode(trigPin, OUTPUT);
  pinMode(echoPin, INPUT);
  pinMode(pirPin, INPUT);

  WiFi.begin(ssid, password);
  Serial.print("[WiFi] Connecting");
  while (WiFi.status() != WL_CONNECTED) { delay(400); Serial.print("."); }
  Serial.print("\n[WiFi] IP: "); Serial.println(WiFi.localIP());

  // WebSocket event handler: "reset"/"start" komande
  ws.onEvent([](AsyncWebSocket * server, AsyncWebSocketClient * client, AwsEventType type,
                void * arg, uint8_t * data, size_t len) {
    if (type == WS_EVT_CONNECT) {
      Serial.println("[WS] client connected");
    } else if (type == WS_EVT_DISCONNECT) {
      Serial.println("[WS] client disconnected");
    } else if (type == WS_EVT_DATA) {
      String msg; msg.reserve(len);
      for (size_t i=0;i<len;i++) msg += (char)data[i];
      Serial.printf("[WS] msg: %s\n", msg.c_str());
      if (msg == "reset") {
        moveRight(); delay(600); stopMovement();
        haveMeasure = false;            // čekaj prvo mjerenje poslije reseta
        systemStarted = true;
        Serial.println("[WS] reset done");
      } else if (msg == "start") {
        haveMeasure = false;            // spriječi desno prije prvog mjerenja
        systemStarted = true;
        Serial.println("[WS] start");
      }
    }
  });

  server.addHandler(&ws);
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send_P(200, "text/html", index_html);
  });
  server.begin();

  stopMovement();
  runner.init();
  runner.addTask(tDistance);  tDistance.enable();
  runner.addTask(tMovement);  tMovement.enable();
}

//
// === loop() ===
// Kooperativno izvršavanje zadataka (TaskScheduler).
//
void loop() {
  runner.execute();
}
