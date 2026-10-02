package main

import (
	"bytes"
	"context"
	"encoding/binary"
	"encoding/json"
	"fmt"
	"io"
	"log"
	"math"
	"net/http"
	"os"
	"regexp"
	"strings"
	"sync"
	"sync/atomic"
	"time"
	"unicode"

	"github.com/gorilla/websocket"
	"github.com/hraban/opus"
	"github.com/joho/godotenv"

	// Google Cloud Speech Imports
	speech "cloud.google.com/go/speech/apiv1"
	speechpb "cloud.google.com/go/speech/apiv1/speechpb"
	"google.golang.org/api/option"
)

// --- KONFIGURASI AUDIO ---
const (
	SampleRate      = 16000
	Channels        = 1
	FrameDurationMS = 20
	FrameSize       = SampleRate * FrameDurationMS / 1000 // 320 samples @ 16kHz / 20ms

	// FrameInterval adalah durasi satu frame Opus dalam bentuk time.Duration.
	// Device (ESP32) mengonsumsi audio dari I2S DMA-nya pada 16000Hz, jadi setiap
	// frame 320-sample = tepat 20ms audio. Pengiriman dari bridge HARUS terkunci
	// pada cadence ini agar tidak terjadi underrun (audio patah) atau overflow.
	FrameInterval = FrameDurationMS * time.Millisecond

	// PrebufferFrames: jumlah frame pertama yang dikirim back-to-back (tanpa jeda)
	// untuk mengisi jitter buffer device sebelum playback dimulai. 10 frame = 200ms
	// lead time; cukup untuk menyerap lonjakan latensi jaringan tanpa membuat device
	// menunggu (device queue max = 2400ms, jadi 200ms sangat aman).
	PrebufferFrames = 10

	// SilencePaddingFrames: silence singkat di akhir tiap segmen agar (a) encoder Opus
	// mem-flush lookahead-nya (~6.5ms) sehingga kata terakhir tidak terpotong, dan
	// (b) ada sedikit tail buffer di device. 4 frame = 80ms (dikecilkan dari 8 agar
	// jeda antar-segmen tidak terasa lama).
	SilencePaddingFrames = 4

	// InterSegmentPause: jeda natural antar kalimat/segmen.
	InterSegmentPause = 90 * time.Millisecond

	// VAD Settings
	SilenceThreshold = 150

	// Timeout Settings
	UserIdleTimeout = 10 * time.Second

	// --- Google STT stream lifetime ---
	// Google Streaming Recognize punya hard limit ~305 detik per stream. Stream
	// dirotasi proaktif sebelum limit: saat user tidak sedang bicara di usia 240s,
	// atau dipaksa di 290s bila user terus bicara.
	SttRotateAge      = 240 * time.Second
	SttForceRotateAge = 290 * time.Second

	// Google Cloud Credential File (fallback jika GOOGLE_CREDS_JSON kosong)
	GoogleCredsFile = "avagenc-dev-130e3830892b.json"

	// --- Cartesia TTS Settings ---
	// Model default: sonic-3.5 (kualitas terbaik & terbaru dari keluarga Sonic,
	// mendukung 51 bahasa termasuk Indonesia). Bisa dioverride via env CARTESIA_MODEL.
	DefaultCartesiaModel = "sonic-3.5"
	CartesiaVoiceID      = "b441c4fd-4910-4c55-ae56-f0291057e2cc"
	CartesiaVoiceMode    = "id"
	CartesiaLanguage     = "id" // Bahasa Indonesia (IETF-ish code yang diterima Cartesia)
	CartesiaAPIVersion   = "2026-03-01"

	// PENTING (audio jernih): kita minta Cartesia meng-output PCM langsung pada 16000Hz
	// sehingga TIDAK perlu resample di bridge. Resample 44100->16000 dengan linear
	// interpolation tanpa anti-alias filter menyebabkan aliasing (suara kasar/berdesis).
	// Dengan output native 16000Hz, audio langsung cocok dengan Opus encoder 16kHz.
	CartesiaSampleRate = 16000
)

// CartesiaModel mengembalikan model TTS aktif (env override, default sonic-3.5).
func CartesiaModel() string {
	if m := os.Getenv("CARTESIA_MODEL"); m != "" {
		return m
	}
	return DefaultCartesiaModel
}

// --- EMOTION MAPPING ---
//
// Sumber kebenaran emosi = animasi GIF di device pada folder:
//   managed_components/78__xiaozhi-fonts/gif/avagenc_128
// Device merender GIF berdasarkan NAMA emosi yang dikirim di pesan "llm".emotion.
// Jadi tag emosi yang di-parse dari Main Agent (#emotion#) dan yang kita kirim ke
// device HARUS berasal dari daftar 21 emosi valid berikut. Jika nama tidak dikenal,
// device menampilkan "Emoji not found" (tidak ada animasi).
var validDeviceEmotions = map[string]bool{
	"affectionate": true,
	"annoyed":      true,
	"anxious":      true,
	"bitter":       true,
	"bored":        true,
	"confused":     true,
	"default":      true,
	"devastated":   true,
	"ecstatic":     true,
	"furious":      true,
	"guilty":       true,
	"melancholy":   true,
	"neutral":      true,
	"pouting":      true,
	"relieved":     true,
	"shocked":      true,
	"shy":          true,
	"skeptical":    true,
	"smug":         true,
	"smug_angry":   true,
	"thinking":     true,
}

// normalizeDeviceEmotion memastikan emosi yang dikirim ke device selalu valid.
// Jika tidak dikenal, fallback ke "neutral" (state istirahat).
func normalizeDeviceEmotion(e string) string {
	e = strings.ToLower(strings.TrimSpace(e))
	if validDeviceEmotions[e] {
		return e
	}
	return "neutral"
}

// Cartesia generation_config.emotion mendukung 7 nilai:
//   neutral, calm, content, excited, sad, scared, angry
// Mapping 21 emosi device -> 7 emosi Cartesia berdasarkan kedekatan nuansa,
// agar warna suara TTS selaras dengan animasi wajah yang tampil di device.
var deviceEmotionToCartesia = map[string]string{
	"affectionate": "content", // hangat / penuh kasih
	"annoyed":      "angry",   // kesal
	"anxious":      "scared",  // cemas / gugup
	"bitter":       "angry",   // getir / sinis
	"bored":        "calm",    // bosan / datar
	"confused":     "neutral", // bingung
	"default":      "neutral", // istirahat
	"devastated":   "sad",     // hancur / sangat sedih
	"ecstatic":     "excited", // sangat gembira
	"furious":      "angry",   // murka
	"guilty":       "sad",     // bersalah
	"melancholy":   "sad",     // murung / sendu
	"neutral":      "neutral", // netral
	"pouting":      "sad",     // merajuk / cemberut
	"relieved":     "calm",    // lega
	"shocked":      "excited", // terkejut (intensitas tinggi)
	"shy":          "scared",  // malu / segan (gugup)
	"skeptical":    "neutral", // ragu / skeptis
	"smug":         "content", // puas diri
	"smug_angry":   "angry",   // puas diri + sebal
	"thinking":     "neutral", // berpikir
}

// mapEmotionToCartesia menerjemahkan emosi device -> emosi Cartesia (default neutral).
func mapEmotionToCartesia(deviceEmotion string) string {
	if c, ok := deviceEmotionToCartesia[strings.ToLower(strings.TrimSpace(deviceEmotion))]; ok {
		return c
	}
	return "neutral"
}

// --- STRUCTS ---

// MainAgentRequest adalah body request ke Main Agent (format terbaru).
//
//	REQ_BODY: { "message": "string" }
type MainAgentRequest struct {
	Message string `json:"message"`
}

// MainAgentResponse adalah body response dari Main Agent (format terbaru, flat).
//
//	RESP_BODY: { "response": "string" }
type MainAgentResponse struct {
	Response string `json:"response"`
}

// CartesiaTTSRequest adalah body request ke Cartesia TTS API.
type CartesiaTTSRequest struct {
	ModelID    string            `json:"model_id"`
	Transcript string            `json:"transcript"`
	Language   string            `json:"language,omitempty"`
	Voice      CartesiaVoice     `json:"voice"`
	OutputFmt  CartesiaOutputFmt `json:"output_format"`
	GenConfig  CartesiaGenConfig `json:"generation_config"`
}

type CartesiaVoice struct {
	Mode string `json:"mode"`
	ID   string `json:"id"`
}

type CartesiaOutputFmt struct {
	Container  string `json:"container"`
	Encoding   string `json:"encoding"`
	SampleRate int    `json:"sample_rate"`
}

type CartesiaGenConfig struct {
	Speed   float64 `json:"speed"`
	Volume  float64 `json:"volume"`
	Emotion string  `json:"emotion"`
}

type XiaozhiMessage struct {
	Type      string `json:"type"`
	SessionID string `json:"session_id,omitempty"`
	Text      string `json:"text,omitempty"`
	Emotion   string `json:"emotion,omitempty"`
	State     string `json:"state,omitempty"`
}

type Segment struct {
	Text    string
	Emotion string
}

// --- STATE MACHINE SESSION ---

type Session struct {
	ID      string
	Conn    *websocket.Conn
	Encoder *opus.Encoder
	Decoder *opus.Decoder

	// UserID diambil dari header "X-User-Id" saat WS handshake (device mengirim
	// user_id yang tersimpan hasil pairing lokal — lihat firmware user_pairing).
	// Menggantikan env X_USER_ID yang dulu dipakai global untuk semua sesi.
	UserID string

	IsProcessing bool
	Mutex        sync.Mutex

	LastActivity time.Time
	TimeoutStage int

	IsSpeaking bool

	// --- STT state (semua field di bawah dijaga oleh SttMu) ---
	// Stream STT bersifat PERSISTEN: dibuka saat sesi WS dimulai, di-restart
	// otomatis saat error, dirotasi sebelum limit Google, dan ditutup rapi saat
	// WS putus. STT selalu siap selama koneksi device<->bridge hidup.
	SttMu              sync.Mutex
	Stream             speechpb.Speech_StreamingRecognizeClient
	StreamCancel       context.CancelFunc
	StreamDone         chan struct{}
	StreamGen          int       // generasi stream; goroutine stream lama tidak boleh menimpa state stream baru
	StreamStartedAt    time.Time // untuk rotasi proaktif sebelum limit ~305s
	FinalText          string
	InterimText        string
	LastTextUpdateTime time.Time

	// Dedicated write mutex terpisah dari logic mutex agar streaming audio
	// (WriteMessage) tidak saling blok dengan update state.
	WriteMutex sync.Mutex

	// NR membersihkan audio mic sebelum VAD & Google STT. Noise suppression di
	// device (AFE NS) tidak bisa dipakai — board tidak punya partisi model esp-sr
	// dan crash saat init — jadi pembersihan dilakukan di bridge.
	NR *NoiseReducer

	// AbortPlayback di-set saat device mengirim pesan "abort" (mis. wake word /
	// user menyela). streamAudioToDevice berhenti di frame berikutnya dan sisa
	// segmen di pipeline di-skip. Di-reset saat device siap lagi ("listen" start)
	// dan di awal pipeline baru.
	AbortPlayback atomic.Bool

	// Closed di-set saat sesi WS berakhir; mencegah goroutine lain (mis. tick
	// idle monitor) membuka kembali stream STT setelah cleanup.
	Closed atomic.Bool
}

// --- NOISE REDUCTION (BRIDGE-SIDE) ---
//
// Dua tahap ringan yang aman untuk speech:
//  1. High-pass biquad 120Hz: membuang hum listrik / gemuruh AC-kipas frekuensi
//     rendah yang menaikkan RMS (bikin VAD salah trigger) tanpa menyentuh suara.
//  2. Adaptive noise gate: melacak noise floor ruangan (EMA saat hening) lalu
//     MEREDAM (bukan mute total) frame yang jelas bukan ucapan. Gate membuka
//     SEKETIKA saat ada suara (awal kata tidak terpotong) dan menutup perlahan
//     (tidak terdengar "pumping").
type NoiseReducer struct {
	// state biquad high-pass
	x1, x2, y1, y2     float64
	b0, b1, b2, a1, a2 float64

	noiseFloor float64 // estimasi RMS noise ruangan (EMA lambat)
	gain       float64 // gain gate yang dihaluskan
}

func NewNoiseReducer() *NoiseReducer {
	nr := &NoiseReducer{noiseFloor: 50, gain: 1.0}

	// Koefisien biquad high-pass Butterworth: fc=120Hz, fs=16000, Q=0.707.
	const fc, fs, q = 120.0, 16000.0, 0.7071
	w0 := 2 * math.Pi * fc / fs
	cosw, sinw := math.Cos(w0), math.Sin(w0)
	alpha := sinw / (2 * q)
	a0 := 1 + alpha
	nr.b0 = (1 + cosw) / 2 / a0
	nr.b1 = -(1 + cosw) / a0
	nr.b2 = (1 + cosw) / 2 / a0
	nr.a1 = -2 * cosw / a0
	nr.a2 = (1 - alpha) / a0
	return nr
}

// Process membersihkan satu frame PCM in-place dan mengembalikan RMS-nya
// SETELAH pembersihan (dipakai sebagai sinyal VAD).
func (nr *NoiseReducer) Process(pcm []int16) float64 {
	// Tahap 1: high-pass.
	for i, s := range pcm {
		x := float64(s)
		y := nr.b0*x + nr.b1*nr.x1 + nr.b2*nr.x2 - nr.a1*nr.y1 - nr.a2*nr.y2
		nr.x2, nr.x1 = nr.x1, x
		nr.y2, nr.y1 = nr.y1, y
		switch {
		case y > 32767:
			pcm[i] = 32767
		case y < -32768:
			pcm[i] = -32768
		default:
			pcm[i] = int16(y)
		}
	}

	rms := calculateRMS(pcm)

	// Update noise floor hanya saat frame terdengar seperti noise (dekat floor),
	// supaya ucapan tidak ikut menaikkan estimasi noise.
	if rms < nr.noiseFloor*1.5 {
		nr.noiseFloor = 0.95*nr.noiseFloor + 0.05*math.Max(rms, 1)
	}

	// Tahap 2: gate. Frame di bawah 2x noise floor dianggap noise -> redam -12dB.
	target := 1.0
	if rms < nr.noiseFloor*2.0 {
		target = 0.25
	}
	if target >= nr.gain {
		nr.gain = target // buka seketika: ucapan langsung lolos utuh
	} else {
		nr.gain = 0.85*nr.gain + 0.15*target // tutup perlahan (~60ms)
	}
	if nr.gain < 0.999 {
		for i := range pcm {
			pcm[i] = int16(float64(pcm[i]) * nr.gain)
		}
	}
	return rms
}

// SpeechThreshold mengembalikan ambang VAD adaptif: minimal SilenceThreshold,
// tapi naik otomatis di ruangan berisik (3x noise floor) agar tidak salah trigger.
func (nr *NoiseReducer) SpeechThreshold() float64 {
	return math.Max(SilenceThreshold, nr.noiseFloor*3.0)
}

var upgrader = websocket.Upgrader{
	CheckOrigin: func(r *http.Request) bool { return true },
}

var (
	speechClient         *speech.Client
	CachedWelcomePCM     []int16
	CachedTimeoutWarning []int16
	CachedGoodbye        []int16
	CachedUlangiPCM      []int16

	globalHTTPClient = &http.Client{
		Timeout: 30 * time.Second,
		Transport: &http.Transport{
			MaxIdleConns:        100,
			MaxIdleConnsPerHost: 100,
			IdleConnTimeout:     90 * time.Second,
		},
	}
)

func main() {
	if err := godotenv.Load(); err != nil {
		log.Println("Info: .env not found, using system env")
	}

	if os.Getenv("CARTESIA_API_KEY") == "" {
		log.Fatal("❌ CARTESIA_API_KEY tidak ditemukan di environment variable.")
	}

	ctx := context.Background()
	var err error

	// Autentikasi Google Cloud via FILE JSON service account.
	// Nama file diambil dari env GOOGLE_CREDS_FILE (boleh ditulis tanpa ".json",
	// ekstensi akan ditambahkan otomatis). Default ke GoogleCredsFile.
	credsFile := os.Getenv("GOOGLE_CREDS_FILE")
	if credsFile == "" {
		credsFile = GoogleCredsFile
	}
	if !strings.HasSuffix(strings.ToLower(credsFile), ".json") {
		credsFile += ".json"
	}
	log.Printf("🔑 Mengautentikasi Google Cloud dengan file: %s", credsFile)

	speechClient, err = speech.NewClient(ctx, option.WithCredentialsFile(credsFile))
	if err != nil {
		log.Fatalf("❌ Gagal init Google Speech Client: %v", err)
	}
	defer speechClient.Close()

	log.Printf("⚡ Generating Static Audio Cache using Cartesia (%s)...", CartesiaModel())

	CachedWelcomePCM, err = callCartesiaTTS("Halo bos, ada yang bisa dibantu?", "ecstatic")
	if err != nil {
		log.Printf("⚠️ Warning: Failed to cache welcome msg: %v", err)
	}

	CachedTimeoutWarning, _ = callCartesiaTTS("Apakah ada hal lain yang bisa saya bantu bos?", "affectionate")
	CachedGoodbye, _ = callCartesiaTTS("Sampai jumpa lagi bos.", "melancholy")
	CachedUlangiPCM, _ = callCartesiaTTS("Oke bos, silakan diulang.", "relieved")

	log.Println("✅ Audio Cache Ready!")

	port := os.Getenv("PORT")
	if port == "" {
		port = "8081" // sesuai konfigurasi WebSocket device (ws://<host>:8081/ws)
	}

	http.HandleFunc("/health", func(w http.ResponseWriter, r *http.Request) {
		w.WriteHeader(http.StatusOK)
		w.Write([]byte("OK"))
	})

	http.HandleFunc("/ws", handleWebSocket)

	log.Printf("🚀 Oynox Server Started on port %s", port)
	log.Printf("🤖 Mode: Xiaozhi | STT: Google Streaming (Realtime) | TTS: Cartesia %s @ %dHz raw",
		CartesiaModel(), CartesiaSampleRate)

	if err := http.ListenAndServe(":"+port, nil); err != nil {
		log.Fatal(err)
	}
}

func handleWebSocket(w http.ResponseWriter, r *http.Request) {
	// Safety net: panic pada satu sesi tidak boleh mematikan seluruh server.
	defer func() {
		if rec := recover(); rec != nil {
			log.Printf("🔥 PANIC recovered in session handler: %v", rec)
		}
	}()

	// Ambil identitas user dari header handshake SEBELUM upgrade (device mengirim
	// X-User-Id hasil pairing lokal). Header hanya tersedia di request HTTP awal.
	userID := r.Header.Get("X-User-Id")

	conn, err := upgrader.Upgrade(w, r, nil)
	if err != nil {
		log.Println("❌ Upgrade Error:", err)
		return
	}
	defer conn.Close()

	if userID == "" {
		log.Printf("⚠️ WS handshake tanpa X-User-Id (device belum ter-pairing?) dari %s", conn.RemoteAddr())
	}
	log.Printf("🔗 New WebSocket connection from %s (user=%q)", conn.RemoteAddr(), userID)

	// Encoder Opus 16kHz mono, mode VoIP (dioptimasi untuk suara).
	enc, err := opus.NewEncoder(SampleRate, Channels, opus.AppVoIP)
	if err != nil {
		log.Println("❌ Failed to create Opus encoder:", err)
		return
	}
	// Kualitas ("jernih"): bitrate 32kbps sudah transparan untuk speech wideband 16kHz,
	// dan complexity 10 memberi kualitas encode terbaik (CPU server murah).
	// Transport WebSocket berjalan di atas TCP sehingga tidak ada packet loss di layer
	// Opus (paket dijamin terkirim urut) — smoothness ditangani oleh pacing + prebuffer,
	// bukan oleh FEC.
	enc.SetBitrate(32000)
	enc.SetComplexity(10)

	dec, err := opus.NewDecoder(SampleRate, Channels)
	if err != nil {
		log.Println("❌ Failed to create Opus decoder:", err)
		return
	}

	sessionID := fmt.Sprintf("sess-%d", time.Now().Unix())

	session := &Session{
		ID:           sessionID,
		Conn:         conn,
		Encoder:      enc,
		Decoder:      dec,
		UserID:       userID,
		LastActivity: time.Now(),
		TimeoutStage: 0,
		NR:           NewNoiseReducer(),
	}

	log.Printf("✅ [%s] Session established (client %s)", session.ID, conn.RemoteAddr())
	defer log.Printf("🔚 [%s] Session ended", session.ID)

	handleHandshake(session)

	// STT dinyalakan SEGERA setelah handshake dan dijaga hidup selama koneksi WS
	// ada. Cleanup di defer WAJIB: stream gRPC yang bocor saat WS diputus paksa
	// akan menumpuk sampai limit concurrent-stream HTTP/2, membuat
	// StreamingRecognize berikutnya menggantung ("STT mati sendiri").
	if err := session.openGoogleStream(); err != nil {
		log.Printf("❌ [%s] Failed to open initial Google STT stream: %v", session.ID, err)
	}
	defer session.closeGoogleStream()

	if len(CachedWelcomePCM) > 0 {
		log.Printf("👋 [%s] Playing Welcome Message...", session.ID)
		session.SendJSON("llm", "Halo bos, ada yang bisa dibantu?", "ecstatic")
		streamAudioToDevice(session, CachedWelcomePCM)
		session.SendJSON("llm", "", "neutral")

		session.Mutex.Lock()
		session.LastActivity = time.Now()
		session.Mutex.Unlock()
	}

	stopMonitor := make(chan bool)
	go monitorIdleTimeout(session, stopMonitor)
	defer close(stopMonitor)

	for {
		mt, message, err := conn.ReadMessage()
		if err != nil {
			if websocket.IsCloseError(err, websocket.CloseNormalClosure, websocket.CloseGoingAway) {
				log.Printf("🔌 [%s] Client disconnected normally", session.ID)
			} else {
				log.Printf("⚠️ [%s] Connection closed: %v", session.ID, err)
			}
			break
		}

		// Log & tangani pesan kontrol (JSON) dari device — mis. hello/listen/abort.
		if mt == websocket.TextMessage {
			log.Printf("📥 [%s] Device text: %s", session.ID, string(message))

			var ctrl XiaozhiMessage
			if json.Unmarshal(message, &ctrl) == nil {
				switch ctrl.Type {
				case "abort":
					// Device minta berhenti bicara (wake word / disela user):
					// hentikan streaming audio yang sedang berjalan + sisa segmen.
					log.Printf("🛑 [%s] Device abort (%s) — stopping playback", session.ID, ctrl.State)
					session.AbortPlayback.Store(true)
				case "listen":
					// Device siap mendengar lagi — izinkan playback berikutnya.
					session.AbortPlayback.Store(false)
				}
			}
		}

		session.Mutex.Lock()
		session.LastActivity = time.Now()
		session.TimeoutStage = 0
		session.Mutex.Unlock()

		session.Mutex.Lock()
		if session.IsProcessing {
			session.Mutex.Unlock()
			continue
		}
		session.Mutex.Unlock()

		if mt == websocket.BinaryMessage {
			pcmBuffer := make([]int16, 1000)
			n, err := session.Decoder.Decode(message, pcmBuffer)
			if err != nil {
				continue
			}
			pcmData := pcmBuffer[:n]

			// Noise reduction dulu (high-pass + adaptive gate), baru feed ke STT.
			// rms yang dikembalikan adalah level SETELAH pembersihan.
			rms := session.NR.Process(pcmData)

			// Pastikan stream STT hidup (auto-restart bila mati, rotasi bila tua),
			// lalu feed SEMUA audio mic ke Google. Stream persisten berarti awal
			// kata tidak terpotong menunggu trigger VAD, dan STT selalu siap.
			session.ensureGoogleStream()
			session.sendAudioToGoogle(pcmData)

			if rms > session.NR.SpeechThreshold() && !session.IsSpeaking {
				session.Mutex.Lock()
				session.IsSpeaking = true
				session.Mutex.Unlock()

				session.SttMu.Lock()
				session.LastTextUpdateTime = time.Now()
				session.SttMu.Unlock()

				log.Printf("🎤 [%s] Speaking detected (rms=%.0f)", session.ID, rms)
			}

			if session.IsSpeaking {
				session.SttMu.Lock()
				timeSinceTextUpdate := time.Since(session.LastTextUpdateTime)
				textLen := len(strings.TrimSpace(session.FinalText + session.InterimText))
				session.SttMu.Unlock()

				if (textLen > 0 && timeSinceTextUpdate > 1500*time.Millisecond) ||
					(textLen == 0 && timeSinceTextUpdate > 4000*time.Millisecond) {

					log.Println("🛑 STT Stabil (Teks berhenti). Harvesting result...")

					session.Mutex.Lock()
					session.IsProcessing = true
					session.IsSpeaking = false
					session.LastActivity = time.Now()
					session.Mutex.Unlock()

					finalText := session.harvestSttResult()

					go processPipeline(session, finalText)
				}
			}
		}
	}
}

// --- GOOGLE STT STREAMING (PERSISTEN) ---
//
// Prinsip: selama koneksi WS device<->bridge hidup, stream STT ke Google harus
// SELALU siap. Stream dibuka saat sesi dimulai, di-restart otomatis saat error
// (mis. koneksi diputus paksa oleh user / limit Google tersentuh), dirotasi
// proaktif sebelum hard limit ~305s, dan ditutup rapi saat WS putus. Tanpa
// cleanup, stream gRPC bocor menumpuk sampai limit concurrent-stream HTTP/2 dan
// StreamingRecognize berikutnya menggantung — gejala "STT mati sendiri".

// sttStreamingConfig membangun request konfigurasi awal untuk satu stream STT.
func sttStreamingConfig() *speechpb.StreamingRecognizeRequest {
	return &speechpb.StreamingRecognizeRequest{
		StreamingRequest: &speechpb.StreamingRecognizeRequest_StreamingConfig{
			StreamingConfig: &speechpb.StreamingRecognitionConfig{
				Config: &speechpb.RecognitionConfig{
					Encoding:                   speechpb.RecognitionConfig_LINEAR16,
					SampleRateHertz:            16000,
					LanguageCode:               "id-ID",
					EnableAutomaticPunctuation: true,
					UseEnhanced:                true,
					Model:                      "latest_short",
					SpeechContexts: []*speechpb.SpeechContext{
						{
							Phrases: []string{
								"bos", "oke", "iya", "nggak", "enggak", "gimana",
								"tolong", "bantu", "ulangi", "ulang", "halo",
								"terima kasih", "makasih", "sip", "siap",
							},
							Boost: 15.0,
						},
					},
				},
				InterimResults: true,
			},
		},
	}
}

// openGoogleStream membuka stream STT baru (thread-safe).
func (s *Session) openGoogleStream() error {
	s.SttMu.Lock()
	defer s.SttMu.Unlock()
	return s.openGoogleStreamLocked()
}

// openGoogleStreamLocked membuka stream STT baru. HARUS dipanggil dengan SttMu
// sudah dipegang. Stream lama (jika ada) ditutup lebih dulu. FinalText/InterimText
// TIDAK di-reset di sini — reset hanya terjadi saat harvest, sehingga rotasi
// stream di tengah percakapan tidak menghilangkan transkrip yang sudah terkumpul.
func (s *Session) openGoogleStreamLocked() error {
	s.closeStreamLocked()

	ctx, cancel := context.WithCancel(context.Background())
	stream, err := speechClient.StreamingRecognize(ctx)
	if err != nil {
		cancel()
		return fmt.Errorf("open stream: %w", err)
	}
	if err := stream.Send(sttStreamingConfig()); err != nil {
		cancel()
		return fmt.Errorf("send config: %w", err)
	}

	s.StreamGen++
	gen := s.StreamGen
	done := make(chan struct{})

	s.Stream = stream
	s.StreamCancel = cancel
	s.StreamDone = done
	s.StreamStartedAt = time.Now()

	go s.receiveSttResults(stream, ctx, gen, done)
	log.Printf("🎙️ [%s] Google STT stream ready (gen %d)", s.ID, gen)
	return nil
}

// receiveSttResults menerima hasil transkrip dari satu stream STT (per generasi).
// Saat stream mati, field Stream di-nil-kan agar ensureGoogleStream membukanya
// kembali — kecuali stream sudah digantikan generasi yang lebih baru.
func (s *Session) receiveSttResults(stream speechpb.Speech_StreamingRecognizeClient, ctx context.Context, gen int, done chan struct{}) {
	defer close(done)
	for {
		resp, err := stream.Recv()
		if err != nil {
			if err != io.EOF && ctx.Err() == nil {
				log.Printf("⚠️ [%s] Google Recv error (gen %d): %v — stream akan di-restart otomatis", s.ID, gen, err)
			}
			s.SttMu.Lock()
			if s.StreamGen == gen {
				s.Stream = nil
			}
			s.SttMu.Unlock()
			return
		}

		for _, result := range resp.Results {
			if len(result.Alternatives) == 0 {
				continue
			}
			transcript := result.Alternatives[0].Transcript

			s.SttMu.Lock()
			if s.StreamGen != gen {
				// Hasil dari stream lama setelah digantikan — abaikan.
				s.SttMu.Unlock()
				continue
			}
			s.LastTextUpdateTime = time.Now()
			isFinal := result.IsFinal
			if isFinal {
				s.FinalText += transcript + " "
				s.InterimText = ""
			} else {
				s.InterimText = transcript
			}
			currentView := s.FinalText + s.InterimText
			s.SttMu.Unlock()

			if !isFinal {
				s.SendJSON("stt", currentView, "")
			}
		}
	}
}

// ensureGoogleStream memastikan stream STT hidup: restart jika mati, rotasi
// proaktif jika mendekati hard limit Google. Dipanggil di read loop untuk setiap
// frame audio yang masuk (murah: hanya cek pointer + umur stream).
func (s *Session) ensureGoogleStream() {
	if s.Closed.Load() {
		return
	}

	s.Mutex.Lock()
	speaking := s.IsSpeaking
	s.Mutex.Unlock()

	s.SttMu.Lock()
	defer s.SttMu.Unlock()

	if s.Stream == nil {
		if err := s.openGoogleStreamLocked(); err != nil {
			log.Printf("❌ [%s] STT stream restart failed: %v", s.ID, err)
		}
		return
	}

	age := time.Since(s.StreamStartedAt)
	if (age > SttRotateAge && !speaking) || age > SttForceRotateAge {
		log.Printf("♻️ [%s] Rotating STT stream (age %.0fs)", s.ID, age.Seconds())
		if err := s.openGoogleStreamLocked(); err != nil {
			log.Printf("❌ [%s] STT stream rotation failed: %v", s.ID, err)
		}
	}
}

// sendAudioToGoogle mem-feed PCM ke stream STT aktif. Jika pengiriman gagal,
// stream ditandai mati agar ensureGoogleStream me-restart-nya pada frame berikut.
func (s *Session) sendAudioToGoogle(pcmData []int16) {
	s.SttMu.Lock()
	stream := s.Stream
	s.SttMu.Unlock()
	if stream == nil {
		return
	}

	byteData := make([]byte, len(pcmData)*2)
	for i, v := range pcmData {
		binary.LittleEndian.PutUint16(byteData[i*2:], uint16(v))
	}

	if err := stream.Send(&speechpb.StreamingRecognizeRequest{
		StreamingRequest: &speechpb.StreamingRecognizeRequest_AudioContent{
			AudioContent: byteData,
		},
	}); err != nil {
		log.Printf("⚠️ [%s] Error sending audio to Google: %v (stream akan di-restart)", s.ID, err)
		s.SttMu.Lock()
		if s.Stream == stream {
			s.Stream = nil
		}
		s.SttMu.Unlock()
	}
}

// harvestSttResult mengambil transkrip yang terkumpul (menunggu finalisasi Google
// sebentar), me-reset buffer teks, lalu LANGSUNG membuka stream baru sehingga STT
// tetap nyala untuk ucapan berikutnya.
func (s *Session) harvestSttResult() string {
	s.SttMu.Lock()
	stream := s.Stream
	done := s.StreamDone
	s.SttMu.Unlock()

	if stream != nil {
		stream.CloseSend() // minta Google mem-finalisasi sisa audio yang di-buffer
		if done != nil {
			select {
			case <-done:
			case <-time.After(500 * time.Millisecond):
				log.Printf("⚠️ [%s] Google STT finalize timeout", s.ID)
			}
		}
	}

	s.SttMu.Lock()
	fullResult := strings.TrimSpace(s.FinalText + s.InterimText)
	s.FinalText = ""
	s.InterimText = ""
	if err := s.openGoogleStreamLocked(); err != nil {
		log.Printf("❌ [%s] STT reopen after harvest failed: %v", s.ID, err)
	}
	s.SttMu.Unlock()

	log.Printf("🗣️ [%s] Google Final STT: %s", s.ID, fullResult)
	s.SendJSON("stt", fullResult, "")
	return fullResult
}

// closeStreamLocked menutup stream aktif tanpa membuka yang baru. HARUS dipanggil
// dengan SttMu sudah dipegang.
func (s *Session) closeStreamLocked() {
	if s.Stream != nil {
		s.Stream.CloseSend()
	}
	if s.StreamCancel != nil {
		s.StreamCancel()
		s.StreamCancel = nil
	}
	s.Stream = nil
}

// closeGoogleStream menutup stream STT sepenuhnya. Dipanggil (via defer) saat
// koneksi WS berakhir — apapun penyebabnya (idle timeout, END dari agent, atau
// diputus paksa oleh user) — supaya tidak ada stream gRPC yang bocor dan sesi
// berikutnya selalu mendapat STT segar.
func (s *Session) closeGoogleStream() {
	s.Closed.Store(true) // hentikan reopen dari goroutine lain (idle monitor, dsb.)
	s.SttMu.Lock()
	defer s.SttMu.Unlock()
	s.StreamGen++ // invalidasi goroutine receiver lama
	s.closeStreamLocked()
	log.Printf("🎙️ [%s] Google STT stream closed (session cleanup)", s.ID)
}

// --- FUNGSI HELPER: KIRIM JSON KE DEVICE (LCD) ---
//
// Untuk pesan "llm", field emotion selalu dinormalisasi ke salah satu dari 21 emosi
// GIF valid di avagenc_128 agar device pasti punya animasi yang cocok.
func (s *Session) SendJSON(msgType string, text string, emotion string) {
	msg := XiaozhiMessage{
		Type:      msgType,
		SessionID: s.ID,
		Text:      text,
		Emotion:   emotion,
	}

	if msgType == "llm" {
		if msg.Emotion == "" {
			msg.Emotion = "neutral"
		}
		msg.Emotion = normalizeDeviceEmotion(msg.Emotion)
	}

	// Log pesan keluar ke device (llm=text+emosi, stt=transkrip live).
	switch msgType {
	case "llm":
		log.Printf("📤 [%s] -> llm | emo=%-12s | %q", s.ID, msg.Emotion, text)
	case "stt":
		log.Printf("📤 [%s] -> stt | %q", s.ID, text)
	default:
		log.Printf("📤 [%s] -> %s | %q", s.ID, msgType, text)
	}

	// SEMUA tulisan ke koneksi WS (JSON maupun frame audio biner) HARUS lewat
	// WriteMutex yang sama. gorilla/websocket PANIC pada concurrent write —
	// panic itu mematikan seluruh proses server, bukan hanya satu sesi.
	s.WriteMutex.Lock()
	err := s.Conn.WriteJSON(msg)
	s.WriteMutex.Unlock()
	if err != nil {
		log.Printf("⚠️ [%s] WriteJSON(%s) error: %v", s.ID, msgType, err)
	}
}

// endTagRe mendeteksi tag #END# (case-insensitive) di output Main Agent —
// perintah untuk menutup panggilan setelah semua segmen selesai diputar.
var endTagRe = regexp.MustCompile(`(?i)#\s*end\s*#`)

func isValidSegment(text string) bool {
	text = strings.TrimSpace(text)
	runes := []rune(text)

	if len(runes) == 1 {
		return unicode.IsLetter(runes[0])
	}
	return len(runes) > 0
}

// parseSegments memecah output Main Agent menjadi segmen-segmen berdasarkan tag
// emosi #emotion#. Emosi yang tidak dikenal akan dinormalisasi ke emosi device valid
// saat dikirim (lihat SendJSON) & saat dipetakan ke Cartesia.
func parseSegments(input string) []Segment {
	var segments []Segment
	defaultEmotion := "neutral"
	re := regexp.MustCompile(`#\s*([a-zA-Z_]+)\s*#`)
	matches := re.FindAllStringSubmatchIndex(input, -1)
	lastPos := 0

	for _, match := range matches {
		startTag, endTag := match[0], match[1]
		startEmo, endEmo := match[2], match[3]
		textPart := strings.TrimSpace(input[lastPos:startTag])
		emotionPart := strings.ToLower(input[startEmo:endEmo])

		if textPart != "" {
			segments = append(segments, Segment{Text: textPart, Emotion: emotionPart})
		}
		lastPos = endTag
	}

	if lastPos < len(input) {
		remainingText := strings.TrimSpace(input[lastPos:])
		if remainingText != "" {
			segments = append(segments, Segment{Text: remainingText, Emotion: defaultEmotion})
		}
	}

	if len(segments) == 0 && len(strings.TrimSpace(input)) > 0 {
		segments = append(segments, Segment{Text: strings.TrimSpace(input), Emotion: defaultEmotion})
	}
	return segments
}

func monitorIdleTimeout(s *Session, stopChan chan bool) {
	defer func() {
		if rec := recover(); rec != nil {
			log.Printf("🔥 [%s] PANIC recovered in idle monitor: %v", s.ID, rec)
		}
	}()

	ticker := time.NewTicker(1 * time.Second)
	defer ticker.Stop()

	for {
		select {
		case <-stopChan:
			return
		case <-ticker.C:
			// Jaga STT tetap hidup meski device sedang tidak mengirim audio
			// (mis. Google menutup stream karena "audio timeout" saat hening).
			s.ensureGoogleStream()

			s.Mutex.Lock()
			elapsed := time.Since(s.LastActivity)
			isProcessing := s.IsProcessing
			currentStage := s.TimeoutStage
			s.Mutex.Unlock()

			if isProcessing {
				continue
			}

			if elapsed > UserIdleTimeout && currentStage == 0 {
				log.Println("⏰ Idle 10s. Sending Warning...")
				s.Mutex.Lock()
				s.IsProcessing = true
				s.Mutex.Unlock()

				s.SendJSON("llm", "Apakah ada hal lain yang bisa saya bantu?", "affectionate")
				streamAudioToDevice(s, CachedTimeoutWarning)

				s.Mutex.Lock()
				s.IsProcessing = false
				s.LastActivity = time.Now()
				s.TimeoutStage = 1
				s.Mutex.Unlock()
			}

			if elapsed > UserIdleTimeout && currentStage == 1 {
				log.Println("⏰ Idle 10s again (Stage 2). Closing.")
				s.Mutex.Lock()
				s.IsProcessing = true
				s.Mutex.Unlock()

				s.SendJSON("llm", "Sampai jumpa lagi bos.", "melancholy")
				streamAudioToDevice(s, CachedGoodbye)

				s.endCall("Bye Idle")
				return
			}
		}
	}
}

// --- CORE PIPELINE ---

// processPipeline adalah pipeline utama yang dioptimasi untuk Time-to-First-Audio (TTFA).
//
// Strategi:
//  1. Segmen pertama di-generate SEGERA (blocking) untuk TTFA minimum.
//  2. Segmen berikutnya di-generate CONCURRENT di background selagi segmen pertama diputar.
//  3. Pipeline: generate[0] -> play[0] + generate[1..] -> play[1] -> play[2] -> ...
//
// Latensi yang dirasakan user = latensi Cartesia untuk 1 segmen saja.
func processPipeline(s *Session, textInput string) {
	defer func() {
		if rec := recover(); rec != nil {
			log.Printf("🔥 [%s] PANIC recovered in pipeline: %v", s.ID, rec)
		}
		s.Mutex.Lock()
		s.IsProcessing = false
		s.LastActivity = time.Now()
		s.TimeoutStage = 0
		s.Mutex.Unlock()
	}()

	if textInput == "" {
		log.Printf("🧑 [%s] Empty query — skipping pipeline", s.ID)
		return
	}

	log.Printf("🧑 [%s] User query: %q", s.ID, textInput)

	// Pipeline baru = playback baru; bersihkan sisa flag abort dari interaksi lalu.
	s.AbortPlayback.Store(false)

	cleanInput := strings.ToLower(strings.TrimRight(strings.TrimSpace(textInput), " .,!?"))

	if cleanInput == "ulang" || strings.HasSuffix(cleanInput, " ulang") {
		log.Printf("🛑 User membatalkan perintah. (Teks asli: %s)", textInput)
		s.SendJSON("llm", "Oke bos, silakan diulang.", "relieved")
		streamAudioToDevice(s, CachedUlangiPCM)
		s.SendJSON("llm", "", "neutral")
		return
	}

	log.Println("🤔 Mengirim status thinking ke device...")
	s.SendJSON("llm", "thinking . . .", "thinking")

	// === STEP 1: Panggil Main Agent ===
	log.Println("2️⃣ Brain (Main Agent)...")
	tBrain := time.Now()
	mainAgentMessage, err := callMainAgent(textInput, s.UserID)
	if err != nil {
		log.Printf("❌ Main Agent Error: %v", err)
		mainAgentMessage = "Maaf, ada gangguan."
	}
	log.Printf("🤖 AI Response (%.0fms): %s", float64(time.Since(tBrain).Milliseconds()), mainAgentMessage)

	cleanMsg := strings.TrimSpace(mainAgentMessage)
	if strings.EqualFold(cleanMsg, "end") {
		log.Println("🛑 Main Agent requested END.")
		s.SendJSON("llm", "Sampai jumpa lagi bos.", "melancholy")
		streamAudioToDevice(s, CachedGoodbye)
		s.endCall("Bye Command")
		return
	}

	// === STEP 2: Parse Segments ===
	// Emotion "END" dari agent adalah PERINTAH tutup panggilan, bukan emosi
	// tampilan: semua segmen tetap diputar dulu, lalu koneksi WS ditutup.
	// Dideteksi di teks mentah agar posisi tag bebas (awal/tengah/akhir kalimat).
	// Untuk tampilannya sendiri segmen ber-tag END memakai "melancholy" (nada
	// perpisahan, konsisten dengan pesan goodbye lain).
	endCall := endTagRe.MatchString(mainAgentMessage)
	rawSegments := parseSegments(mainAgentMessage)
	var segments []Segment
	for _, seg := range rawSegments {
		if strings.EqualFold(strings.TrimSpace(seg.Emotion), "end") {
			seg.Emotion = "melancholy"
		}
		if isValidSegment(seg.Text) {
			segments = append(segments, seg)
		}
	}
	log.Printf("🧩 Valid Segments Found: %d (endCall=%v)", len(segments), endCall)

	if len(segments) == 0 {
		if endCall {
			// Agent hanya mengirim #END# tanpa teks — pamit dengan pesan cache.
			s.SendJSON("llm", "Sampai jumpa lagi bos.", "melancholy")
			streamAudioToDevice(s, CachedGoodbye)
			s.endCall("Bye END")
		}
		return
	}

	// === STEP 3: Generate segmen 2..n CONCURRENT di background ===
	// (dimulai duluan supaya sudah siap saat gilirannya tiba)
	type indexedResult struct {
		index int
		audio []int16
		err   error
	}

	remainingCount := len(segments) - 1
	remainingChs := make([]chan indexedResult, remainingCount)
	for i := 0; i < remainingCount; i++ {
		ch := make(chan indexedResult, 1)
		remainingChs[i] = ch
		go func(idx int, seg Segment) {
			log.Printf("🔄 [BG] Generating segment %d/%d: [%s]", idx+2, len(segments), seg.Text)
			audio, err := callCartesiaTTS(seg.Text, seg.Emotion)
			ch <- indexedResult{index: idx + 1, audio: audio, err: err}
		}(i, segments[i+1])
	}

	// === STEP 4: LIVE-STREAM segmen pertama (Time-to-First-Audio minimum) ===
	// Audio mulai terdengar begitu chunk PCM pertama tiba dari Cartesia —
	// TIDAK menunggu seluruh segmen selesai di-generate seperti mode buffered.
	tTTFA := time.Now()
	log.Printf("▶️ Playing Segment 1/%d (live): [%s] (Emo: %s)", len(segments), segments[0].Text, segments[0].Emotion)
	s.SendJSON("llm", segments[0].Text, segments[0].Emotion)
	if err := streamCartesiaTTSLive(s, segments[0].Text, segments[0].Emotion); err != nil {
		log.Printf("❌ Live TTS segment 1 failed: %v — falling back to buffered", err)
		if audio, err2 := callCartesiaTTS(segments[0].Text, segments[0].Emotion); err2 == nil {
			streamAudioToDevice(s, audio)
		} else {
			log.Printf("❌ Buffered fallback also failed: %v", err2)
		}
	}
	log.Printf("⚡ [TTFA] Segment 1 done %.0fms after pipeline start", float64(time.Since(tTTFA).Milliseconds()))

	// === STEP 5: Putar segmen sisanya secara berurutan ===
	for i, ch := range remainingChs {
		segIdx := i + 1
		res := <-ch

		// Device menyela (abort) — buang sisa segmen, kembali mendengarkan.
		if s.AbortPlayback.Load() {
			log.Printf("🛑 [%s] Skipping remaining segments after abort", s.ID)
			break
		}

		seg := segments[segIdx]
		log.Printf("▶️ Playing Segment %d/%d: [%s] (Emo: %s)", segIdx+1, len(segments), seg.Text, seg.Emotion)
		s.SendJSON("llm", seg.Text, seg.Emotion)

		if res.err != nil {
			log.Printf("❌ TTS Error on segment %d: %v", segIdx+1, res.err)
			continue
		}

		// Jeda singkat antar segmen agar transisi terasa natural.
		time.Sleep(InterSegmentPause)
		streamAudioToDevice(s, res.audio)
	}

	if endCall {
		log.Printf("👋 [%s] Emotion END dari agent — closing connection after playback", s.ID)
		s.endCall("Bye END")
		return
	}

	log.Println("😐 Resetting emotion to Neutral.")
	s.SendJSON("llm", "", "neutral")
	log.Println("✅ All segments processed. Ready for next input.")
}

// endCall menutup koneksi WS dengan rapi: kirim frame Close (agar device tahu ini
// penutupan normal, bukan error jaringan) lalu tutup socket. Pembersihan STT
// terjadi otomatis lewat defer closeGoogleStream di handleWebSocket saat read
// loop berakhir.
func (s *Session) endCall(reason string) {
	log.Printf("👋 [%s] Ending call: %s", s.ID, reason)
	s.WriteMutex.Lock()
	s.Conn.WriteMessage(websocket.CloseMessage, websocket.FormatCloseMessage(websocket.CloseNormalClosure, reason))
	s.WriteMutex.Unlock()
	s.Conn.Close()
}

// streamAudioToDevice mengirim PCM int16 (16kHz) ke device sebagai frame Opus.
//
// Kunci audio "lancar" (tidak patah):
//   - Prebuffer: PrebufferFrames frame pertama dikirim back-to-back untuk mengisi
//     jitter buffer device sebelum playback, menyerap lonjakan latensi jaringan.
//   - Wall-clock pacing: frame ke-i dijadwalkan pada waktu absolut (start + i*20ms),
//     BUKAN dengan time.Sleep tetap. Ini mencegah waktu encode+kirim terakumulasi
//     menjadi drift yang perlahan mengosongkan buffer device (penyebab audio patah).
func streamAudioToDevice(s *Session, pcmData []int16) {
	if len(pcmData) == 0 {
		return
	}

	log.Printf("🔊 [%s] Streaming audio: %d samples (~%.0fms)", s.ID, len(pcmData), float64(len(pcmData))/float64(SampleRate)*1000.0)

	// Silence padding di ekor: flush lookahead encoder Opus + tail buffer device.
	silencePadding := make([]int16, FrameSize*SilencePaddingFrames)
	paddedPCM := append(pcmData, silencePadding...)

	buffer := make([]byte, 4000)
	start := time.Now()
	prebufferLead := time.Duration(PrebufferFrames) * FrameInterval

	frameIndex := 0
	for cursor := 0; cursor+FrameSize <= len(paddedPCM); cursor += FrameSize {
		// Device meminta berhenti (abort) — hentikan streaming segera.
		if s.AbortPlayback.Load() {
			log.Printf("🛑 [%s] Playback aborted by device at frame %d", s.ID, frameIndex)
			return
		}
		frame := paddedPCM[cursor : cursor+FrameSize]

		n, err := s.Encoder.Encode(frame, buffer)
		if err != nil {
			frameIndex++
			continue
		}

		// Target waktu kirim frame ini. Untuk PrebufferFrames pertama, target berada
		// di masa lalu (negatif) sehingga langsung dikirim tanpa jeda -> priming buffer.
		target := start.Add(time.Duration(frameIndex)*FrameInterval - prebufferLead)
		if d := time.Until(target); d > 0 {
			time.Sleep(d)
		}

		s.WriteMutex.Lock()
		err = s.Conn.WriteMessage(websocket.BinaryMessage, buffer[:n])
		s.WriteMutex.Unlock()

		if err != nil {
			return
		}
		frameIndex++
	}
}

// --- CARTESIA TTS ---

// callCartesiaTTS memanggil Cartesia API dan mengembalikan PCM int16 pada 16000Hz.
//
// Optimasi audio jernih:
//   - Output langsung raw PCM s16le @ 16000Hz -> TIDAK ada resample (bebas aliasing)
//     dan TIDAK ada WAV header untuk di-strip (container "raw" = pure PCM).
//   - language "id" untuk pengucapan Bahasa Indonesia yang benar.
//   - emotion dipetakan dari emosi device (GIF) ke 7 emosi Cartesia agar selaras.
// newCartesiaRequest membangun HTTP request TTS (dipakai mode buffered & streaming).
func newCartesiaRequest(text string, emotion string) (*http.Request, error) {
	if text == "" {
		return nil, fmt.Errorf("empty text")
	}

	apiKey := os.Getenv("CARTESIA_API_KEY")
	if apiKey == "" {
		return nil, fmt.Errorf("CARTESIA_API_KEY not set")
	}

	reqBody := CartesiaTTSRequest{
		ModelID:    CartesiaModel(),
		Transcript: text,
		Language:   CartesiaLanguage,
		Voice: CartesiaVoice{
			Mode: CartesiaVoiceMode,
			ID:   CartesiaVoiceID,
		},
		OutputFmt: CartesiaOutputFmt{
			Container:  "raw", // pure PCM, tanpa header
			Encoding:   "pcm_s16le",
			SampleRate: CartesiaSampleRate, // 16000Hz native -> cocok Opus, tanpa resample
		},
		GenConfig: CartesiaGenConfig{
			Speed:   1.1, // sedikit lebih cepat (preferensi user)
			Volume:  1.0,
			Emotion: mapEmotionToCartesia(emotion),
		},
	}

	jsonBytes, err := json.Marshal(reqBody)
	if err != nil {
		return nil, fmt.Errorf("marshal cartesia request: %v", err)
	}

	req, err := http.NewRequest("POST", "https://api.cartesia.ai/tts/bytes", bytes.NewBuffer(jsonBytes))
	if err != nil {
		return nil, fmt.Errorf("create cartesia request: %v", err)
	}
	req.Header.Set("Cartesia-Version", CartesiaAPIVersion)
	req.Header.Set("X-API-Key", apiKey)
	req.Header.Set("Content-Type", "application/json")
	return req, nil
}

func callCartesiaTTS(text string, emotion string) ([]int16, error) {
	req, err := newCartesiaRequest(text, emotion)
	if err != nil {
		return nil, err
	}

	resp, err := globalHTTPClient.Do(req)
	if err != nil {
		return nil, fmt.Errorf("cartesia request failed: %v", err)
	}
	defer resp.Body.Close()

	if resp.StatusCode != http.StatusOK {
		body, _ := io.ReadAll(resp.Body)
		return nil, fmt.Errorf("cartesia error %d: %s", resp.StatusCode, string(body))
	}

	rawPCMBytes, err := io.ReadAll(resp.Body)
	if err != nil {
		return nil, fmt.Errorf("read cartesia response: %v", err)
	}

	// Container "raw" -> body langsung PCM 16-bit little-endian @16000Hz, tanpa header.
	if len(rawPCMBytes) < 2 {
		return nil, fmt.Errorf("cartesia response too short: %d bytes", len(rawPCMBytes))
	}
	if len(rawPCMBytes)%2 != 0 {
		rawPCMBytes = rawPCMBytes[:len(rawPCMBytes)-1]
	}

	pcm := make([]int16, len(rawPCMBytes)/2)
	for i := range pcm {
		pcm[i] = int16(binary.LittleEndian.Uint16(rawPCMBytes[i*2 : i*2+2]))
	}
	return pcm, nil
}

// streamCartesiaTTSLive men-stream TTS LANGSUNG dari Cartesia ke device tanpa
// menunggu seluruh audio selesai di-generate.
//
// Ini kunci Time-to-First-Audio rendah: mode buffered (callCartesiaTTS) menunggu
// SELURUH body respons (io.ReadAll) sebelum frame pertama dikirim — untuk kalimat
// panjang itu 1-3 detik hening. Di sini frame Opus pertama terkirim begitu ~20ms
// PCM pertama tiba dari Cartesia (biasanya 100-300ms setelah request).
//
// Pacing tetap wall-clock (anchor = frame pertama) + prebuffer, identik dengan
// streamAudioToDevice, sehingga kualitas "lancar" tidak berubah. Cartesia
// men-generate lebih cepat dari real-time, jadi pengisian selalu mendahului pacing.
func streamCartesiaTTSLive(s *Session, text string, emotion string) error {
	req, err := newCartesiaRequest(text, emotion)
	if err != nil {
		return err
	}

	resp, err := globalHTTPClient.Do(req)
	if err != nil {
		return fmt.Errorf("cartesia request failed: %v", err)
	}
	defer resp.Body.Close()

	if resp.StatusCode != http.StatusOK {
		body, _ := io.ReadAll(resp.Body)
		return fmt.Errorf("cartesia error %d: %s", resp.StatusCode, string(body))
	}

	encBuf := make([]byte, 4000)
	readBuf := make([]byte, 4096)
	pcmFrame := make([]int16, 0, FrameSize*4)
	var leftover byte
	haveLeftover := false

	var start time.Time
	frameIndex := 0
	prebufferLead := time.Duration(PrebufferFrames) * FrameInterval
	totalSamples := 0

	sendFrame := func(frame []int16) error {
		n, encErr := s.Encoder.Encode(frame, encBuf)
		if encErr != nil {
			frameIndex++
			return nil
		}
		if frameIndex == 0 {
			start = time.Now()
			log.Printf("⚡ [%s] First TTS chunk arrived — audio starts now", s.ID)
		}
		target := start.Add(time.Duration(frameIndex)*FrameInterval - prebufferLead)
		if d := time.Until(target); d > 0 {
			time.Sleep(d)
		}
		s.WriteMutex.Lock()
		wErr := s.Conn.WriteMessage(websocket.BinaryMessage, encBuf[:n])
		s.WriteMutex.Unlock()
		frameIndex++
		return wErr
	}

	for {
		if s.AbortPlayback.Load() {
			log.Printf("🛑 [%s] Live TTS aborted by device", s.ID)
			return nil
		}

		n, readErr := resp.Body.Read(readBuf)
		if n > 0 {
			chunk := readBuf[:n]
			// Sambungkan byte ganjil yang tersisa dari chunk sebelumnya.
			if haveLeftover {
				pcmFrame = append(pcmFrame, int16(binary.LittleEndian.Uint16([]byte{leftover, chunk[0]})))
				chunk = chunk[1:]
				haveLeftover = false
			}
			if len(chunk)%2 != 0 {
				leftover = chunk[len(chunk)-1]
				haveLeftover = true
				chunk = chunk[:len(chunk)-1]
			}
			for i := 0; i+1 < len(chunk); i += 2 {
				pcmFrame = append(pcmFrame, int16(binary.LittleEndian.Uint16(chunk[i:i+2])))
			}
			for len(pcmFrame) >= FrameSize {
				if err := sendFrame(pcmFrame[:FrameSize]); err != nil {
					return err
				}
				totalSamples += FrameSize
				pcmFrame = pcmFrame[FrameSize:]
			}
		}
		if readErr == io.EOF {
			break
		}
		if readErr != nil {
			return fmt.Errorf("read cartesia stream: %v", readErr)
		}
	}

	// Ekor: genapkan sisa PCM jadi satu frame + silence padding (flush lookahead
	// encoder Opus + tail buffer device) — sama seperti streamAudioToDevice.
	if len(pcmFrame) > 0 {
		totalSamples += len(pcmFrame)
		pad := make([]int16, FrameSize-len(pcmFrame))
		pcmFrame = append(pcmFrame, pad...)
		if err := sendFrame(pcmFrame); err != nil {
			return err
		}
	}
	silence := make([]int16, FrameSize)
	for i := 0; i < SilencePaddingFrames; i++ {
		if err := sendFrame(silence); err != nil {
			return err
		}
	}

	log.Printf("🔊 [%s] Live-streamed TTS: %d samples (~%.0fms)", s.ID, totalSamples,
		float64(totalSamples)/float64(SampleRate)*1000.0)
	return nil
}

// --- EXTERNAL CALLS ---

// callMainAgent memanggil Main Agent dengan format terbaru.
//
//	HEADER : user-id, time-zone (IANA), api-key
//	REQ    : { "message": "string" }
//	RESP   : { "response": "string" }
//
// userID berasal dari sesi (header X-User-Id hasil pairing lokal device), BUKAN
// lagi dari env X_USER_ID — jadi tiap device/sesi memakai user-nya sendiri.
func callMainAgent(text string, userID string) (string, error) {
	url := os.Getenv("MAIN_AGENT_URL")
	if url == "" {
		return "", fmt.Errorf("MAIN_AGENT_URL is empty")
	}

	if userID == "" {
		log.Println("⚠️ Warning: user-id kosong (device belum ter-pairing); Main Agent dipanggil tanpa user-id")
	}

	// Time zone dalam format IANA (mis. "Asia/Jakarta"). Default ke Asia/Jakarta.
	timeZone := os.Getenv("MAIN_AGENT_TIMEZONE")
	if timeZone == "" {
		timeZone = "Asia/Jakarta"
	}

	apiKey := os.Getenv("MAIN_AGENT_API_KEY")
	if apiKey == "" {
		log.Println("⚠️ Warning: MAIN_AGENT_API_KEY kosong di environment variables")
	}

	payload := MainAgentRequest{
		Message: text,
	}
	jsonPayload, _ := json.Marshal(payload)

	req, err := http.NewRequest("POST", url, bytes.NewBuffer(jsonPayload))
	if err != nil {
		return "", err
	}
	req.Header.Set("Content-Type", "application/json")
	// Header sesuai format Main Agent terbaru.
	req.Header.Set("user-id", userID)
	req.Header.Set("time-zone", timeZone)
	req.Header.Set("api-key", apiKey)

	resp, err := globalHTTPClient.Do(req)
	if err != nil {
		return "", err
	}
	defer resp.Body.Close()

	bodyBytes, err := io.ReadAll(resp.Body)
	if err != nil {
		return "", err
	}

	if resp.StatusCode != http.StatusOK {
		return "", fmt.Errorf("main agent error %d: %s", resp.StatusCode, string(bodyBytes))
	}

	var agentResp MainAgentResponse
	if err := json.Unmarshal(bodyBytes, &agentResp); err == nil && agentResp.Response != "" {
		return agentResp.Response, nil
	}

	// Fallback: jika bukan JSON yang diharapkan, kembalikan body mentah.
	return strings.TrimSpace(string(bodyBytes)), nil
}

func handleHandshake(s *Session) {
	mt, data, err := s.Conn.ReadMessage()
	if err != nil {
		log.Printf("⚠️ [%s] Handshake read error: %v", s.ID, err)
	} else {
		log.Printf("🤝 [%s] Handshake recv (type=%d): %s", s.ID, mt, string(data))
	}

	resp := map[string]interface{}{
		"type":       "hello",
		"transport":  "websocket",
		"session_id": s.ID,
		"audio_params": map[string]interface{}{
			"format": "opus", "sample_rate": 16000, "channels": 1, "frame_duration": 20,
		},
	}
	s.WriteMutex.Lock()
	err = s.Conn.WriteJSON(resp)
	s.WriteMutex.Unlock()
	if err != nil {
		log.Printf("⚠️ [%s] Handshake write error: %v", s.ID, err)
	} else {
		log.Printf("🤝 [%s] Sent hello (opus/16000Hz/20ms)", s.ID)
	}
}

func calculateRMS(samples []int16) float64 {
	if len(samples) == 0 {
		return 0
	}
	var sum float64
	for _, s := range samples {
		sum += float64(s) * float64(s)
	}
	return math.Sqrt(sum / float64(len(samples)))
}
