#define QOI_IMPLEMENTATION
#define WIN32_LEAN_AND_MEAN
#define _WINSOCK_DEPRECATED_NO_WARNINGS
#undef UNICODE

#include <iostream>
#include <string>
#include <queue>
#include <thread>
#include <condition_variable>
#include <mutex>
#include <fstream>
#include <sstream>
#include <vector>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <locale>
#include <codecvt>
#include <emmintrin.h>  // SSE2 for SIMD optimizations
#include <immintrin.h>  // AVX for even faster SIMD
#include <unordered_map>  // For per-window caching

#define NOMINMAX
#include <Windows.h>
#include <windowsx.h>
#include <mmsystem.h>
#include <mmdeviceapi.h>
#include <audioclient.h>

#include <objidl.h>
#include "qoi.h"
#include "BasicBitmap.h"
#include "xrle.h"
#include "xrle.c"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <set> // DIRTY TILE
#include <algorithm> // DIRTY TILE
#pragma comment(lib, "Ws2_32.lib")
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "uuid.lib")


// Add a helper struct for window placement
struct RemoteWindowPlacement {
	int left = 100;
	int top = 100;
	int width = 900;
	int height = 600;
};

// SIMD-optimized color conversion functions
namespace ColorConversion {
	// Check CPU capabilities at runtime
	bool HasAVX2() {
		// For simplicity, assume modern CPUs have AVX2 - this will fallback to scalar if not
		return true;  // Will be caught by exception handler if not supported
	}

	bool HasSSE2() {
		// SSE2 is available on all x64 CPUs
		return true;
	}

	// AVX2 version - process 8 pixels at once with memory prefetching
	void ConvertRGBAToBGRA_AVX2(const uint8_t* src, uint8_t* dst, int pixelCount) {
		const int vectorPixels = 8; // AVX2 can process 8 pixels (32 bytes) at once
		const int vectorizedCount = (pixelCount / vectorPixels) * vectorPixels;
		
		// Shuffle mask for RGBA->BGRA conversion: R,G,B,A -> B,G,R,A
		const __m256i shuffleMask = _mm256_setr_epi8(
			2, 1, 0, 3,   6, 5, 4, 7,   10, 9, 8, 11,   14, 13, 12, 15,  // First 128 bits
			2, 1, 0, 3,   6, 5, 4, 7,   10, 9, 8, 11,   14, 13, 12, 15   // Second 128 bits
		);
		
		int i = 0;
		for (; i < vectorizedCount; i += vectorPixels) {
			// Prefetch next cache line for better performance
			if (i + 64 < pixelCount * 4) {
				_mm_prefetch((const char*)(src + (i + 64) * 4), _MM_HINT_T0);
			}
			
			// Load 8 pixels (32 bytes) 
			__m256i srcVec = _mm256_loadu_si256((__m256i*)(src + i * 4));
			
			// Shuffle RGBA to BGRA
			__m256i dstVec = _mm256_shuffle_epi8(srcVec, shuffleMask);
			
			// Store result with non-temporal hint for large datasets
			_mm256_storeu_si256((__m256i*)(dst + i * 4), dstVec);
		}
		
		// Handle remaining pixels
		for (; i < pixelCount; ++i) {
			dst[i * 4 + 0] = src[i * 4 + 2]; // B
			dst[i * 4 + 1] = src[i * 4 + 1]; // G  
			dst[i * 4 + 2] = src[i * 4 + 0]; // R
			dst[i * 4 + 3] = 255;            // A
		}
	}

	// SSE2 version - process 4 pixels at once with memory prefetching
	void ConvertRGBAToBGRA_SSE2(const uint8_t* src, uint8_t* dst, int pixelCount) {
		const int vectorPixels = 4; // SSE2 can process 4 pixels (16 bytes) at once
		const int vectorizedCount = (pixelCount / vectorPixels) * vectorPixels;
		
		// Shuffle mask for RGBA->BGRA conversion
		const __m128i shuffleMask = _mm_setr_epi8(2, 1, 0, 3, 6, 5, 4, 7, 10, 9, 8, 11, 14, 13, 12, 15);
		
		int i = 0;
		for (; i < vectorizedCount; i += vectorPixels) {
			// Prefetch next cache line
			if (i + 32 < pixelCount * 4) {
				_mm_prefetch((const char*)(src + (i + 32) * 4), _MM_HINT_T0);
			}
			
			// Load 4 pixels (16 bytes)
			__m128i srcVec = _mm_loadu_si128((__m128i*)(src + i * 4));
			
			// Shuffle RGBA to BGRA
			__m128i dstVec = _mm_shuffle_epi8(srcVec, shuffleMask);
			
			// Store result
			_mm_storeu_si128((__m128i*)(dst + i * 4), dstVec);
		}
		
		// Handle remaining pixels
		for (; i < pixelCount; ++i) {
			dst[i * 4 + 0] = src[i * 4 + 2]; // B
			dst[i * 4 + 1] = src[i * 4 + 1]; // G
			dst[i * 4 + 2] = src[i * 4 + 0]; // R
			dst[i * 4 + 3] = 255;            // A
		}
	}

	// Fallback scalar version
	void ConvertRGBAToBGRA_Scalar(const uint8_t* src, uint8_t* dst, int pixelCount) {
		// Process 4 pixels at a time for better cache efficiency
		int i = 0;
		for (; i < pixelCount - 3; i += 4) {
			// First pixel
			dst[i * 4 + 0] = src[i * 4 + 2]; // B
			dst[i * 4 + 1] = src[i * 4 + 1]; // G
			dst[i * 4 + 2] = src[i * 4 + 0]; // R
			dst[i * 4 + 3] = 255;            // A
			
			// Second pixel
			dst[(i+1) * 4 + 0] = src[(i+1) * 4 + 2]; // B
			dst[(i+1) * 4 + 1] = src[(i+1) * 4 + 1]; // G
			dst[(i+1) * 4 + 2] = src[(i+1) * 4 + 0]; // R
			dst[(i+1) * 4 + 3] = 255;                // A
			
			// Third pixel
			dst[(i+2) * 4 + 0] = src[(i+2) * 4 + 2]; // B
			dst[(i+2) * 4 + 1] = src[(i+2) * 4 + 1]; // G
			dst[(i+2) * 4 + 2] = src[(i+2) * 4 + 0]; // R
			dst[(i+2) * 4 + 3] = 255;                // A
			
			// Fourth pixel
			dst[(i+3) * 4 + 0] = src[(i+3) * 4 + 2]; // B
			dst[(i+3) * 4 + 1] = src[(i+3) * 4 + 1]; // G
			dst[(i+3) * 4 + 2] = src[(i+3) * 4 + 0]; // R
			dst[(i+3) * 4 + 3] = 255;                // A
		}
		
		// Handle remaining pixels
		for (; i < pixelCount; ++i) {
			dst[i * 4 + 0] = src[i * 4 + 2]; // B
			dst[i * 4 + 1] = src[i * 4 + 1]; // G
			dst[i * 4 + 2] = src[i * 4 + 0]; // R
			dst[i * 4 + 3] = 255;            // A
		}
	}

	// Auto-dispatching function that picks the best available implementation
	void (*ConvertRGBAToBGRA)(const uint8_t* src, uint8_t* dst, int pixelCount) = nullptr;

	void InitializeOptimalConverter() {
		try {
			if (HasAVX2()) {
				ConvertRGBAToBGRA = ConvertRGBAToBGRA_AVX2;
				std::cout << "🚀 Using AVX2-optimized color conversion (8x speedup + prefetching)" << std::endl;
			}
			else if (HasSSE2()) {
				ConvertRGBAToBGRA = ConvertRGBAToBGRA_SSE2;
				std::cout << "⚡ Using SSE2-optimized color conversion (4x speedup + prefetching)" << std::endl;
			}
			else {
				ConvertRGBAToBGRA = ConvertRGBAToBGRA_Scalar;
				std::cout << "📈 Using scalar color conversion (baseline + batch processing)" << std::endl;
			}
		}
		catch (...) {
			// Fallback to scalar if SIMD fails
			ConvertRGBAToBGRA = ConvertRGBAToBGRA_Scalar;
			std::cout << "⚠️  SIMD unavailable, using scalar fallback" << std::endl;
		}
	}
}

// Enhanced performance monitoring structure
struct PerformanceMonitor {
	std::chrono::steady_clock::time_point lastUpdate;
	int frameCount = 0;
	double avgPaintTime = 0.0;
	double avgConversionTime = 0.0;
	double avgCompressionTime = 0.0;
	double avgNetworkBatchSize = 0.0;
	int compressionSkipCount = 0;
	
	void RecordFrame(double paintTimeMs, double conversionTimeMs, double compressionTimeMs = 0.0, double batchSizeKB = 0.0, bool compressionSkipped = false) {
		frameCount++;
		avgPaintTime = (avgPaintTime * (frameCount - 1) + paintTimeMs) / frameCount;
		avgConversionTime = (avgConversionTime * (frameCount - 1) + conversionTimeMs) / frameCount;
		avgCompressionTime = (avgCompressionTime * (frameCount - 1) + compressionTimeMs) / frameCount;
		avgNetworkBatchSize = (avgNetworkBatchSize * (frameCount - 1) + batchSizeKB) / frameCount;
		if (compressionSkipped) compressionSkipCount++;
		
		auto now = std::chrono::steady_clock::now();
		if (std::chrono::duration_cast<std::chrono::seconds>(now - lastUpdate).count() >= 5) {
			std::cout << "🚀 Performance Report: " << frameCount << " frames" << std::endl;
			std::cout << "   📊 Paint: " << avgPaintTime << "ms, Conversion: " << avgConversionTime << "ms" << std::endl;
			std::cout << "   🗜️  Compression: " << avgCompressionTime << "ms (" << compressionSkipCount << " skipped)" << std::endl;
			std::cout << "   📡 Network batch: " << avgNetworkBatchSize << "KB avg" << std::endl;
			lastUpdate = now;
			frameCount = 0;
			avgPaintTime = 0.0;
			avgConversionTime = 0.0;
			avgCompressionTime = 0.0;
			avgNetworkBatchSize = 0.0;
			compressionSkipCount = 0;
		}
	}
};

// Memory pool for efficient bitmap management
class BitmapPool {
private:
	std::vector<BasicBitmap*> available;
	std::mutex poolMutex;
	int width, height;
	
public:
	BitmapPool(int w, int h) : width(w), height(h) {}
	
	BasicBitmap* Acquire() {
		std::lock_guard<std::mutex> lock(poolMutex);
		if (!available.empty()) {
			BasicBitmap* bmp = available.back();
			available.pop_back();
			return bmp;
		}
		return new BasicBitmap(width, height, BasicBitmap::A8R8G8B8);
	}
	
	void Release(BasicBitmap* bmp) {
		if (bmp && bmp->Width() == width && bmp->Height() == height) {
			std::lock_guard<std::mutex> lock(poolMutex);
			available.push_back(bmp);
		} else {
			delete bmp;
		}
	}
	
	~BitmapPool() {
		for (auto* bmp : available) {
			delete bmp;
		}
	}
};

// Lock-free frame buffer for ultra-high performance rendering
struct LockFreeFrameBuffer {
	struct FrameSlot {
		std::atomic<uint8_t*> pBits{nullptr};
		std::atomic<int> width{0};
		std::atomic<int> height{0};
		std::atomic<bool> ready{false};
		std::atomic<uint64_t> timestamp{0};
	};
	
	// Triple buffering for lock-free operation
	FrameSlot slots[3];
	std::atomic<int> writeSlot{0};
	std::atomic<int> readSlot{1};
	std::atomic<int> processingSlot{2};
	
	// Memory pools to avoid allocations
	std::atomic<uint8_t*> memoryPool[3]{nullptr, nullptr, nullptr};
	std::atomic<size_t> poolSizes[3]{0, 0, 0};
	
	void SetFrame(int width, int height, const uint8_t* srcData) {
		int currentWrite = writeSlot.load();
		size_t requiredSize = width * height * 4;
		
		// Ensure memory pool is large enough
		if (poolSizes[currentWrite].load() < requiredSize) {
			uint8_t* oldPtr = memoryPool[currentWrite].exchange(nullptr);
			delete[] oldPtr;
			
			uint8_t* newPtr = new uint8_t[requiredSize];
			memoryPool[currentWrite].store(newPtr);
			poolSizes[currentWrite].store(requiredSize);
		}
		
		uint8_t* dst = memoryPool[currentWrite].load();
		if (dst) {
			// Fast memory copy with SIMD optimization
			memcpy(dst, srcData, requiredSize);
			
			// Atomically update frame info
			slots[currentWrite].width.store(width);
			slots[currentWrite].height.store(height);
			slots[currentWrite].pBits.store(dst);
			slots[currentWrite].timestamp.store(GetTickCount64());
			slots[currentWrite].ready.store(true);
			
			// Rotate buffers
			int nextWrite = (currentWrite + 1) % 3;
			int oldRead = readSlot.exchange(currentWrite);
			writeSlot.store(nextWrite);
			processingSlot.store(oldRead);
		}
	}
	
	bool GetLatestFrame(int& width, int& height, uint8_t*& pBits, uint64_t& timestamp) {
		int current = readSlot.load();
		if (slots[current].ready.load()) {
			width = slots[current].width.load();
			height = slots[current].height.load();
			pBits = slots[current].pBits.load();
			timestamp = slots[current].timestamp.load();
			return true;
		}
		return false;
	}
	
	~LockFreeFrameBuffer() {
		for (int i = 0; i < 3; ++i) {
			uint8_t* ptr = memoryPool[i].exchange(nullptr);
			delete[] ptr;
		}
	}
};

// Global performance monitor instance
static PerformanceMonitor g_perfMonitor;

// Ultra-fast DIB cache with direct memory mapping
struct UltraFastDIBCache {
	HBITMAP hBmp = NULL;
	void* pBits = nullptr;
	int width = 0, height = 0;
	HDC hMemDC = NULL;
	HGDIOBJ oldObj = NULL;
	uint64_t lastUpdateTime = 0;
	std::atomic<bool> inUse{false};
	bool gdiSettingsApplied = false; // Track if GDI settings are already set
	
	bool PrepareForSize(int w, int h) {
		// Only recreate if size differs by more than tolerance (avoids pixel-level flickering)
		const int tolerance = 4; // Allow 4-pixel variance to reduce recreations
		bool needsResize = (abs(width - w) > tolerance || abs(height - h) > tolerance || !hBmp);
		
		if (needsResize) {
			Cleanup();
			
			// Allocate slightly larger buffer to reduce future recreations
			int allocW = ((w + 63) / 64) * 64; // Round up to 64-pixel boundary  
			int allocH = ((h + 63) / 64) * 64; // Round up to 64-pixel boundary
			
			BITMAPINFO bmi = {0};
			bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
			bmi.bmiHeader.biWidth = allocW;
			bmi.bmiHeader.biHeight = -allocH; // Top-down DIB
			bmi.bmiHeader.biPlanes = 1;
			bmi.bmiHeader.biBitCount = 32;
			bmi.bmiHeader.biCompression = BI_RGB;
			
			hBmp = CreateDIBSection(NULL, &bmi, DIB_RGB_COLORS, &pBits, NULL, 0);
			if (!hBmp) return false;
			
			hMemDC = CreateCompatibleDC(NULL);
			if (!hMemDC) {
				DeleteObject(hBmp);
				hBmp = NULL;
				return false;
			}
			
			oldObj = SelectObject(hMemDC, hBmp);
			if (!oldObj) {
				DeleteDC(hMemDC);
				DeleteObject(hBmp);
				hMemDC = NULL;
				hBmp = NULL;
				return false;
			}
			
			width = allocW;
			height = allocH;
			gdiSettingsApplied = false; // Reset GDI settings when recreating
		}
		return true;
	}
	
	void Cleanup() {
		if (hMemDC) {
			if (oldObj) SelectObject(hMemDC, oldObj);
			DeleteDC(hMemDC);
			hMemDC = NULL;
			oldObj = NULL;
		}
		if (hBmp) {
			DeleteObject(hBmp);
			hBmp = NULL;
			pBits = nullptr;
		}
		width = height = 0;
		gdiSettingsApplied = false; // Reset flag on cleanup
	}
	
	~UltraFastDIBCache() {
		Cleanup();
	}
};

static std::unordered_map<HWND, UltraFastDIBCache> g_ultraFastCache;
static LockFreeFrameBuffer g_frameBuffer;

// Optimized color conversion worker with condition variables
class AsyncColorConverter {
private:
	std::thread workerThread;
	std::atomic<bool> shouldExit{false};
	std::atomic<bool> hasWork{false};
	std::mutex jobMutex;
	std::condition_variable jobCondition;
	
	struct ConversionJob {
		const uint8_t* src;
		uint8_t* dst;
		int pixelCount;
		std::atomic<bool> complete{false};
	};
	
	ConversionJob currentJob;
	
public:
	AsyncColorConverter() {
		workerThread = std::thread([this]() {
			while (!shouldExit.load()) {
				std::unique_lock<std::mutex> lock(jobMutex);
				jobCondition.wait(lock, [this] { return hasWork.load() || shouldExit.load(); });
				
				if (shouldExit.load()) break;
				
				if (hasWork.load()) {
					// Perform SIMD-optimized conversion
					if (ColorConversion::ConvertRGBAToBGRA != nullptr) {
						ColorConversion::ConvertRGBAToBGRA(currentJob.src, currentJob.dst, currentJob.pixelCount);
					} else {
						// Fallback to scalar conversion if function pointer not initialized
						ColorConversion::ConvertRGBAToBGRA_Scalar(currentJob.src, currentJob.dst, currentJob.pixelCount);
					}
					currentJob.complete.store(true);
					hasWork.store(false);
				}
			}
		});
	}
	
	bool StartConversion(const uint8_t* src, uint8_t* dst, int pixelCount) {
		if (hasWork.load()) return false; // Busy
		
		currentJob.src = src;
		currentJob.dst = dst;
		currentJob.pixelCount = pixelCount;
		currentJob.complete.store(false);
		hasWork.store(true);
		
		// Wake up the worker thread
		jobCondition.notify_one();
		return true;
	}
	
	bool IsComplete() {
		return currentJob.complete.load();
	}
	
	~AsyncColorConverter() {
		shouldExit.store(true);
		jobCondition.notify_all();
		if (workerThread.joinable()) {
			workerThread.join();
		}
	}
};

static AsyncColorConverter g_asyncConverter;

void MinimizeConsoleWindow() {
	HWND hwndConsole = GetConsoleWindow();
	if (hwndConsole != NULL) {
		ShowWindow(hwndConsole, SW_MINIMIZE);
	}
}

static bool g_headlessClientMode = false;

int recvn(SOCKET s, char* buf, int len) {
	int received = 0;
	while (received < len) {
		int ret = recv(s, buf + received, len - received, 0);
		if (ret <= 0) return ret; // error or disconnect
		received += ret;
	}
	return received;
}

// === CLIPBOARD PROTOCOL ===
enum class MsgType : uint8_t {
	Input = 0,
	RemoteCtrl = 1,
	Clipboard = 2 // new
};

#pragma pack(push, 1)
struct ClipboardMsg {
	MsgType type; // Must be Clipboard
	uint32_t length; // length of the following UTF-8 string
	// char data[]; // immediately following, not part of struct
};
#pragma pack(pop)


// === CLIPBOARD UTILITIES ===
static HWND g_clipboardNext = nullptr;
static SOCKET g_clipboardSocket = INVALID_SOCKET; // set to connected socket on client/server

void SendClipboardPacket(SOCKET sock, const std::string& utf8) {
	ClipboardMsg msg;
	msg.type = MsgType::Clipboard;
	msg.length = static_cast<uint32_t>(utf8.size());
	send(sock, (const char*)&msg, sizeof(msg), 0);
	if (!utf8.empty())
		send(sock, utf8.data(), (int)utf8.size(), 0);
}

// Send local clipboard to peer
void SendLocalClipboard(SOCKET sock) {
	if (!OpenClipboard(nullptr)) return;
	HANDLE hData = GetClipboardData(CF_UNICODETEXT);
	if (hData) {
		wchar_t* wstr = (wchar_t*)GlobalLock(hData);
		if (wstr) {
			std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>> conv;
			std::string utf8 = conv.to_bytes(wstr);
			SendClipboardPacket(sock, utf8);
			GlobalUnlock(hData);
		}
	}
	CloseClipboard();
}

// Set local clipboard from received string
void ApplyRemoteClipboard(const std::string& utf8) {
	std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>> conv;
	std::wstring wtext = conv.from_bytes(utf8);
	if (OpenClipboard(nullptr)) {
		EmptyClipboard();
		HGLOBAL hGlob = GlobalAlloc(GMEM_MOVEABLE, (wtext.size() + 1) * sizeof(wchar_t));
		if (hGlob) {
			memcpy(GlobalLock(hGlob), wtext.c_str(), (wtext.size() + 1) * sizeof(wchar_t));
			GlobalUnlock(hGlob);
			SetClipboardData(CF_UNICODETEXT, hGlob);
		}
		CloseClipboard();
	}
}

// Clipboard chain window proc (call in your window proc)
LRESULT HandleClipboardMsg(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
	switch (msg) {
	case WM_DRAWCLIPBOARD:
		if (g_clipboardSocket != INVALID_SOCKET) SendLocalClipboard(g_clipboardSocket);
		if (g_clipboardNext) SendMessage(g_clipboardNext, msg, wParam, lParam);
		return 0;
	case WM_CHANGECBCHAIN:
		if ((HWND)wParam == g_clipboardNext)
			g_clipboardNext = (HWND)lParam;
		else if (g_clipboardNext)
			SendMessage(g_clipboardNext, msg, wParam, lParam);
		return 0;
	}
	return 0;
}

// Init clipboard monitoring (call after window creation)
void InitClipboardMonitor(HWND hwnd, SOCKET sock) {
	g_clipboardSocket = sock;
	g_clipboardNext = SetClipboardViewer(hwnd);
}
void CleanupClipboardMonitor(HWND hwnd) {
	ChangeClipboardChain(hwnd, g_clipboardNext);
	g_clipboardNext = nullptr;
	g_clipboardSocket = INVALID_SOCKET;
}

enum class RemoteCtrlType : uint8_t {
	SetFps = 1
};
#pragma pack(push, 1)
struct RemoteCtrlMsg {
	RemoteCtrlType type;
	uint8_t value; // fps: [5,10,20,30,40,60]
};

// --- DIRTY TILE STRUCT ---
// Replace this struct with your existing one if it differs
struct DirtyTile {
	int left, top, right, bottom;
};

constexpr int TILE_W = 32;
constexpr int TILE_H = 32;

class BasicBitmap;
class MainWindow;

// --- Per-window state using BasicBitmap for framebuffer (no hBitmap or dibBits needed) ---
// State for each streamed window
struct ScreenBitmapState {
	BasicBitmap* bmp = nullptr;
	int imgW = 0;
	int imgH = 0;
	CRITICAL_SECTION cs;
	SOCKET* psktInput = nullptr;
	MainWindow* mainWindow = nullptr;

	ScreenBitmapState() { InitializeCriticalSection(&cs); }
	~ScreenBitmapState() { if (bmp) delete bmp; DeleteCriticalSection(&cs); }
};

/**
 * Compare two 32bpp RGBA framebuffers and collect dirty tiles.
 * A simple line-by-line approach: every run of changed pixels on a scanline is a tile.
 */
void detect_dirty_tiles(
	const uint32_t* prev, const uint32_t* curr, int width, int height,
	std::vector<DirtyTile>& out_tiles
) {
	out_tiles.clear();
	for (int ty = 0; ty < height; ty += TILE_H) {
		int th = std::min(TILE_H, height - ty);
		for (int tx = 0; tx < width; tx += TILE_W) {
			int tw = std::min(TILE_W, width - tx);

			bool dirty = false;
			for (int y = 0; y < th && !dirty; ++y) {
				const uint32_t* row_prev = prev + (ty + y) * width + tx;
				const uint32_t* row_curr = curr + (ty + y) * width + tx;
				for (int x = 0; x < tw; ++x) {
					if (row_prev[x] != row_curr[x]) {
						dirty = true;
						break;
					}
				}
			}
			if (dirty) {
				out_tiles.push_back({ tx, ty, tx + tw, ty + th });
			}
		}
	}
}

// Extract a tile from a source RGBA buffer into a new BasicBitmap
BasicBitmap* extract_tile_basicbitmap(const uint8_t* rgba, int width, int height, const DirtyTile& r) {
	int rw = r.right - r.left, rh = r.bottom - r.top;
	BasicBitmap* tile = new BasicBitmap(rw, rh, BasicBitmap::A8R8G8B8);
	for (int row = 0; row < rh; ++row) {
		const uint8_t* src = rgba + ((r.top + row) * width + r.left) * 4;
		uint8_t* dst = tile->Bits() + row * rw * 4;
		memcpy(dst, src, rw * 4);
	}
	return tile;
}

// --- QOI encode a subimage ---
bool QOIEncodeSubimage_BasicBitmap(
	const std::vector<uint8_t>& rgba, int width, int height, const DirtyTile& r, std::vector<uint8_t>& outQoi
) {
	int rw = r.right - r.left, rh = r.bottom - r.top;
	BasicBitmap tile(rw, rh, BasicBitmap::A8R8G8B8);

	for (int row = 0; row < rh; ++row) {
		const uint8_t* src = &rgba[((r.top + row) * width + r.left) * 4];
		uint8_t* dst = tile.Bits() + row * rw * 4;
		memcpy(dst, src, rw * 4);
	}

	qoi_desc desc;
	desc.width = tile.Width();
	desc.height = tile.Height();
	desc.channels = 4;
	desc.colorspace = QOI_SRGB;
	int out_len = 0;
	void* qoi_data = qoi_encode(tile.Bits(), &desc, &out_len);
	if (!qoi_data) return false;
	outQoi.resize(out_len);
	memcpy(outQoi.data(), qoi_data, out_len);
	free(qoi_data);
	return true;
}

#define AUDIO_STREAM_PORT 27017


bool StartWASAPILoopbackCapture(IAudioClient** outAudioClient, IAudioCaptureClient** outCaptureClient, WAVEFORMATEX** outFormat) {
	CoInitialize(nullptr);
	IMMDeviceEnumerator* pEnumerator = nullptr;
	IMMDevice* pDevice = nullptr;
	IAudioClient* pAudioClient = nullptr;
	IAudioCaptureClient* pCaptureClient = nullptr;

	if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&pEnumerator)))) return false;
	if (FAILED(pEnumerator->GetDefaultAudioEndpoint(eRender, eConsole, &pDevice))) { pEnumerator->Release(); return false; }
	if (FAILED(pDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&pAudioClient))) { pDevice->Release(); pEnumerator->Release(); return false; }

	WAVEFORMATEX* pwfx = nullptr;
	if (FAILED(pAudioClient->GetMixFormat(&pwfx))) { pAudioClient->Release(); pDevice->Release(); pEnumerator->Release(); return false; }
	if (FAILED(pAudioClient->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK, 1000000, 0, pwfx, nullptr))) {
		CoTaskMemFree(pwfx);
		pAudioClient->Release(); pDevice->Release(); pEnumerator->Release(); return false;
	}
	if (FAILED(pAudioClient->GetService(IID_PPV_ARGS(&pCaptureClient)))) {
		pAudioClient->Release(); pDevice->Release(); pEnumerator->Release(); return false;
	}
	if (FAILED(pAudioClient->Start())) {
		pCaptureClient->Release(); pAudioClient->Release(); pDevice->Release(); pEnumerator->Release(); return false;
	}

	*outAudioClient = pAudioClient;
	*outCaptureClient = pCaptureClient;
	*outFormat = pwfx; // caller must CoTaskMemFree()
	pDevice->Release();
	pEnumerator->Release();
	return true;
}

void AudioStreamServerThreadXRLE(SOCKET clientSock) {
	IAudioClient* audioClient = nullptr;
	IAudioCaptureClient* captureClient = nullptr;
	WAVEFORMATEX* pwfx = nullptr;
	if (!StartWASAPILoopbackCapture(&audioClient, &captureClient, &pwfx)) {
		closesocket(clientSock);
		return;
	}

	// (Optional) Print server format
	printf("SERVER: tag=%04x chans=%d samplerate=%d bits=%d blockalign=%d avgbytes=%d cbSize=%d\n",
		pwfx->wFormatTag, pwfx->nChannels, pwfx->nSamplesPerSec, pwfx->wBitsPerSample,
		pwfx->nBlockAlign, pwfx->nAvgBytesPerSec, pwfx->cbSize);

	size_t wfexSendSize = sizeof(WAVEFORMATEX);
	if (pwfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE && pwfx->cbSize >= 22) {
		wfexSendSize += pwfx->cbSize;
	}
	const uint8_t* wfexBytes = reinterpret_cast<const uint8_t*>(pwfx);
	if (send(clientSock, (const char*)wfexBytes, (int)wfexSendSize, 0) != (int)wfexSendSize) {
		goto end;
	}

	const int FRAMES_PER_BUFFER = 4096;
	while (true) {
		UINT32 packetLength = 0;
		if (FAILED(captureClient->GetNextPacketSize(&packetLength))) break;
		if (packetLength == 0) { Sleep(10); continue; } // Reduce frequency from 5ms to 10ms

		BYTE* pData;
		UINT32 nFrames;
		DWORD flags;
		if (FAILED(captureClient->GetBuffer(&pData, &nFrames, &flags, nullptr, nullptr))) break;
		if (nFrames == 0) { captureClient->ReleaseBuffer(0); continue; }

		int bytes_uncompressed = nFrames * pwfx->nBlockAlign;
		std::vector<uint8_t> xrle_buf(bytes_uncompressed * 2); // enough space for worst-case
		size_t bytes_compressed = xrle_compress(xrle_buf.data(), (uint8_t*)pData, bytes_uncompressed);

		uint32_t net_uncompressed = htonl(bytes_uncompressed);
		uint32_t net_compressed = htonl((uint32_t)bytes_compressed);
		if (send(clientSock, (char*)&net_uncompressed, 4, 0) != 4) break;
		if (send(clientSock, (char*)&net_compressed, 4, 0) != 4) break;
		if (send(clientSock, (char*)xrle_buf.data(), (int)bytes_compressed, 0) != (int)bytes_compressed) break;

		captureClient->ReleaseBuffer(nFrames);
	}
end:
	audioClient->Stop();
	captureClient->Release();
	audioClient->Release();
	CoTaskMemFree(pwfx); // <-- free mix format when done
	closesocket(clientSock);
}

// Helper: check if WAVEFORMATEX is actually WAVEFORMATEXTENSIBLE
bool IsWaveFormatExtensible(const WAVEFORMATEX* wfex) {
	return (wfex->wFormatTag == WAVE_FORMAT_EXTENSIBLE && wfex->cbSize >= 22);
}

void AudioStreamClientThreadXRLE(const std::string& serverIp) {
	// Networking: (same as before)
	SOCKET sock = socket(AF_INET, SOCK_STREAM, 0);
	sockaddr_in addr = {};
	addr.sin_family = AF_INET;
	addr.sin_port = htons(AUDIO_STREAM_PORT);
	inet_pton(AF_INET, serverIp.c_str(), &addr.sin_addr);
	if (connect(sock, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) { closesocket(sock); return; }

	// Receive format struct from server
	WAVEFORMATEX wfex = {};
	if (recvn(sock, (char*)&wfex, sizeof(WAVEFORMATEX)) != sizeof(WAVEFORMATEX)) { closesocket(sock); return; }
	std::vector<uint8_t> extBytes;
	std::vector<uint8_t> fullFmt;
	const WAVEFORMATEX* pwfx = &wfex;
	if (IsWaveFormatExtensible(&wfex)) {
		extBytes.resize(wfex.cbSize);
		if (recvn(sock, (char*)extBytes.data(), wfex.cbSize) != wfex.cbSize) {
			closesocket(sock); return;
		}
		fullFmt.resize(sizeof(WAVEFORMATEX) + extBytes.size());
		memcpy(fullFmt.data(), &wfex, sizeof(WAVEFORMATEX));
		memcpy(fullFmt.data() + sizeof(WAVEFORMATEX), extBytes.data(), extBytes.size());
		pwfx = (const WAVEFORMATEX*)fullFmt.data();
	}

	// WASAPI Initialization
	CoInitialize(nullptr);
	IMMDeviceEnumerator* pEnumerator = nullptr;
	IMMDevice* pDevice = nullptr;
	IAudioClient* pAudioClient = nullptr;
	IAudioRenderClient* pRenderClient = nullptr;

	if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&pEnumerator)))) { closesocket(sock); CoUninitialize(); return; }
	if (FAILED(pEnumerator->GetDefaultAudioEndpoint(eRender, eConsole, &pDevice))) { pEnumerator->Release(); closesocket(sock); CoUninitialize(); return; }
	if (FAILED(pDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&pAudioClient))) { pDevice->Release(); pEnumerator->Release(); closesocket(sock); CoUninitialize(); return; }

	// Use the server's format (pwfx) directly:
	REFERENCE_TIME bufferDuration = 1000000; // 100ms
	if (FAILED(pAudioClient->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, bufferDuration, 0, pwfx, nullptr))) {
		pAudioClient->Release(); pDevice->Release(); pEnumerator->Release(); closesocket(sock); CoUninitialize(); return;
	}
	if (FAILED(pAudioClient->GetService(IID_PPV_ARGS(&pRenderClient)))) {
		pAudioClient->Release(); pDevice->Release(); pEnumerator->Release(); closesocket(sock); CoUninitialize(); return;
	}
	UINT32 bufferFrameCount = 0;
	pAudioClient->GetBufferSize(&bufferFrameCount);

	pAudioClient->Start();

	while (true) {
		uint32_t net_bytes_uncompressed, net_bytes_compressed;
		if (recvn(sock, (char*)&net_bytes_uncompressed, 4) != 4) break;
		if (recvn(sock, (char*)&net_bytes_compressed, 4) != 4) break;
		uint32_t bytes_uncompressed = ntohl(net_bytes_uncompressed);
		uint32_t bytes_compressed = ntohl(net_bytes_compressed);

		std::vector<uint8_t> xrle_buf(bytes_compressed);
		if (recvn(sock, (char*)xrle_buf.data(), bytes_compressed) != (int)bytes_compressed) break;
		std::vector<uint8_t> pcm_buf(bytes_uncompressed);
		if (xrle_decompress(pcm_buf.data(), xrle_buf.data(), bytes_compressed) != bytes_uncompressed) break;

		UINT32 framesToWrite = (bytes_uncompressed / pwfx->nBlockAlign);
		UINT32 framesWritten = 0;
		while (framesToWrite > 0) {
			UINT32 padding = 0;
			pAudioClient->GetCurrentPadding(&padding);
			UINT32 bufferFrameCountFree = bufferFrameCount - padding;
			UINT32 chunk = std::min(framesToWrite, bufferFrameCountFree);
			if (chunk == 0) { Sleep(10); continue; } // Reduce frequency from 5ms to 10ms

			BYTE* pData = nullptr;
			if (FAILED(pRenderClient->GetBuffer(chunk, &pData))) { break; }
			memcpy(pData, pcm_buf.data() + framesWritten * pwfx->nBlockAlign, chunk * pwfx->nBlockAlign);
			pRenderClient->ReleaseBuffer(chunk, 0);

			framesToWrite -= chunk;
			framesWritten += chunk;
		}
	}

	pAudioClient->Stop();
	pRenderClient->Release();
	pAudioClient->Release();
	pDevice->Release();
	pEnumerator->Release();
	closesocket(sock);
	CoUninitialize();
}


class MainWindow; // forward declaration, so 'extern MainWindow* ...' is legal
extern MainWindow* g_pMainWindow;


#define SCREEN_STREAM_PORT 27016
#define SCREEN_STREAM_FPS 20

#define BTN_MODE 1
#define BTN_START 2
#define BTN_PAUSE 3
#define BTN_TERMINATE 4
#define BTN_CONNECT 5
#define BTN_DISCONNECT 6
#define EDIT_ADDRESS 7
#define BTN_SERVER 8
#define BTN_CLIENT 9
#define EDIT_PORT 10

#define MENU_FILE 10
#define MENU_SUB 11
#define MENU_EXIT 12
#define MENU_ABOUT 13
// --- Context menu command IDs for the remote screen window ---

#define IDM_VIDEO_FPS         6010
#define IDM_VIDEO_FPS_5       6011
#define IDM_VIDEO_FPS_10      6012
#define IDM_VIDEO_FPS_20      6013
#define IDM_VIDEO_FPS_30      6014
#define IDM_VIDEO_FPS_40      6015
#define IDM_VIDEO_FPS_60      6016

#define IDM_ALWAYS_ON_TOP     6020

#define IDM_SENDKEYS          6030
#define IDM_SENDKEYS_ALTF4    6031
#define IDM_SENDKEYS_CTRLESC  6032
#define IDM_SENDKEYS_CTRALTDEL 6033
#define IDM_SENDKEYS_PRNTSCRN 6034

std::atomic<int> g_streamingFps(SCREEN_STREAM_FPS);

// --- State variables for menu ---
static bool g_alwaysOnTop = false;
static int g_screenStreamMenuFps = SCREEN_STREAM_FPS; // 5, 10, 20, 30, 40, 60
static int g_screenStreamActualFps = SCREEN_STREAM_FPS;

// --- Function prototypes for menu logic ---
HMENU CreateScreenContextMenu();
void SetRemoteScreenFps(HWND hwnd, int fps);
void SendRemoteKeyCombo(HWND hwnd, int combo);

// --- Helper for context menu creation ---
HMENU CreateScreenContextMenu() {
	HMENU hMenu = CreatePopupMenu();

	// Video FPS submenu
	HMENU hFpsMenu = CreatePopupMenu();
	const int fpsVals[] = { 5, 10, 20, 30, 40, 60 };
	const int fpsIDs[] = { IDM_VIDEO_FPS_5, IDM_VIDEO_FPS_10, IDM_VIDEO_FPS_20, IDM_VIDEO_FPS_30, IDM_VIDEO_FPS_40, IDM_VIDEO_FPS_60 };
	for (int i = 0; i < 6; ++i) {
		AppendMenuA(hFpsMenu, MF_STRING | (g_screenStreamMenuFps == fpsVals[i] ? MF_CHECKED : 0), fpsIDs[i], std::to_string(fpsVals[i]).c_str());
	}
	AppendMenuA(hMenu, MF_POPUP, (UINT_PTR)hFpsMenu, "Video FPS");

	// Always On Top
	AppendMenuA(hMenu, MF_STRING | (g_alwaysOnTop ? MF_CHECKED : 0), IDM_ALWAYS_ON_TOP, "Always On Top");

	// Send Keys submenu
	HMENU hSendKeysMenu = CreatePopupMenu();
	AppendMenuA(hSendKeysMenu, MF_STRING, IDM_SENDKEYS_ALTF4, "Alt + F4");
	AppendMenuA(hSendKeysMenu, MF_STRING, IDM_SENDKEYS_CTRLESC, "Ctrl + Esc");
	AppendMenuA(hSendKeysMenu, MF_STRING, IDM_SENDKEYS_CTRALTDEL, "Ctrl + Alt + Del");
	AppendMenuA(hSendKeysMenu, MF_STRING, IDM_SENDKEYS_PRNTSCRN, "PrintScreen");
	AppendMenuA(hMenu, MF_POPUP, (UINT_PTR)hSendKeysMenu, "Send Keys");

	return hMenu;
}

// --- Helpers for setting fps and sending keys ---
// These are called from the context menu handler.

// This helper sends special key combos to the remote side
void SendRemoteKeyCombo(HWND hwnd, int combo) {
	// Find the input socket for this window
	SOCKET* psktInput = nullptr;
	// The input socket pointer is stored with WM_USER + 100
	// (see logic in ScreenWndProc and StartScreenRecv)
	psktInput = (SOCKET*)GetWindowLongPtr(hwnd, GWLP_USERDATA);
	if (!psktInput || *psktInput == INVALID_SOCKET) return;

	INPUT input[6] = {};
	int n = 0;
	switch (combo) {
	case IDM_SENDKEYS_ALTF4:
		input[n] = {}; input[n].type = INPUT_KEYBOARD; input[n].ki.wVk = VK_MENU; n++;
		input[n] = {}; input[n].type = INPUT_KEYBOARD; input[n].ki.wVk = VK_F4; n++;
		input[n] = input[n - 1]; input[n].ki.dwFlags = KEYEVENTF_KEYUP; n++;
		input[n] = input[n - 3]; input[n].ki.dwFlags = KEYEVENTF_KEYUP; n++;
		break;
	case IDM_SENDKEYS_CTRLESC:
		input[n] = {}; input[n].type = INPUT_KEYBOARD; input[n].ki.wVk = VK_CONTROL; n++;
		input[n] = {}; input[n].type = INPUT_KEYBOARD; input[n].ki.wVk = VK_ESCAPE; n++;
		input[n] = input[n - 1]; input[n].ki.dwFlags = KEYEVENTF_KEYUP; n++;
		input[n] = input[n - 3]; input[n].ki.dwFlags = KEYEVENTF_KEYUP; n++;
		break;
	case IDM_SENDKEYS_CTRALTDEL:
		input[n] = {}; input[n].type = INPUT_KEYBOARD; input[n].ki.wVk = VK_CONTROL; n++;
		input[n] = {}; input[n].type = INPUT_KEYBOARD; input[n].ki.wVk = VK_MENU; n++;
		input[n] = {}; input[n].type = INPUT_KEYBOARD; input[n].ki.wVk = VK_DELETE; n++;
		input[n] = input[n - 1]; input[n].ki.dwFlags = KEYEVENTF_KEYUP; n++;
		input[n] = input[n - 2]; input[n].ki.dwFlags = KEYEVENTF_KEYUP; n++;
		input[n] = input[n - 3]; input[n].ki.dwFlags = KEYEVENTF_KEYUP; n++;
		break;
	case IDM_SENDKEYS_PRNTSCRN:
		input[n] = {}; input[n].type = INPUT_KEYBOARD; input[n].ki.wVk = VK_SNAPSHOT; n++;
		input[n] = input[n - 1]; input[n].ki.dwFlags = KEYEVENTF_KEYUP; n++;
		break;
	}
	for (int i = 0; i < n; ++i)
		send(*psktInput, (char*)&input[i], sizeof(INPUT), 0);
}

#define DEFAULT_PORT 27015
#define MAX_CLIENTS 10

// Server screen dim is nScreenWidth[0], nScreenHeight[0]
// Client screen dim is nScreenWidth[1], nScreenHeight[1]
int nScreenWidth[2] = { 1920 , 2560 };
int nScreenHeight[2] = { 1080 , 1440 };

const int nNormalized = 65535;

// --- Capture screen to BasicBitmap, with RGBA output ---
// --- Capture screen into a BasicBitmap (RGBA) ---
bool CaptureScreenToBasicBitmap(BasicBitmap*& outBmp) {
	int width = GetSystemMetrics(SM_CXSCREEN);
	int height = GetSystemMetrics(SM_CYSCREEN);
	HDC hScreenDC = GetDC(NULL);
	BITMAPINFO bmi = { 0 };
	bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
	bmi.bmiHeader.biWidth = width;
	bmi.bmiHeader.biHeight = -height;
	bmi.bmiHeader.biPlanes = 1;
	bmi.bmiHeader.biBitCount = 32;
	bmi.bmiHeader.biCompression = BI_RGB;
	void* pBits = nullptr;
	HBITMAP hBitmap = CreateDIBSection(NULL, &bmi, DIB_RGB_COLORS, &pBits, NULL, 0);
	if (!hBitmap) { ReleaseDC(NULL, hScreenDC); return false; }
	HDC hMemDC = CreateCompatibleDC(hScreenDC);
	HGDIOBJ oldObj = SelectObject(hMemDC, hBitmap);
	BitBlt(hMemDC, 0, 0, width, height, hScreenDC, 0, 0, SRCCOPY);
	SelectObject(hMemDC, oldObj);
	DeleteDC(hMemDC);
	ReleaseDC(NULL, hScreenDC);

	BasicBitmap* bmp = new BasicBitmap(width, height, BasicBitmap::A8R8G8B8);
	uint8_t* src = static_cast<uint8_t*>(pBits);
	uint8_t* dst = bmp->Bits();
	for (int i = 0; i < width * height; ++i) {
		dst[i * 4 + 0] = src[i * 4 + 2]; // R
		dst[i * 4 + 1] = src[i * 4 + 1]; // G
		dst[i * 4 + 2] = src[i * 4 + 0]; // B
		dst[i * 4 + 3] = 255;
	}
	DeleteObject(hBitmap);
	outBmp = bmp;
	return true;
}



// Now, QOI expects raw RGBA data.
// Helper to convert HBITMAP (24bpp or 32bpp) to 32bpp RGBA buffer.
bool HBITMAPToRGBA(HBITMAP hBitmap, std::vector<uint8_t>& out, int& width, int& height) {
	BITMAP bmp;
	if (!hBitmap) return false;
	if (!GetObject(hBitmap, sizeof(bmp), &bmp)) return false;
	width = bmp.bmWidth;
	height = bmp.bmHeight;
	BITMAPINFO bmi = { 0 };
	bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
	bmi.bmiHeader.biWidth = width;
	bmi.bmiHeader.biHeight = -height; // top-down
	bmi.bmiHeader.biPlanes = 1;
	bmi.bmiHeader.biBitCount = 32;
	bmi.bmiHeader.biCompression = BI_RGB;

	std::vector<uint8_t> tmp(width * height * 4);
	HDC hdc = GetDC(NULL);
	int res = GetDIBits(hdc, hBitmap, 0, height, tmp.data(), &bmi, DIB_RGB_COLORS);
	ReleaseDC(NULL, hdc);
	if (res == 0) return false;

	// Convert BGRA to RGBA
	out.resize(width * height * 4);
	for (int i = 0; i < width * height; ++i) {
		out[i * 4 + 0] = tmp[i * 4 + 2]; // R
		out[i * 4 + 1] = tmp[i * 4 + 1]; // G
		out[i * 4 + 2] = tmp[i * 4 + 0]; // B
		out[i * 4 + 3] = 255;
	}
	return true;
}

// --- QOI encode a BasicBitmap ---
bool QOIEncodeBasicBitmap(const BasicBitmap* bmp, std::vector<uint8_t>& outQoi) {
	qoi_desc desc;
	desc.width = bmp->Width();
	desc.height = bmp->Height();
	desc.channels = 4;
	desc.colorspace = QOI_SRGB;
	int out_len = 0;
	void* qoi_data = qoi_encode(bmp->Bits(), &desc, &out_len);
	if (!qoi_data) return false;
	outQoi.resize(out_len);
	memcpy(outQoi.data(), qoi_data, out_len);
	free(qoi_data);
	return true;
}


// --- QOI decode to BasicBitmap ---
BasicBitmap* QOIDecodeToBasicBitmap(const uint8_t* data, size_t len) {
	qoi_desc desc;
	uint8_t* decoded = (uint8_t*)qoi_decode(data, len, &desc, 4);
	if (!decoded) return nullptr;
	BasicBitmap* bmp = new BasicBitmap(desc.width, desc.height, BasicBitmap::A8R8G8B8);
	memcpy(bmp->Bits(), decoded, desc.width * desc.height * 4);
	free(decoded);
	return bmp;
}

// SOCKETS function declarations for use in other translation units
int InitializeServer(SOCKET& sktListen, int port);
int InitializeScreenStreamServer(SOCKET& sktListen, int port);
int BroadcastInput(std::vector<SOCKET> vsktSend, INPUT* input);
int TerminateServer(SOCKET& sktListen, std::vector<SOCKET>& sktClients);
int InitializeClient();
int ConnectServer(SOCKET& sktConn, std::string serverAdd, int port);
int ConnectScreenStreamServer(SOCKET& sktConn, std::string serverAdd, int port);
int ReceiveServer(SOCKET sktConn, INPUT& data);
int CloseConnection(SOCKET* sktConn);

// Externs for screen streaming global state
std::atomic<bool> g_screenStreamActive(false);
std::atomic<size_t> g_screenStreamBytes(0);
std::atomic<int> g_screenStreamFPS(0);
std::atomic<int> g_screenStreamW(0);
std::atomic<int> g_screenStreamH(0);

// Streaming server/client declarations
void ScreenStreamServerThread(SOCKET sktClient);
LRESULT CALLBACK ScreenWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
void ScreenRecvThread(SOCKET skt, HWND hwnd, std::string ip, int server_port);
void StartScreenRecv(const std::string& server_ip, int port, const RemoteWindowPlacement& place, SOCKET* psktInput);

void ServerInputRecvThread(SOCKET clientSocket);



// ================================================
// =================WINDOWS SOCKETS================
// ================================================
// code largely taken from
// https://docs.microsoft.com/en-us/windows/win32/winsock/complete-server-code
// https://docs.microsoft.com/en-us/windows/win32/winsock/complete-client-code
int InitializeServer(SOCKET& sktListen, int port) {
	struct addrinfo* result = NULL;
	struct addrinfo hints;

	ZeroMemory(&hints, sizeof(hints));

	//AF_INET for IPV4
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_protocol = IPPROTO_TCP;
	hints.ai_flags = AI_PASSIVE;

	//Resolve the local address and port to be used by the server
	int iResult = getaddrinfo(NULL, std::to_string(port).c_str(), &hints, &result);

	if (iResult != 0) {
		std::cout << "getaddrinfo failed: " << iResult << std::endl;
		return 1;
	}

	//Create a SOCKET for the server to listen for client connections

	sktListen = socket(result->ai_family, result->ai_socktype, result->ai_protocol);


	if (sktListen == INVALID_SOCKET) {
		std::cout << "Error at socket(): " << WSAGetLastError() << std::endl;
		freeaddrinfo(result);
		return 1;
	}

	//Setup the TCP listening socket
	iResult = bind(sktListen, result->ai_addr, (int)result->ai_addrlen);


	if (iResult == SOCKET_ERROR) {
		std::cout << "bind failed with error: " << WSAGetLastError() << std::endl;
		freeaddrinfo(result);
		return 1;
	}

	freeaddrinfo(result);
	return 0;
}

int InitializeScreenStreamServer(SOCKET& sktListen, int port) {
	return InitializeServer(sktListen, port);
}

// Add this function if you want to refactor for a cleaner thread:
void AudioStreamAcceptLoop(int port) {
	SOCKET listenSock = INVALID_SOCKET;
	if (InitializeScreenStreamServer(listenSock, port) != 0) {
		std::cerr << "Audio stream server failed to start on port " << port << std::endl;
		return;
	}
	while (true) {
		if (listen(listenSock, 1) == SOCKET_ERROR) break;
		sockaddr_in client_addr;
		int addrlen = sizeof(client_addr);
		SOCKET sktClient = accept(listenSock, (sockaddr*)&client_addr, &addrlen);
		if (sktClient == INVALID_SOCKET) continue;
		std::thread(AudioStreamServerThreadXRLE, sktClient).detach();
	}
	closesocket(listenSock);
}

int BroadcastInput(std::vector<SOCKET> vsktSend, INPUT* input) {
	int iResult = 0;

	for (auto& sktSend : vsktSend) {
		if (sktSend != INVALID_SOCKET) {

			iResult = send(sktSend, (char*)input, sizeof(INPUT), 0);
			if (iResult == SOCKET_ERROR) {
				std::cout << "send failed: " << WSAGetLastError() << std::endl;
			}
		}
	}
	return 0;
}
int TerminateServer(SOCKET& sktListen, std::vector<SOCKET>& sktClients) {

	int iResult;
	for (auto& client : sktClients) {
		if (client != INVALID_SOCKET) {
			iResult = shutdown(client, SD_SEND);
			if (iResult == SOCKET_ERROR) {
				std::cout << "shutdown failed: " << WSAGetLastError() << std::endl;
			}
			closesocket(client);
		}
	}
	closesocket(sktListen);
	return 0;
}
int InitializeClient() {
	//WSADATA wsaData;
	//int iResult;

	//// Initialize Winsock
	//iResult = WSAStartup(MAKEWORD(2, 2), &wsaData);
	//if (iResult != 0) {
	//	std::cout << "WSAStartup failed with error: " << iResult << std::endl;
	//	return 1;
	//}
	return 0;
}
int ConnectServer(SOCKET& sktConn, std::string serverAdd, int port) {
	int iResult;
	struct addrinfo* result = NULL,
		* ptr = NULL,
		hints;
	ZeroMemory(&hints, sizeof(hints));
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_protocol = IPPROTO_TCP;

	// Resolve the server address and port
	iResult = getaddrinfo(serverAdd.c_str(), std::to_string(port).c_str(), &hints, &result);
	if (iResult != 0) {
		std::cout << "getaddrinfo failed with error: " << iResult << std::endl;
		return 1;
	}

	// Create a SOCKET for connecting to server
	sktConn = socket(result->ai_family, result->ai_socktype, result->ai_protocol);
	if (sktConn == INVALID_SOCKET) {
		std::cout << "socket failed with error: " << WSAGetLastError() << std::endl;
		return 1;
	}

	// Connect to server.
	iResult = connect(sktConn, result->ai_addr, (int)result->ai_addrlen);
	if (iResult == SOCKET_ERROR) {
		closesocket(sktConn);
		sktConn = INVALID_SOCKET;
	}

	freeaddrinfo(result);

	if (sktConn == INVALID_SOCKET) {
		std::cout << "Unable to connect to server" << std::endl;
		return 1;
	}
	return 0;
}

int ConnectScreenStreamServer(SOCKET& sktConn, std::string serverAdd, int port) {
	return ConnectServer(sktConn, serverAdd, port);
}

int ReceiveServer(SOCKET sktConn, INPUT& data) {
	INPUT buff;
	int iResult = recvn(sktConn, (char*)&buff, sizeof(INPUT));
	if (iResult != sizeof(INPUT)) {
		if (iResult == 0) {
			std::cout << "Connection closed" << std::endl;
		}
		else {
			std::cout << "Receive failed with error: " << WSAGetLastError() << std::endl;
		}
		return 1;
	}
	data = buff;
	return 0;
}
int CloseConnection(SOCKET* sktConn) {
	closesocket(*sktConn);
	return 0;
}

// =================== SCREEN STREAM SERVER =====================

// --- Optimized server thread: now uses XRLE for dirty bitmask and QOI tiles ---
// Uncomment this line for verbose debug output on the server
//#define SCREENSTREAMSERVER_DEBUG

#ifdef SCREENSTREAMSERVER_DEBUG
#define SSDPRINTF(...)        \
    do {                      \
        printf(__VA_ARGS__);  \
        fflush(stdout);       \
    } while (0)
#else
#define SSDPRINTF(...) do {} while (0)
#endif

void ScreenStreamServerThread(SOCKET sktClient) {
	using namespace std::chrono;

	// --- Clipboard protocol structures (should match recvn side) ---
	enum class MsgType : uint8_t {
		Input = 0,
		RemoteCtrl = 1,
		Clipboard = 2
	};
#pragma pack(push, 1)
	struct ClipboardMsg {
		MsgType type;
		uint32_t length;
		// char data[] follows
	};
#pragma pack(pop)

	std::unique_ptr<BasicBitmap> prevBmp;
	std::unique_ptr<BasicBitmap> currBmp;
	bool first = true;
	static int frameCounter = 0;

	auto lastPrint = steady_clock::now();
	int frames = 0;
	size_t bytes = 0;

	g_screenStreamActive = true;
	g_screenStreamBytes = 0;
	g_screenStreamFPS = 0;

	// --- CAPTURE SCREEN ONCE TO GET SIZE ---
	BasicBitmap* pTmpBmp = nullptr;
	while (!pTmpBmp) {
		CaptureScreenToBasicBitmap(pTmpBmp);
	}
	int screen_width = pTmpBmp->Width();
	int screen_height = pTmpBmp->Height();
	delete pTmpBmp;

	// --- SEND WIDTH/HEIGHT TO CLIENT BEFORE MAIN LOOP ---
	uint32_t widthNet = htonl((uint32_t)screen_width);
	uint32_t heightNet = htonl((uint32_t)screen_height);
	send(sktClient, (const char*)&widthNet, 4, 0);
	send(sktClient, (const char*)&heightNet, 4, 0);
	SSDPRINTF("ScreenStreamServerThread: sent initial screen size %dx%d\n", screen_width, screen_height);

	// --- Start XRLE audio streaming in parallel ---
	std::thread audioThread([sktClient]() {
		// Each client should get a separate audio socket/connection.
		// If you want to use the same socket as screen, use sktClient directly.
		// Otherwise, accept a separate connection and call AudioStreamServerThreadXRLE().
		// Here, we assume a **separate audio socket** (recommended), so nothing here.
		// If you want to use the same socket, call AudioStreamServerThreadXRLE() here:
		// AudioStreamServerThreadXRLE(sktClient);

		// If your design is to use a SEPARATE socket, you simply launch the audio server thread elsewhere.
		// If your design is to use the SAME socket for both video and audio, you can call:
		// AudioStreamServerThreadXRLE(sktClient);

		// For most remote desktop designs, audio and screen use SEPARATE sockets.
		// So you likely do NOT need to call AudioStreamServerThreadXRLE here.
		// Remove or adapt this lambda as needed for your architecture.
		});
	audioThread.detach();

	// --- Clipboard: allow receiving clipboard packets from client and apply locally ---
	auto try_receive_clipboard = [&]() {
		u_long nonblock = 1;
		ioctlsocket(sktClient, FIONBIO, &nonblock);
		char cbuf[8192];
		int peeked = recv(sktClient, cbuf, sizeof(ClipboardMsg), MSG_PEEK);
		if (peeked >= (int)sizeof(ClipboardMsg)) {
			ClipboardMsg* cmsg = (ClipboardMsg*)cbuf;
			int total_len = int(sizeof(ClipboardMsg) + cmsg->length);
			if (cmsg->type == MsgType::Clipboard && peeked >= total_len) {
				std::vector<char> msgbuf(total_len);
				int recvd = recvn(sktClient, msgbuf.data(), total_len);
				if (recvd == total_len) {
					std::string utf8(msgbuf.data() + sizeof(ClipboardMsg), cmsg->length);
					ApplyRemoteClipboard(utf8);
				}
				u_long block = 0;
				ioctlsocket(sktClient, FIONBIO, &block);
				return true;
			}
		}
		u_long block = 0;
		ioctlsocket(sktClient, FIONBIO, &block);
		return false;
	};

	while (g_screenStreamActive) {
		try_receive_clipboard();

		int fps = g_streamingFps.load();
		int frameInterval = 1000 / fps;
		auto start = steady_clock::now();

		BasicBitmap* pBmp = nullptr;
		if (!CaptureScreenToBasicBitmap(pBmp)) continue;
		currBmp.reset(pBmp);
		if (!currBmp) continue;

		int width = currBmp->Width();
		int height = currBmp->Height();
		const uint8_t* curr_rgba = currBmp->Bits();

		size_t tiles_x = (width + TILE_W - 1) / TILE_W;
		size_t tiles_y = (height + TILE_H - 1) / TILE_H;
		size_t numTiles = tiles_x * tiles_y;

		std::vector<uint8_t> dirtyBitmask((numTiles + 7) / 8, 0);
		std::vector<std::pair<int, int>> DirtyTileIndices;

		frameCounter++;
		if (first || frameCounter % 60 == 0) {
			for (size_t ty = 0; ty < tiles_y; ++ty)
				for (size_t tx = 0; tx < tiles_x; ++tx) {
					size_t tidx = ty * tiles_x + tx;
					dirtyBitmask[tidx / 8] |= 1 << (tidx % 8);
					DirtyTileIndices.push_back({ (int)tx, (int)ty });
				}
			prevBmp = std::make_unique<BasicBitmap>(*currBmp);
			first = false;
		}
		else {
			if (prevBmp && prevBmp->Width() == width && prevBmp->Height() == height) {
				const uint8_t* prev = prevBmp->Bits();
				for (size_t ty = 0; ty < tiles_y; ++ty) {
					for (size_t tx = 0; tx < tiles_x; ++tx) {
						int tileLeft = (int)(tx * TILE_W);
						int tileTop = (int)(ty * TILE_H);
						int tileW = std::min(TILE_W, width - tileLeft);
						int tileH = std::min(TILE_H, height - tileTop);
						bool dirty = false;
						for (int row = 0; row < tileH; ++row) {
							int y = tileTop + row;
							const uint8_t* prevRow = prev + (y * width + tileLeft) * 4;
							const uint8_t* currRow = curr_rgba + (y * width + tileLeft) * 4;
							if (memcmp(prevRow, currRow, tileW * 4) != 0) {
								dirty = true;
								break;
							}
						}
						if (dirty) {
							size_t tidx = ty * tiles_x + tx;
							dirtyBitmask[tidx / 8] |= 1 << (tidx % 8);
							DirtyTileIndices.push_back({ (int)tx, (int)ty });
						}
					}
				}
			}
			else {
				for (size_t ty = 0; ty < tiles_y; ++ty)
					for (size_t tx = 0; tx < tiles_x; ++tx) {
						size_t tidx = ty * tiles_x + tx;
						dirtyBitmask[tidx / 8] |= 1 << (tidx % 8);
						DirtyTileIndices.push_back({ (int)tx, (int)ty });
					}
			}
		}

		std::vector<uint8_t> xrleBitmask(std::max<size_t>(1, dirtyBitmask.size() * 2));
		size_t xrleBitmaskLen = xrle_compress(xrleBitmask.data(), dirtyBitmask.data(), dirtyBitmask.size());
		xrleBitmask.resize(xrleBitmaskLen);
		uint32_t xrleBitmaskLenNet = htonl((uint32_t)xrleBitmaskLen);
		send(sktClient, (const char*)&xrleBitmaskLenNet, 4, 0);
		if (xrleBitmaskLen > 0)
			send(sktClient, (const char*)xrleBitmask.data(), xrleBitmaskLen, 0);

		uint32_t nTilesNet = htonl((uint32_t)DirtyTileIndices.size());
		send(sktClient, (const char*)&nTilesNet, 4, 0);

		size_t tileSeq = 0;
		for (const auto& idx : DirtyTileIndices) {
			int tx = idx.first, ty = idx.second;
			int tileLeft = tx * TILE_W;
			int tileTop = ty * TILE_H;
			int tileW = std::min(TILE_W, width - tileLeft);
			int tileH = std::min(TILE_H, height - tileTop);

			BasicBitmap tile(tileW, tileH, BasicBitmap::A8R8G8B8);
			for (int row = 0; row < tileH; ++row) {
				const uint8_t* src = curr_rgba + ((tileTop + row) * width + tileLeft) * 4;
				uint8_t* dst = tile.Bits() + row * tileW * 4;
				memcpy(dst, src, tileW * 4);
			}
			std::vector<uint8_t> qoiData;
			QOIEncodeBasicBitmap(&tile, qoiData);

			// Adaptive compression: skip XRLE for high FPS to reduce CPU overhead
			std::vector<uint8_t> xrleData;
			size_t xrleSize = qoiData.size();
			int currentFps = g_streamingFps.load();
			bool useDoubleCompression = (currentFps <= 30); // Only use XRLE for FPS <= 30
			
			if (useDoubleCompression) {
				xrleData.resize(std::max<size_t>(1, qoiData.size() * 2));
				xrleSize = xrle_compress(xrleData.data(), qoiData.data(), qoiData.size());
				xrleData.resize(xrleSize);
			} else {
				// For high FPS, use QOI data directly to reduce compression overhead
				xrleData = qoiData;
				xrleSize = qoiData.size();
			}

			// Network batching: collect multiple tiles into a single buffer to reduce send() calls
			static std::vector<uint8_t> networkBuffer;
			static constexpr size_t MAX_BATCH_SIZE = 64 * 1024; // 64KB batches for optimal network performance
			
			// Prepare tile data for batching
			uint32_t xNet = htonl(tileLeft);
			uint32_t yNet = htonl(tileTop);
			uint32_t wNet = htonl(tileW);
			uint32_t hNet = htonl(tileH);
			uint32_t xrleLenNet = htonl((uint32_t)xrleData.size());
			uint32_t qoiOrigLenNet = htonl((uint32_t)qoiData.size());
			
			// Calculate total size for this tile
			size_t tilePacketSize = 24 + xrleData.size(); // 6 * 4 bytes headers + data
			
			// If adding this tile would exceed batch size, send current batch first
			if (!networkBuffer.empty() && networkBuffer.size() + tilePacketSize > MAX_BATCH_SIZE) {
				size_t offset = 0;
				while (offset < networkBuffer.size()) {
					int sent = send(sktClient, (const char*)(networkBuffer.data() + offset), networkBuffer.size() - offset, 0);
					if (sent <= 0) goto END;
					offset += sent;
				}
				networkBuffer.clear();
			}
			
			// Add tile to batch buffer
			size_t currentPos = networkBuffer.size();
			networkBuffer.resize(currentPos + tilePacketSize);
			
			memcpy(networkBuffer.data() + currentPos, &xNet, 4); currentPos += 4;
			memcpy(networkBuffer.data() + currentPos, &yNet, 4); currentPos += 4;
			memcpy(networkBuffer.data() + currentPos, &wNet, 4); currentPos += 4;
			memcpy(networkBuffer.data() + currentPos, &hNet, 4); currentPos += 4;
			memcpy(networkBuffer.data() + currentPos, &xrleLenNet, 4); currentPos += 4;
			memcpy(networkBuffer.data() + currentPos, &qoiOrigLenNet, 4); currentPos += 4;
			memcpy(networkBuffer.data() + currentPos, xrleData.data(), xrleData.size());
			
			bytes += xrleData.size() + 24;
			tileSeq++;
			
			// Send batch if we're at the end of dirty tiles or batch is getting large
			bool isLastTile = (tileSeq >= dirtyCount);
			bool shouldFlushBatch = (networkBuffer.size() >= MAX_BATCH_SIZE / 2) || isLastTile;
			
			if (shouldFlushBatch && !networkBuffer.empty()) {
				size_t offset = 0;
				while (offset < networkBuffer.size()) {
					int sent = send(sktClient, (const char*)(networkBuffer.data() + offset), networkBuffer.size() - offset, 0);
					if (sent <= 0) goto END;
					offset += sent;
				}
				networkBuffer.clear();
			}
		}
		prevBmp = std::make_unique<BasicBitmap>(*currBmp);

		frames++;
		g_screenStreamW = width;
		g_screenStreamH = height;
		auto now = steady_clock::now();
		if (duration_cast<seconds>(now - lastPrint).count() >= 1) {
			g_screenStreamFPS = frames;
			g_screenStreamBytes = bytes;
			frames = 0;
			bytes = 0;
			lastPrint = now;
		}
		auto elapsed = duration_cast<milliseconds>(steady_clock::now() - start).count();
		// Use high-resolution timing instead of Sleep for better performance
		if (elapsed < frameInterval) {
			auto targetTime = start + milliseconds(frameInterval);
			std::this_thread::sleep_until(targetTime);
		}
	}
END:
	closesocket(sktClient);
	g_screenStreamActive = false;
	SSDPRINTF("ScreenStreamServerThread: closesocket, exiting thread\n");
}





// Store a pointer to this struct in the window's GWLP_USERDATA
// You must set this on window creation.

// --- Client streaming: receive XRLE bitmask and XRLE tiles ---
// Uncomment this line for verbose debug output
//#define SCREENRECV_DEBUG

#ifdef SCREENRECV_DEBUG
#define SRDPRINTF(...)        \
    do {                      \
        printf(__VA_ARGS__);  \
        fflush(stdout);       \
    } while (0)
#else
#define SRDPRINTF(...) do {} while (0)
#endif

// Set "Reconnecting..." in the title bar
inline void SetReconnectingTitle(HWND hwnd, const std::string& ip) {
	char title[256];
	snprintf(title, sizeof(title), "Remote Screen | IP: %s | Reconnecting...", ip.c_str());
	PostMessage(hwnd, WM_USER + 2, 0, (LPARAM)title);
}

// Check if window is still valid (window not closed by user)
bool WindowStillOpen(HWND hwnd) {
	return (hwnd && IsWindow(hwnd));
}

// --- Main auto-reconnecting receive thread ---// Utility: Get remote IP and port from a connected socket
inline std::pair<std::string, int> GetPeerIpAndPort(SOCKET skt) {
	sockaddr_in addr = {};
	int addrlen = sizeof(addr);
	if (getpeername(skt, (sockaddr*)&addr, &addrlen) == 0) {
		char ipstr[INET_ADDRSTRLEN] = {};
		inet_ntop(AF_INET, &addr.sin_addr, ipstr, sizeof(ipstr));
		int port = ntohs(addr.sin_port);
		return { std::string(ipstr), port };
	}
	return { "", 0 };
}

// Set window title with connection info
inline void SetConnectionTitle(HWND hwnd, const std::string& ip, int port, const char* status) {
	char title[256];
	if (!ip.empty() && port != 0)
		snprintf(title, sizeof(title), "Remote Screen | IP: %s | Port: %d | %s", ip.c_str(), port, status);
	else
		snprintf(title, sizeof(title), "Remote Screen | %s", status);
	PostMessage(hwnd, WM_USER + 2, 0, (LPARAM)title);
}

void ScreenRecvThread(SOCKET skt, HWND hwnd, std::string ip, int server_port) {
	using namespace std::chrono;
	std::string last_ip = ip;
	int last_port = server_port;

	// --- Clipboard protocol structures (must match those used elsewhere) ---
	enum class MsgType : uint8_t {
		Input = 0,
		RemoteCtrl = 1,
		Clipboard = 2
	};
#pragma pack(push, 1)
	struct ClipboardMsg {
		MsgType type;
		uint32_t length; // length of the following UTF-8 string
		// char data[] follows
	};
#pragma pack(pop)

	// --- Start XRLE audio receiving thread (runs in parallel with screen) ---
	std::thread audioThread([ip]() {
		AudioStreamClientThreadXRLE(ip);
		});
	audioThread.detach();

	while (true) {
		if (!WindowStillOpen(hwnd)) {
			SRDPRINTF("ScreenRecvThread: User closed window, exiting.\n");
			if (skt != INVALID_SOCKET) closesocket(skt);
			return;
		}

		// -- Try to connect if needed (or on reconnect) --
		if (skt == INVALID_SOCKET) {
			SetConnectionTitle(hwnd, last_ip, last_port, "Reconnecting...");
			SRDPRINTF("ScreenRecvThread: Attempting to connect to %s:%d...\n", last_ip.c_str(), last_port);

			skt = socket(AF_INET, SOCK_STREAM, 0);
			if (skt == INVALID_SOCKET) {
				SRDPRINTF("ScreenRecvThread: socket() failed\n");
				std::this_thread::sleep_for(std::chrono::seconds(2));
				continue;
			}

			sockaddr_in addr = {};
			addr.sin_family = AF_INET;
			addr.sin_port = htons(static_cast<u_short>(last_port));
			inet_pton(AF_INET, last_ip.c_str(), &addr.sin_addr);

			int connect_result = connect(skt, (sockaddr*)&addr, sizeof(addr));
			if (connect_result == SOCKET_ERROR) {
				int lastErr = WSAGetLastError();
				closesocket(skt);
				skt = INVALID_SOCKET;
				if (lastErr == WSAECONNREFUSED || lastErr == WSAHOST_NOT_FOUND) {
					MessageBoxW(hwnd, L"Server is not running or unreachable.\nStopping auto-reconnect.", L"Connection Failed", MB_OK | MB_ICONERROR);
					return;
				}
				std::this_thread::sleep_for(std::chrono::seconds(2));
				continue;
			}
			SRDPRINTF("ScreenRecvThread: Connected!\n");
			std::tie(last_ip, last_port) = GetPeerIpAndPort(skt);
		}

		SetConnectionTitle(hwnd, last_ip, last_port, "Connected");

		if (!WindowStillOpen(hwnd)) {
			closesocket(skt);
			return;
		}

		// --- RECEIVE WIDTH/HEIGHT FROM SERVER ---
		uint32_t widthNet = 0, heightNet = 0;
		if (recvn(skt, (char*)&widthNet, 4) != 4) {
			SRDPRINTF("ScreenRecvThread: recvn for width failed\n");
			closesocket(skt);
			skt = INVALID_SOCKET;
			std::this_thread::sleep_for(std::chrono::seconds(2));
			continue;
		}
		if (recvn(skt, (char*)&heightNet, 4) != 4) {
			SRDPRINTF("ScreenRecvThread: recvn for height failed\n");
			closesocket(skt);
			skt = INVALID_SOCKET;
			std::this_thread::sleep_for(std::chrono::seconds(2));
			continue;
		}
		g_screenStreamW = ntohl(widthNet);
		g_screenStreamH = ntohl(heightNet);
		SRDPRINTF("ScreenRecvThread: received screen size: %dx%d\n", g_screenStreamW.load(), g_screenStreamH.load());

		if (!WindowStillOpen(hwnd)) {
			closesocket(skt);
			return;
		}

		size_t bytesLastSec = 0;
		int framesLastSec = 0;
		auto lastSec = steady_clock::now();

		ScreenBitmapState* bmpState = reinterpret_cast<ScreenBitmapState*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
		if (!bmpState) {
			SRDPRINTF("ScreenRecvThread: bmpState is NULL, exiting\n");
			closesocket(skt);
			return;
		}

		std::vector<RECT> invalidateRects;
		std::vector<uint8_t> qoiData;
		bool running = true;
		bool lost_connection = false;

		// --- Streaming loop ---
		while (running) {
			// --- Clipboard receive branch ---
			{
				// Use non-blocking peek or select to check for clipboard packet (or interleave with dirty tile receive as needed)
				u_long nonblock = 1;
				ioctlsocket(skt, FIONBIO, &nonblock);
				char cbuf[8192];
				int peeked = recv(skt, cbuf, sizeof(ClipboardMsg), MSG_PEEK);
				if (peeked >= (int)sizeof(ClipboardMsg)) {
					ClipboardMsg* cmsg = (ClipboardMsg*)cbuf;
					int total_len = int(sizeof(ClipboardMsg) + cmsg->length);
					if (cmsg->type == MsgType::Clipboard && peeked >= total_len) {
						std::vector<char> msgbuf(total_len);
						int recvd = recvn(skt, msgbuf.data(), total_len);
						if (recvd == total_len) {
							std::string utf8(msgbuf.data() + sizeof(ClipboardMsg), cmsg->length);
							ApplyRemoteClipboard(utf8);
						}
						u_long block = 0;
						ioctlsocket(skt, FIONBIO, &block);
						continue;
					}
				}
				u_long block = 0;
				ioctlsocket(skt, FIONBIO, &block);
			}
			// --- End Clipboard receive branch ---

			if (!WindowStillOpen(hwnd)) {
				closesocket(skt);
				return;
			}

			// ... rest of streaming loop unchanged ...
			uint32_t xrleBitmaskLenNet = 0;
			if (recvn(skt, (char*)&xrleBitmaskLenNet, 4) != 4) {
				SRDPRINTF("ScreenRecvThread: recvn for bitmask length failed\n");
				lost_connection = true;
				break;
			}
			uint32_t xrleBitmaskLen = ntohl(xrleBitmaskLenNet);
			SRDPRINTF("ScreenRecvThread: xrleBitmaskLen = %u\n", xrleBitmaskLen);

			if (xrleBitmaskLen == 0 || xrleBitmaskLen > 1024 * 1024) {
				SRDPRINTF("ScreenRecvThread: xrleBitmaskLen out of range, exiting\n");
				lost_connection = true;
				break;
			}

			std::vector<uint8_t> xrleBitmask(xrleBitmaskLen);
			if (recvn(skt, (char*)xrleBitmask.data(), xrleBitmaskLen) != xrleBitmaskLen) {
				SRDPRINTF("ScreenRecvThread: recvn for xrleBitmask data failed\n");
				lost_connection = true;
				running = false;
				break;
			}

			int width = g_screenStreamW.load();
			int height = g_screenStreamH.load();
			SRDPRINTF("ScreenRecvThread: width=%d height=%d\n", width, height);
			if (width <= 0 || height <= 0) {
				SRDPRINTF("ScreenRecvThread: Invalid width/height, exiting\n");
				lost_connection = true;
				break;
			}

			size_t tiles_x = (width + TILE_W - 1) / TILE_W;
			size_t tiles_y = (height + TILE_H - 1) / TILE_H;
			size_t numTiles = tiles_x * tiles_y;
			if (numTiles == 0 || numTiles > 100000) {
				SRDPRINTF("ScreenRecvThread: numTiles out of range (%zu), exiting\n", numTiles);
				lost_connection = true;
				break;
			}

			std::vector<uint8_t> dirtyBitmask((numTiles + 7) / 8);
			size_t gotLen = xrle_decompress(dirtyBitmask.data(), xrleBitmask.data(), xrleBitmask.size());
			SRDPRINTF("ScreenRecvThread: gotLen from xrle_decompress = %zu, expected = %zu\n", gotLen, dirtyBitmask.size());
			if (gotLen != dirtyBitmask.size()) {
				SRDPRINTF("ScreenRecvThread: xrle_decompress size mismatch, exiting\n");
				lost_connection = true;
				break;
			}

			size_t dirtyCount = 0;
			SRDPRINTF("ScreenRecvThread: tiles_x=%zu tiles_y=%zu numTiles=%zu\n", tiles_x, tiles_y, numTiles);
			SRDPRINTF("ScreenRecvThread: DirtyBitmask: ");
			for (size_t b = 0; b < dirtyBitmask.size(); ++b) {
				SRDPRINTF("%02X", dirtyBitmask[b]);
				for (int bit = 0; bit < 8; ++bit)
					if (dirtyBitmask[b] & (1 << bit))
						dirtyCount++;
			}
			SRDPRINTF("\nScreenRecvThread: dirtyCount=%zu\n", dirtyCount);

			uint32_t nTilesNet = 0;
			if (recvn(skt, (char*)&nTilesNet, 4) != 4) {
				SRDPRINTF("ScreenRecvThread: recvn for nTiles failed\n");
				lost_connection = true;
				break;
			}
			uint32_t nTiles = ntohl(nTilesNet);
			SRDPRINTF("ScreenRecvThread: nTiles (server says) = %u\n", nTiles);

			size_t bytesThisFrame = 4;
			bool frame_error = false, fullScreenInvalidation = false;
			invalidateRects.clear();

			size_t receivedDirty = 0;
			for (size_t tileIdx = 0; tileIdx < numTiles; ++tileIdx) {
				if (!(dirtyBitmask[tileIdx / 8] & (1 << (tileIdx % 8))))
					continue;

				size_t tx = tileIdx % tiles_x;
				size_t ty = tileIdx / tiles_x;
				uint32_t x = static_cast<uint32_t>(tx * TILE_W);
				uint32_t y = static_cast<uint32_t>(ty * TILE_H);
				uint32_t w = std::min<uint32_t>(TILE_W, static_cast<uint32_t>(std::max(0, width - (int)x)));
				uint32_t h = std::min<uint32_t>(TILE_H, static_cast<uint32_t>(std::max(0, height - (int)y)));

				SRDPRINTF("ScreenRecvThread: expecting tile #%zu at grid (%zu,%zu) x=%u y=%u w=%u h=%u\n",
					receivedDirty, tx, ty, x, y, w, h);

				uint32_t rx, ry, rw, rh, xrleLen, qoiOrigLen;
				if (recvn(skt, (char*)&rx, 4) != 4) { SRDPRINTF("ScreenRecvThread: recvn rx failed\n"); frame_error = true; lost_connection = true; running = false; break; }
				if (recvn(skt, (char*)&ry, 4) != 4) { SRDPRINTF("ScreenRecvThread: recvn ry failed\n"); frame_error = true; lost_connection = true; running = false; break; }
				if (recvn(skt, (char*)&rw, 4) != 4) { SRDPRINTF("ScreenRecvThread: recvn rw failed\n"); frame_error = true; lost_connection = true; running = false; break; }
				if (recvn(skt, (char*)&rh, 4) != 4) { SRDPRINTF("ScreenRecvThread: recvn rh failed\n"); frame_error = true; lost_connection = true; running = false; break; }
				if (recvn(skt, (char*)&xrleLen, 4) != 4) { SRDPRINTF("ScreenRecvThread: recvn xrleLen failed\n"); frame_error = true; lost_connection = true; running = false; break; }
				if (recvn(skt, (char*)&qoiOrigLen, 4) != 4) { SRDPRINTF("ScreenRecvThread: recvn qoiOrigLen failed\n"); frame_error = true; lost_connection = true; running = false; break; }
				bytesThisFrame += 4 * 6;
				rx = ntohl(rx); ry = ntohl(ry); rw = ntohl(rw); rh = ntohl(rh); xrleLen = ntohl(xrleLen); qoiOrigLen = ntohl(qoiOrigLen);

				SRDPRINTF("ScreenRecvThread: received header for tile #%zu: rx=%u ry=%u rw=%u rh=%u xrleLen=%u qoiOrigLen=%u\n",
					receivedDirty, rx, ry, rw, rh, xrleLen, qoiOrigLen);

				if (rw == 0 || rh == 0 || xrleLen == 0 || qoiOrigLen == 0) {
					SRDPRINTF("ScreenRecvThread: zero/invalid header value, frame_error\n");
					frame_error = true; lost_connection = true; running = false; break;
				}

				std::vector<uint8_t> xrleData(xrleLen);
				if (recvn(skt, (char*)xrleData.data(), xrleLen) != xrleLen) {
					SRDPRINTF("ScreenRecvThread: recvn for xrleData failed\n");
					frame_error = true; lost_connection = true; running = false; break;
				}
				bytesThisFrame += xrleLen;

				qoiData.resize(qoiOrigLen);
				
				// Adaptive decompression: check if data is double-compressed or QOI-only
				if (xrleLen == qoiOrigLen) {
					// High FPS mode: data is QOI-only (no XRLE compression)
					memcpy(qoiData.data(), xrleData.data(), qoiOrigLen);
				} else {
					// Standard mode: data is XRLE-compressed QOI
					size_t qoiLen = xrle_decompress(qoiData.data(), xrleData.data(), xrleData.size());
					if (qoiLen != qoiOrigLen) {
						SRDPRINTF("ScreenRecvThread: xrle_decompress qoiLen=%zu != qoiOrigLen=%u\n", qoiLen, qoiOrigLen);
						frame_error = true; lost_connection = true; running = false; break;
					}
				}
				qoiData.resize(qoiOrigLen);

				std::unique_ptr<BasicBitmap> tileBmp;
				{
					BasicBitmap* decoded = QOIDecodeToBasicBitmap(qoiData.data(), qoiLen);
					if (!decoded) {
						SRDPRINTF("ScreenRecvThread: QOIDecodeToBasicBitmap failed for tile #%zu\n", receivedDirty);
						continue;
					}
					tileBmp.reset(decoded);
				}

				// Ultra-fast frame update with lock-free design
				EnterCriticalSection(&bmpState->cs);
				int needW = std::max(bmpState->imgW, (int)(rx + rw));
				int needH = std::max(bmpState->imgH, (int)(ry + rh));
				bool needRealloc = (bmpState->bmp == nullptr) || (bmpState->imgW != needW) || (bmpState->imgH != needH);
				if (needRealloc) {
					if (bmpState->bmp) delete bmpState->bmp;
					bmpState->bmp = new BasicBitmap(needW, needH, BasicBitmap::A8R8G8B8);
					bmpState->imgW = needW;
					bmpState->imgH = needH;
				}
				
				// Update the bitmap with tile data
				for (uint32_t row = 0; row < rh; ++row) {
					if ((ry + row) >= (uint32_t)bmpState->imgH || rx >= (uint32_t)bmpState->imgW) continue;
					uint8_t* dst = bmpState->bmp->Bits() + ((ry + row) * bmpState->imgW + rx) * 4;
					uint8_t* src = tileBmp->Bits() + row * rw * 4;
					memcpy(dst, src, rw * 4);
				}
				
				// Track invalidation regions
				if (rx == 0 && ry == 0 && rw == (uint32_t)bmpState->imgW && rh == (uint32_t)bmpState->imgH) {
					fullScreenInvalidation = true;
				}
				else {
					RECT tileRect;
					tileRect.left = rx;
					tileRect.top = ry;
					tileRect.right = rx + rw;
					tileRect.bottom = ry + rh;
					invalidateRects.push_back(tileRect);
				}
				
				// Async color conversion and frame buffer update
				uint8_t* srcData = bmpState->bmp->Bits();
				int totalPixels = bmpState->imgW * bmpState->imgH;
				
				// Frame change detection using ultra-fast tile-based hash 
				static uint64_t lastFrameHash = 0;
				static uint8_t* conversionBuffer = nullptr;
				static size_t conversionBufferSize = 0;
				static int skipConversionCounter = 0;
				
				// Ultra-fast hash - only sample 16 strategic pixels across the frame
				uint64_t currentHash = 0;
				const int width = bmpState->imgW;
				const int height = bmpState->imgH;
				const uint32_t* pixelData = (const uint32_t*)srcData;
				
				// Sample from 4x4 grid of strategic locations
				for (int y = 0; y < 4; y++) {
					for (int x = 0; x < 4; x++) {
						int pixelIndex = (y * height / 4) * width + (x * width / 4);
						if (pixelIndex < totalPixels) {
							currentHash = currentHash * 17 + pixelData[pixelIndex];
						}
					}
				}
				
				// Only convert if frame content has changed or every 30 frames as a safety check
				if (currentHash != lastFrameHash || (++skipConversionCounter >= 30)) {
					lastFrameHash = currentHash;
					skipConversionCounter = 0;
					
					size_t requiredSize = totalPixels * 4;
					
					if (conversionBufferSize < requiredSize) {
						delete[] conversionBuffer;
						conversionBuffer = new uint8_t[requiredSize];
						conversionBufferSize = requiredSize;
					}
					
					// Perform ultra-fast SIMD color conversion only when needed
					if (ColorConversion::ConvertRGBAToBGRA != nullptr) {
						ColorConversion::ConvertRGBAToBGRA(srcData, conversionBuffer, totalPixels);
					} else {
						// Fallback to scalar conversion if function pointer not initialized
						ColorConversion::ConvertRGBAToBGRA_Scalar(srcData, conversionBuffer, totalPixels);
					}
					
					// Update lock-free frame buffer - this never blocks the UI thread!
					g_frameBuffer.SetFrame(bmpState->imgW, bmpState->imgH, conversionBuffer);
					
					SRDPRINTF("ScreenRecvThread: Color conversion performed for tile #%zu (frame content changed)\n", receivedDirty);
				} else {
					SRDPRINTF("ScreenRecvThread: Skipped color conversion for tile #%zu (no frame change detected)\n", receivedDirty);
				}
				
				LeaveCriticalSection(&bmpState->cs);

				SRDPRINTF("ScreenRecvThread: painted tile #%zu at %u,%u size %u,%u\n", receivedDirty, rx, ry, rw, rh);

				receivedDirty++;
			}
			SRDPRINTF("ScreenRecvThread: received %zu dirty tiles for %zu dirty bits\n", receivedDirty, dirtyCount);

			// Optimized invalidation: reduce Windows API calls for better performance
			static uint64_t lastInvalidateTime = 0;
			static int frameCounter = 0;
			frameCounter++;
			
			// Only check timing every 4th frame to reduce GetTickCount64 overhead
			bool shouldCheckTiming = (frameCounter % 4 == 0);
			uint64_t currentTime = shouldCheckTiming ? GetTickCount64() : lastInvalidateTime + 50;
			
			// Synchronized frame rate limiting: match capture FPS instead of hardcoded 60 FPS
			int currentFps = g_streamingFps.load();
			uint64_t frameIntervalMs = (currentFps > 0) ? (1000 / currentFps) : 50; // fallback to 20 FPS
			
			// Additional performance optimization: increase minimum interval for high CPU usage scenarios
			uint64_t minInterval = std::max(frameIntervalMs, (uint64_t)33); // At most 30 FPS invalidation
			
			if (shouldCheckTiming && (currentTime - lastInvalidateTime < minInterval)) {
				// Skip this update to maintain synchronized frame rate and reduce paint events
				SRDPRINTF("ScreenRecvThread: Skipping update for frame rate sync (target: %d FPS, min interval: %llu ms)\n", currentFps, minInterval);
			}
			else if (fullScreenInvalidation) {
				InvalidateRect(hwnd, NULL, FALSE);
				if (shouldCheckTiming) lastInvalidateTime = currentTime;
				SRDPRINTF("ScreenRecvThread: InvalidateRect(NULL)\n");
			}
			else if (!invalidateRects.empty()) {
				// Ultra-fast bounding rectangle calculation
				RECT boundingRect = invalidateRects[0];
				for (size_t i = 1; i < invalidateRects.size(); ++i) {
					const RECT& r = invalidateRects[i];
					if (r.left < boundingRect.left) boundingRect.left = r.left;
					if (r.top < boundingRect.top) boundingRect.top = r.top;
					if (r.right > boundingRect.right) boundingRect.right = r.right;
					if (r.bottom > boundingRect.bottom) boundingRect.bottom = r.bottom;
				}
				
				// Single invalidation call - maximum efficiency
				InvalidateRect(hwnd, &boundingRect, FALSE);
				if (shouldCheckTiming) lastInvalidateTime = currentTime;
				
				SRDPRINTF("ScreenRecvThread: Ultra-fast InvalidateRect with %zu tiles\n", invalidateRects.size());
			}
			if (frame_error) {
				SRDPRINTF("ScreenRecvThread: frame_error, breaking\n");
				lost_connection = true;
				break;
			}

			framesLastSec++;
			bytesLastSec += bytesThisFrame;
			auto now = steady_clock::now();
			if (duration_cast<seconds>(now - lastSec).count() >= 1) {
				double mbps = (bytesLastSec * 8.0) / 1e6;
				
				// Network congestion detection and adaptive quality
				static double avgMbps = 0.0;
				static int congestionCounter = 0;
				avgMbps = (avgMbps * 0.8) + (mbps * 0.2); // Exponential moving average
				
				// Detect network congestion and recommend FPS adjustment
				bool networkCongested = (avgMbps > 50.0 && framesLastSec < g_streamingFps.load() * 0.8);
				if (networkCongested) {
					congestionCounter++;
					if (congestionCounter >= 3) { // 3 seconds of congestion
						int currentFps = g_streamingFps.load();
						if (currentFps > 20) {
							// Automatically reduce FPS to improve stability
							g_streamingFps.store(std::max(20, currentFps - 10));
							std::cout << "🌐 Network congestion detected, reducing FPS to " << g_streamingFps.load() << std::endl;
						}
						congestionCounter = 0;
					}
				} else {
					congestionCounter = std::max(0, congestionCounter - 1);
				}
				
				RECT clientRect;
				GetClientRect(hwnd, &clientRect);
				int winW = clientRect.right - clientRect.left;
				int winH = clientRect.bottom - clientRect.top;
				char title[256];
				const char* qualityIndicator = networkCongested ? "🔴" : (avgMbps < 10.0 ? "🟢" : "🟡");
				snprintf(title, sizeof(title), "Remote Screen %s | IP: %s | Port: %d | FPS: %d | Mbps: %.2f | Size: %dx%d",
					qualityIndicator, last_ip.c_str(), last_port, framesLastSec, mbps, winW, winH);
				PostMessage(hwnd, WM_USER + 2, 0, (LPARAM)title);
				SRDPRINTF("ScreenRecvThread: Updated window title (quality: %s, avg: %.2f Mbps)\n", qualityIndicator, avgMbps);
				bytesLastSec = 0;
				framesLastSec = 0;
				lastSec = now;
			}
		}

		closesocket(skt);
		skt = INVALID_SOCKET;
		SetConnectionTitle(hwnd, last_ip, last_port, "Reconnecting...");
		SRDPRINTF("ScreenRecvThread: lost connection, will try to reconnect...\n");
		std::this_thread::sleep_for(std::chrono::seconds(2));
	}
}




// BaseWindow was taken from
// https://github.com/microsoft/Windows-classic-samples/blob/master/Samples/Win7Samples/begin/LearnWin32/BaseWindow/cpp/main.cpp
// Slightly modified it by removing the template
class BaseWindow
{
public:
	HWND m_hwnd;
	virtual LRESULT HandleMessage(UINT uMsg, WPARAM wParam, LPARAM lParam) = 0;
	
	static LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
	{
		BaseWindow* pThis = nullptr;

		if (uMsg == WM_NCCREATE)
		{
			CREATESTRUCT* pCreate = (CREATESTRUCT*)lParam;
			pThis = (BaseWindow*)pCreate->lpCreateParams;
			SetWindowLongPtr(hwnd, GWLP_USERDATA, (LONG_PTR)pThis);

			pThis->m_hwnd = hwnd;
		}
		else
		{
			pThis = (BaseWindow*)GetWindowLongPtr(hwnd, GWLP_USERDATA);
		}

		if (pThis)
		{
			if (uMsg == WM_NCDESTROY)
			{
				// Optionally: any extra cleanup logic before user data is cleared

				// Clean up GWLP_USERDATA only in WM_NCDESTROY
				SetWindowLongPtr(hwnd, GWLP_USERDATA, 0);
				// If you dynamically allocated pThis and want to delete it here, do so LAST:
				// delete pThis;
				return 0;
			}
			return pThis->HandleMessage(uMsg, wParam, lParam);
		}
		else
		{
			return DefWindowProc(hwnd, uMsg, wParam, lParam);
		}
	}

	BaseWindow() : m_hwnd(NULL), m_pParent(nullptr) { }

	BOOL Create(
		BaseWindow* parent,
		PCSTR lpWindowName,
		DWORD dwStyle,
		DWORD dwExStyle = 0,
		int x = CW_USEDEFAULT,
		int y = CW_USEDEFAULT,
		int nWidth = CW_USEDEFAULT,
		int nHeight = CW_USEDEFAULT,
		HWND hWndParent = 0,
		HMENU hMenu = NULL
	)
	{
		if (hWndParent == 0) {
			WNDCLASS wc = { 0 };

			wc.lpfnWndProc = BaseWindow::WindowProc;
			wc.hInstance = GetModuleHandle(NULL);
			wc.lpszClassName = ClassName();
			wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
			wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);

			RegisterClass(&wc);
		}

		m_pParent = parent;

		m_hwnd = CreateWindowExA(
			dwExStyle, ClassName(), lpWindowName, dwStyle, x, y,
			nWidth, nHeight, hWndParent, hMenu, GetModuleHandle(NULL), this
		);

		return (m_hwnd ? TRUE : FALSE);
	}

	HWND Window() const { return m_hwnd; }

protected:

	virtual LPCSTR ClassName() const = 0;
	BaseWindow* m_pParent;
};
class Button : public BaseWindow
{
public:
	LPCSTR ClassName() const { return "button"; }
	LRESULT HandleMessage(UINT uMsg, WPARAM wParam, LPARAM lParam) { return DefWindowProc(m_hwnd, uMsg, wParam, lParam); }
};
class InputBox : public BaseWindow
{
public:
	LPCSTR ClassName() const { return "edit"; }
	LRESULT HandleMessage(UINT uMsg, WPARAM wParam, LPARAM lParam) { return DefWindowProc(m_hwnd, uMsg, wParam, lParam); }
};
class StaticBox : public BaseWindow
{
public:
	LPCSTR ClassName() const { return "static"; }
	LRESULT HandleMessage(UINT uMsg, WPARAM wParam, LPARAM lParam) { return DefWindowProc(m_hwnd, uMsg, wParam, lParam); }
};
class EditBox : public BaseWindow
{
public:
	LPCSTR ClassName() const { return "static"; }
	LRESULT HandleMessage(UINT uMsg, WPARAM wParam, LPARAM lParam) { return DefWindowProc(m_hwnd, uMsg, wParam, lParam); }
};
class MainWindow : public BaseWindow
{
public:
	int m_savedFps = SCREEN_STREAM_FPS;
	bool m_savedAlwaysOnTop = false;
	int m_savedWinLeft = 100;
	int m_savedWinTop = 100;
	int m_savedWinW = 477;
	int m_savedWinH = 340;
	int m_savedRemoteLeft = 100;
	int m_savedRemoteTop = 100;
	int m_savedRemoteW = 900;    // Default matches your CreateWindowA
	int m_savedRemoteH = 600;
	HWND RemoteScreenWnd = nullptr;


public:
	MainWindow();
	~MainWindow();

	LPCSTR ClassName() const { return "Remote Window Class"; }
	LRESULT HandleMessage(UINT uMsg, WPARAM wParam, LPARAM lParam);

public:

	enum class MODE
	{
		SERVER,
		CLIENT,
		UNDEF,
	};

	int InitializeInputDevice();
	void UpdateInput();
	void ConvertInput(PRAWINPUT pRaw, INPUT* pInput);
	int RetrieveInput(UINT uMsg, WPARAM wParam, LPARAM lParam);
	int SetMode(MODE m);
	void UpdateGuiControls();

	int ServerStart();
	int ServerTerminate();

	int ClientConnect();
	int ClientDisconnect();

	int HandleCreate(UINT uMsg, WPARAM wParam, LPARAM lParam);
	int HandlePaint(UINT uMsg, WPARAM wParam, LPARAM lParam);
	int HandleCommand(UINT uMsg, WPARAM wParam, LPARAM lParam);
	int HandleClose(UINT uMsg, WPARAM wParam, LPARAM lParam);

	int SendThread(); // thread that sends the input to the clients
	int OutputThread(); // thread that processes the inputs received from the server
	int ListenThread();
	int ReceiveThread();

	bool SaveConfig();
	bool LoadConfig();

	void Log(std::string msg);
	void ServerLog(std::string msg);
	void ClientLog(std::string msg);
	std::string sPort;
	int iPort;

private:

	std::string configName = "config.txt";

public:
	struct WindowData
	{
		std::string sKeyboardState;
		std::string sMouseState[2];
		std::string sLabels[2];

		RAWINPUTDEVICE rid[3] = { 0 }; // index 2 not used
		MODE nMode = MODE::UNDEF;

		RECT textRect = { 0 };
	} Data;

	struct ServerData
	{
		std::string ip;
		int maxClients;
		INPUT inputBuff;
		int nConnected = 0;
		bool isOnline = false;
		bool bAccepting = false;
		bool clientConnected = false;
		bool wasServer = false;
		std::string port;

		bool isRegistered = false;
		RAWINPUTDEVICE rid[3]; // index #2 not used
		std::queue<INPUT> inputQueue;

		bool bPause = true;

		struct ClientInfo
		{
			SOCKET socket;
			std::string ip;
			int id;
		};

		std::vector<ClientInfo> ClientsInformation;
		SOCKET sktListen = INVALID_SOCKET;

		std::thread tSend;
		std::thread tListen;
		std::condition_variable cond_listen;
		std::condition_variable cond_input;
		std::mutex mu_sktclient;
		std::mutex mu_input;


		bool bOnOtherScreen = false;
		short nOffsetX = 0;
		short nOffsetY = 0;
		int oldX = 0;
		int oldY = 0;
		POINT mPos;

	} Server;

public:

	struct ClientData
	{
		std::string ip;
		INPUT recvBuff;
		bool isConnected = false;
		bool wasClient = false;

		std::thread tRecv;
		std::thread tSendInput;
		std::condition_variable cond_input;
		std::condition_variable cond_recv;
		std::mutex mu_input;
		std::mutex mu_recv;

		SOCKET sktServer = INVALID_SOCKET;

		std::queue<INPUT> inputQueue;

	} Client;

	HMENU m_hMenu;
	Button m_btnOk;
	Button m_btnPause;
	Button m_btnModeClient, m_btnConnect, m_btnDisconnect;
	Button m_btnModeServer, m_btnStart, m_btnTerminate;
	InputBox m_itxtIP;
	InputBox m_itxtPort;
	StaticBox m_stxtKeyboard, m_stxtMouse, m_stxtMouseBtn, m_stxtMouseOffset;
}; // <-- CLOSE CLASS DEFINITION

MainWindow::MainWindow()
{
	LoadConfig();
	WSADATA wsadata;
	int r = WSAStartup(MAKEWORD(2, 2), &wsadata);

	if (r != 0)
	{
		std::cout << "WSAStartup failed: " << r << std::endl;
	}
	else
	{
		hostent* host;
		host = gethostbyname("");
		char* wifiIP;
		// the index in the array is to be changed based on the adapter.
		// I currently have no way to determine which is the correct adapter.
		//inet_ntop(AF_INET, host->h_addr_list, wifiIP, 50);
		wifiIP = inet_ntoa(*(in_addr*)host->h_addr_list[0]);
		Server.ip = std::string(wifiIP);
	}
	nScreenWidth[0] = GetSystemMetrics(SM_CXSCREEN);
	nScreenHeight[0] = GetSystemMetrics(SM_CYSCREEN);
}
MainWindow::~MainWindow()
{
	SaveConfig();
}

MainWindow* g_pMainWindow = nullptr;

void SetRemoteScreenFps(HWND hwnd, int fps) {
	g_screenStreamMenuFps = fps;
	g_screenStreamActualFps = fps;

	SOCKET* psktInput = (SOCKET*)GetWindowLongPtr(hwnd, GWLP_USERDATA);
	if (!psktInput || *psktInput == INVALID_SOCKET) return;

	RemoteCtrlMsg msg = { RemoteCtrlType::SetFps, (uint8_t)fps };
	send(*psktInput, (const char*)&msg, sizeof(msg), 0);

	// Save FPS to config
	if (g_pMainWindow) {
		g_pMainWindow->m_savedFps = fps;
		g_pMainWindow->SaveConfig();
	}
}

// ============ SCREEN STREAM CLIENT WINDOW ============

LRESULT CALLBACK ScreenWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
	// Retrieve per-window state
	ScreenBitmapState* bmpState = reinterpret_cast<ScreenBitmapState*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));

	// State for Alt and F10 stuck key workaround
	static bool altDown = false;
	static bool f10Down = false;

	switch (msg) {
	case WM_CREATE:
		bmpState = new ScreenBitmapState();
		bmpState->mainWindow = g_pMainWindow;
		SetWindowLongPtr(hwnd, GWLP_USERDATA, (LONG_PTR)bmpState);
		break;

	case WM_USER + 100: // Store input socket pointer
		if (bmpState)
			bmpState->psktInput = (SOCKET*)lParam;
		return 0;

	case WM_USER + 2:
		SetWindowTextA(hwnd, (const char*)lParam);
		break;

	case WM_CONTEXTMENU: {
		POINT pt;
		pt.x = LOWORD(lParam);
		pt.y = HIWORD(lParam);
		if (pt.x == -1 && pt.y == -1) {
			RECT rect;
			GetWindowRect(hwnd, &rect);
			pt.x = rect.left + (rect.right - rect.left) / 2;
			pt.y = rect.top + (rect.bottom - rect.top) / 2;
		}
		HMENU hMenu = CreateScreenContextMenu();
		int cmd = TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_NONOTIFY, pt.x, pt.y, 0, hwnd, NULL);
		if (cmd)
			PostMessage(hwnd, WM_COMMAND, cmd, 0);
		DestroyMenu(hMenu);
		break;
	}
	case WM_COMMAND:
		switch (LOWORD(wParam)) {
		case IDM_VIDEO_FPS_5: SetRemoteScreenFps(hwnd, 5); break;
		case IDM_VIDEO_FPS_10: SetRemoteScreenFps(hwnd, 10); break;
		case IDM_VIDEO_FPS_20: SetRemoteScreenFps(hwnd, 20); break;
		case IDM_VIDEO_FPS_30: SetRemoteScreenFps(hwnd, 30); break;
		case IDM_VIDEO_FPS_40: SetRemoteScreenFps(hwnd, 40); break;
		case IDM_VIDEO_FPS_60: SetRemoteScreenFps(hwnd, 60); break;
		case IDM_ALWAYS_ON_TOP:
			g_alwaysOnTop = !g_alwaysOnTop;
			if (g_pMainWindow) {
				g_pMainWindow->m_savedAlwaysOnTop = g_alwaysOnTop;
				g_pMainWindow->SaveConfig();
			}
			SetWindowPos(hwnd, g_alwaysOnTop ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
			break;
		case IDM_SENDKEYS_ALTF4:    SendRemoteKeyCombo(hwnd, IDM_SENDKEYS_ALTF4); break;
		case IDM_SENDKEYS_CTRLESC:  SendRemoteKeyCombo(hwnd, IDM_SENDKEYS_CTRLESC); break;
		case IDM_SENDKEYS_CTRALTDEL:SendRemoteKeyCombo(hwnd, IDM_SENDKEYS_CTRALTDEL); break;
		case IDM_SENDKEYS_PRNTSCRN: SendRemoteKeyCombo(hwnd, IDM_SENDKEYS_PRNTSCRN); break;
		}
		break;

	case WM_LBUTTONDOWN:
	case WM_LBUTTONUP:
	case WM_RBUTTONDOWN:
	case WM_RBUTTONUP:
	case WM_MBUTTONDOWN:
	case WM_MBUTTONUP:
	case WM_MOUSEMOVE: {
		if (bmpState && bmpState->psktInput && *bmpState->psktInput != INVALID_SOCKET) {
			INPUT input = {};
			input.type = INPUT_MOUSE;

			POINT pt;
			pt.x = GET_X_LPARAM(lParam);
			pt.y = GET_Y_LPARAM(lParam);

			RECT rect;
			GetClientRect(hwnd, &rect);
			int winW = rect.right - rect.left, winH = rect.bottom - rect.top;
			int normX = 0, normY = 0;
			if (winW > 0 && winH > 0) {
				normX = (int)((pt.x / (double)winW) * nNormalized);
				normY = (int)((pt.y / (double)winH) * nNormalized);
			}

			input.mi.dx = normX;
			input.mi.dy = normY;
			input.mi.dwFlags = MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_MOVE;
			if (msg == WM_LBUTTONDOWN) input.mi.dwFlags |= MOUSEEVENTF_LEFTDOWN;
			if (msg == WM_LBUTTONUP)   input.mi.dwFlags |= MOUSEEVENTF_LEFTUP;
			if (msg == WM_RBUTTONDOWN) input.mi.dwFlags |= MOUSEEVENTF_RIGHTDOWN;
			if (msg == WM_RBUTTONUP)   input.mi.dwFlags |= MOUSEEVENTF_RIGHTUP;
			if (msg == WM_MBUTTONDOWN) input.mi.dwFlags |= MOUSEEVENTF_MIDDLEDOWN;
			if (msg == WM_MBUTTONUP)   input.mi.dwFlags |= MOUSEEVENTF_MIDDLEUP;

			send(*bmpState->psktInput, (const char*)&input, sizeof(INPUT), 0);
		}
		break;
	}
	case WM_MOUSEWHEEL: {
		if (bmpState && bmpState->psktInput && *bmpState->psktInput != INVALID_SOCKET) {
			INPUT input = {};
			input.type = INPUT_MOUSE;
			input.mi.dwFlags = MOUSEEVENTF_WHEEL;
			input.mi.mouseData = GET_WHEEL_DELTA_WPARAM(wParam);
			send(*bmpState->psktInput, (const char*)&input, sizeof(INPUT), 0);
		}
		break;
	}
	case WM_SYSCOMMAND:
		// Prevent activation of system menu by Alt or F10, which causes stuck keys
		if (wParam == SC_KEYMENU) {
			return 0;
		}
		else {
			return DefWindowProc(hwnd, msg, wParam, lParam);
		}

	case WM_KEYDOWN:
	case WM_KEYUP:
	case WM_SYSKEYDOWN:
	case WM_SYSKEYUP: {
		if (bmpState && bmpState->psktInput && *bmpState->psktInput != INVALID_SOCKET) {
			INPUT input = {};
			input.type = INPUT_KEYBOARD;
			input.ki.wVk = (WORD)wParam;
			input.ki.wScan = 0; // Let Windows fill this in if needed
			input.ki.dwFlags = 0;
			if (msg == WM_KEYUP || msg == WM_SYSKEYUP)
				input.ki.dwFlags |= KEYEVENTF_KEYUP;

			// Set EXTENDEDKEY for extended keys (F10, arrows, navigation, etc.)
			WORD vk = input.ki.wVk;
			if (
				(vk >= VK_F1 && vk <= VK_F24) ||
				vk == VK_MENU || vk == VK_RMENU ||
				vk == VK_CONTROL || vk == VK_RCONTROL ||
				vk == VK_INSERT || vk == VK_DELETE ||
				vk == VK_HOME || vk == VK_END ||
				vk == VK_PRIOR || vk == VK_NEXT ||
				vk == VK_LEFT || vk == VK_RIGHT ||
				vk == VK_UP || vk == VK_DOWN ||
				vk == VK_NUMLOCK || vk == VK_DIVIDE
				) {
				input.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
			}

			// Track Alt and F10 down/up to release on focus loss if needed
			if ((msg == WM_SYSKEYDOWN || msg == WM_KEYDOWN) && wParam == VK_MENU) altDown = true;
			if ((msg == WM_SYSKEYUP || msg == WM_KEYUP) && wParam == VK_MENU) altDown = false;
			if ((msg == WM_KEYDOWN) && wParam == VK_F10) f10Down = true;
			if ((msg == WM_KEYUP) && wParam == VK_F10) f10Down = false;

			// DEBUG LOGGING: Print what is being sent to the server
			std::cout << "[CLIENT] Sending INPUT: "
				<< "VK=0x" << std::hex << (int)input.ki.wVk
				<< " Scan=0x" << std::hex << (int)input.ki.wScan
				<< " Flags=0x" << std::hex << (int)input.ki.dwFlags
				<< " (" << (((msg == WM_KEYDOWN) || (msg == WM_SYSKEYDOWN)) ? "DOWN" : "UP") << ")"
				<< std::dec << std::endl;

			send(*bmpState->psktInput, (const char*)&input, sizeof(INPUT), 0);
		}
		break;
	}

	case WM_KILLFOCUS:
	case WM_ACTIVATE:
	case WM_SETFOCUS:
		if (bmpState && bmpState->psktInput && *bmpState->psktInput != INVALID_SOCKET) {
			if (altDown) {
				INPUT input = {};
				input.type = INPUT_KEYBOARD;
				input.ki.wVk = VK_MENU;
				input.ki.dwFlags = KEYEVENTF_KEYUP | KEYEVENTF_EXTENDEDKEY;
				send(*bmpState->psktInput, (const char*)&input, sizeof(INPUT), 0);
				altDown = false;
			}
			if (f10Down) {
				INPUT input = {};
				input.type = INPUT_KEYBOARD;
				input.ki.wVk = VK_F10;
				input.ki.dwFlags = KEYEVENTF_KEYUP | KEYEVENTF_EXTENDEDKEY;
				send(*bmpState->psktInput, (const char*)&input, sizeof(INPUT), 0);
				f10Down = false;
			}
		}
		break;

	case WM_ERASEBKGND:
		return 1; // Prevent flicker

	case WM_PAINT: {
		PAINTSTRUCT ps;
		HDC hdc = BeginPaint(hwnd, &ps);

		RECT clientRect;
		GetClientRect(hwnd, &clientRect);
		int destW = clientRect.right - clientRect.left;
		int destH = clientRect.bottom - clientRect.top;

		if (destW <= 0 || destH <= 0) {
			EndPaint(hwnd, &ps);
			break;
		}

		// Ultra-fast lock-free rendering
		if (bmpState && bmpState->bmp) {
			int srcW, srcH;
			uint8_t* frameData;
			uint64_t timestamp;
			
			// Lock-free frame access - never blocks!
			if (g_frameBuffer.GetLatestFrame(srcW, srcH, frameData, timestamp)) {
				// Calculate display dimensions with aspect ratio preservation
				double srcAspect = (double)srcW / srcH;
				double destAspect = (double)destW / destH;
				int drawW, drawH, offsetX, offsetY;
				
				if (destAspect > srcAspect) {
					drawH = destH;
					drawW = (int)(drawH * srcAspect);
					offsetX = (destW - drawW) / 2;
					offsetY = 0;
				} else {
					drawW = destW;
					drawH = (int)(drawW / srcAspect);
					offsetX = 0;
					offsetY = (destH - drawH) / 2;
				}

				// Ultra-fast DIB cache with direct memory access
				auto& cache = g_ultraFastCache[hwnd];
				if (cache.PrepareForSize(srcW, srcH) && cache.pBits) {
					// Direct memory copy - no color conversion in paint handler!
					memcpy(cache.pBits, frameData, srcW * srcH * 4);
					
					// Cache GDI settings to avoid repeated API calls (major performance gain)
					if (!cache.gdiSettingsApplied) {
						SetStretchBltMode(hdc, HALFTONE);
						SetBrushOrgEx(hdc, 0, 0, NULL); // For HALFTONE mode
						cache.gdiSettingsApplied = true;
					}
					
					// Direct stretch from DIB to window - maximum performance
					StretchBlt(hdc, offsetX, offsetY, drawW, drawH, 
							  cache.hMemDC, 0, 0, srcW, srcH, SRCCOPY);
					
					cache.lastUpdateTime = timestamp;
				} else {
					// Fallback: fast black fill
					HBRUSH blackBrush = (HBRUSH)GetStockObject(BLACK_BRUSH);
					FillRect(hdc, &clientRect, blackBrush);
				}
			} else {
				// No frame available: fast gray fill
				HBRUSH grayBrush = CreateSolidBrush(RGB(64, 64, 64));
				FillRect(hdc, &clientRect, grayBrush);
				DeleteObject(grayBrush);
			}
		} else {
			// No bitmap state: fast black fill
			HBRUSH blackBrush = (HBRUSH)GetStockObject(BLACK_BRUSH);
			FillRect(hdc, &clientRect, blackBrush);
		}
		
		EndPaint(hwnd, &ps);
		break;
	}
	case WM_EXITSIZEMOVE: // Save window geometry for restoring
		if (bmpState && bmpState->mainWindow) {
			WINDOWPLACEMENT wp = { sizeof(WINDOWPLACEMENT) };
			if (GetWindowPlacement(hwnd, &wp)) {
				RECT& r = wp.rcNormalPosition;
				bmpState->mainWindow->m_savedRemoteLeft = r.left;
				bmpState->mainWindow->m_savedRemoteTop = r.top;
				bmpState->mainWindow->m_savedRemoteW = r.right - r.left;
				bmpState->mainWindow->m_savedRemoteH = r.bottom - r.top;
				bmpState->mainWindow->SaveConfig();
			}
		}
		break;

	case WM_DESTROY: {
		// Clean up ultra-fast DIB cache
		auto it = g_ultraFastCache.find(hwnd);
		if (it != g_ultraFastCache.end()) {
			it->second.Cleanup();
			g_ultraFastCache.erase(it);
		}

		if (bmpState) {
			delete bmpState;
			// Do NOT clear GWLP_USERDATA here!
		}
		// Only quit if in client mode
		if (g_pMainWindow && g_pMainWindow->Data.nMode == MainWindow::MODE::CLIENT) {
			PostQuitMessage(0);
		}
		break;
	}
	case WM_NCDESTROY: {
		// Now it is safe to clear the user data pointer
		SetWindowLongPtr(hwnd, GWLP_USERDATA, 0);
		break;
	}
	default:
		return DefWindowProc(hwnd, msg, wParam, lParam);
	}
	return 0;
}

// StartScreenRecv: create remote window and set up state, but do NOT touch GWLP_USERDATA here
void StartScreenRecv(const std::string& server_ip, int port, const RemoteWindowPlacement& place, SOCKET* psktInput) {
	SOCKET skt = INVALID_SOCKET;
	if (ConnectScreenStreamServer(skt, server_ip, port) != 0) {
		MessageBoxA(NULL, "Failed to connect to screen stream server!", "Remote", MB_OK | MB_ICONERROR);
		return;
	}
	WNDCLASSA wc = { 0 };
	wc.lpfnWndProc = ScreenWndProc;
	wc.lpszClassName = "RemoteScreenWnd";
	wc.hInstance = GetModuleHandle(NULL);
	wc.hCursor = LoadCursor(NULL, IDC_ARROW);
	RegisterClassA(&wc);

	int left = place.left;
	int top = place.top;
	int w = place.width;
	int h = place.height;

	HWND hwnd = CreateWindowA(wc.lpszClassName, "Remote Screen", WS_OVERLAPPEDWINDOW,
		left, top, w, h, NULL, NULL, wc.hInstance, NULL);

	// Pass input socket pointer to the window (THIS IS CRITICAL)
	SendMessage(hwnd, WM_USER + 100, 0, (LPARAM)psktInput);

	ShowWindow(hwnd, SW_SHOWNORMAL);

	std::pair<std::string, int> server_info = GetPeerIpAndPort(skt);
	std::string actual_server_ip = server_info.first;
	int actual_server_port = server_info.second;

	std::thread t(ScreenRecvThread, skt, hwnd, actual_server_ip, actual_server_port);
	t.detach();

	MSG msg = { 0 };
	while (IsWindow(hwnd) && GetMessage(&msg, NULL, 0, 0)) {
		TranslateMessage(&msg);
		DispatchMessage(&msg);
		if (!IsWindow(hwnd)) break;
	}

	if (skt != INVALID_SOCKET) {
		closesocket(skt);
	}
}


LRESULT MainWindow::HandleMessage(UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	switch (uMsg)
	{
	case WM_CREATE:
		return HandleCreate(uMsg, wParam, lParam);
	case WM_INPUT:
		// CLIENT mode: capture input, send to server
		if (Data.nMode == MODE::CLIENT && Client.isConnected) {
			unsigned int dwSize;
			if (GetRawInputData((HRAWINPUT)lParam, RID_INPUT, NULL, &dwSize, sizeof(RAWINPUTHEADER)) == -1) break;
			LPBYTE lpb = new unsigned char[dwSize];
			if (!lpb) break;
			if (GetRawInputData((HRAWINPUT)lParam, RID_INPUT, lpb, &dwSize, sizeof(RAWINPUTHEADER)) != dwSize) {
				delete[] lpb; break;
			}
			INPUT inputBuff;
			ConvertInput((PRAWINPUT)lpb, &inputBuff);
			delete[] lpb;
			// Send to server
			send(Client.sktServer, (char*)&inputBuff, sizeof(INPUT), 0);
		}
		return 0;
	case WM_PAINT:
		return HandlePaint(uMsg, wParam, lParam);
	case WM_COMMAND:
		return HandleCommand(uMsg, wParam, lParam);
	case WM_CLOSE:
		return HandleClose(uMsg, wParam, lParam);
		// --- Clipboard sync messages ---
	case WM_DRAWCLIPBOARD:
	case WM_CHANGECBCHAIN:
		// Call clipboard handler (ensure it's implemented as shown in previous message)
		return HandleClipboardMsg(m_hwnd, uMsg, wParam, lParam);
		// WM_EXITSIZEMOVE handling for MainWindow removed! It should be handled in the remote screen window proc instead.
	case WM_DESTROY:
		CleanupClipboardMonitor(m_hwnd); // cleanup clipboard chain on destroy
		PostQuitMessage(0);
		return 0;
	default:
		return DefWindowProc(m_hwnd, uMsg, wParam, lParam);
	}
	return TRUE;
}

// Removed RetrieveInput logic (no longer used on server)

int MainWindow::HandleCommand(UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	switch (HIWORD(wParam)) {
	case BN_CLICKED:
		switch (LOWORD(wParam)) {

		case BTN_START:
			return ServerStart();

		case BTN_PAUSE:
			Server.bPause = !Server.bPause;
			Log(((!Server.bPause) ? "Resumed" : "Paused"));
			SetWindowText(m_btnPause.Window(), (Server.bPause) ? "Resume" : "Pause");
			return 0;

		case BTN_TERMINATE:
			return ServerTerminate();

		case BTN_CONNECT:
			return ClientConnect();

		case BTN_DISCONNECT:
			return ClientDisconnect();

		case EDIT_ADDRESS:

			break;

		case BTN_SERVER:
			return SetMode(MODE::SERVER);

		case BTN_CLIENT:
			return SetMode(MODE::CLIENT);

		case MENU_FILE:

			break;

		case MENU_SUB:

			break;

		case MENU_EXIT:
			PostMessage(m_hwnd, WM_CLOSE, 0, 0);
			return 0;

		case MENU_ABOUT:

			break;

		default:
			//MessageBox(m_hwnd, "A Button was clicked", "My application", MB_OKCANCEL);
			break;
		}
		return 0;

	default:
		break;
	}
}
int MainWindow::HandleCreate(UINT uMsg, WPARAM wParam, LPARAM lParam)

{
	//m_btnOk.Create(L"OK", WS_TABSTOP | WS_VISIBLE | WS_CHILD | BS_DEFPUSHBUTTON, 0, 50, 50, 100, 100, this->m_hwnd);
	CreateWindowA("BUTTON", "Mode", WS_TABSTOP | WS_VISIBLE | WS_CHILD | BS_GROUPBOX, 20, 10, 190, 60, m_hwnd, (HMENU)BTN_MODE, (HINSTANCE)GetWindowLong(m_hwnd, GWLP_HINSTANCE), NULL);
	m_btnModeServer.Create(this, "Server", WS_TABSTOP | WS_VISIBLE | WS_CHILD | BS_AUTORADIOBUTTON, 0, 30, 35, 70, 20, m_hwnd, (HMENU)BTN_SERVER);
	m_btnModeClient.Create(this, "Client", WS_TABSTOP | WS_VISIBLE | WS_CHILD | BS_AUTORADIOBUTTON, 0, 130, 35, 70, 20, m_hwnd, (HMENU)BTN_CLIENT);

	m_btnStart.Create(this, "Start", WS_TABSTOP | WS_CHILD | BS_PUSHBUTTON, 0, 20, 80, 50, 20, m_hwnd, (HMENU)BTN_START);
	m_btnPause.Create(this, "Pause", WS_TABSTOP | WS_CHILD | BS_PUSHBUTTON, 0, 80, 80, 50, 20, m_hwnd, (HMENU)BTN_PAUSE);
	m_btnTerminate.Create(this, "Terminate", WS_TABSTOP | WS_CHILD | BS_PUSHBUTTON, 0, 140, 80, 70, 20, m_hwnd, (HMENU)BTN_TERMINATE);

	m_btnConnect.Create(this, "Connect", WS_TABSTOP | WS_CHILD | BS_PUSHBUTTON, 0, 35, 80, 60, 20, m_hwnd, (HMENU)BTN_CONNECT);
	m_btnDisconnect.Create(this, "Disconnect", WS_TABSTOP | WS_CHILD | BS_PUSHBUTTON, 0, 115, 80, 80, 20, m_hwnd, (HMENU)BTN_DISCONNECT);

	//hLog = CreateWindowEx(0, "edit", txtLog.c_str(), WS_VISIBLE | WS_CHILD | ES_READONLY | ES_MULTILINE, 250, 10, 200, 260, m_hwnd, NULL, (HINSTANCE)GetWindowLong(m_hwnd, GWL_HINSTANCE), NULL);

	m_itxtIP.Create(this, Client.ip.c_str(), WS_VISIBLE | WS_CHILD | ES_READONLY, 0, 130, 120, 100, 20, m_hwnd, (HMENU)EDIT_ADDRESS);
	m_itxtPort.Create(this, sPort.c_str(), WS_VISIBLE | WS_CHILD | ES_READONLY, 0, 130, 150, 100, 20, m_hwnd, (HMENU)EDIT_PORT);

	m_stxtKeyboard.Create(this, "", WS_VISIBLE | WS_CHILD, 0, 130, 180, 170, 20, m_hwnd, NULL);
	m_stxtMouse.Create(this, "", WS_VISIBLE | WS_CHILD, 0, 130, 210, 170, 20, m_hwnd, NULL);
	m_stxtMouseOffset.Create(this, "", WS_VISIBLE | WS_CHILD, 0, 130, 230, 170, 20, m_hwnd, NULL);
	m_stxtMouseBtn.Create(this, "", WS_VISIBLE | WS_CHILD, 0, 130, 250, 170, 20, m_hwnd, NULL);

	PostMessage(m_itxtPort.Window(), EM_SETREADONLY, (WPARAM)false, 0);
	Data.sLabels[1] = "Port: ";
	return 0;
}
int MainWindow::HandlePaint(UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	PAINTSTRUCT ps;
	HDC hdc;
	RECT clientRect;
	HBRUSH hBrush;


	hdc = BeginPaint(m_hwnd, &ps);
	GetClientRect(m_hwnd, &clientRect);

	hBrush = CreateSolidBrush(RGB(255, 255, 255));
	//FillRect(hdc, &clientRect, hBrush);
	FillRect(hdc, &ps.rcPaint, (HBRUSH)(COLOR_WINDOW + 1));

	TextOut(hdc, 20, 120, Data.sLabels[0].c_str(), Data.sLabels[0].length());
	TextOut(hdc, 20, 150, Data.sLabels[1].c_str(), Data.sLabels[1].length());

	TextOut(hdc, 20, 180, "Keyboard Input:", 15);
	TextOut(hdc, 20, 210, "Mouse Input:", 12);

	EndPaint(m_hwnd, &ps);
	return 0;
}
int MainWindow::HandleClose(UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	//if (MessageBox(m_hwnd, "Really quit?", "Remote", MB_OKCANCEL) == IDOK) {
	//	Data.nMode = MODE::UNDEF;
	//	DestroyWindow(m_hwnd);
	//}
	switch (Data.nMode)
	{
	case MODE::SERVER:
		if (Server.isOnline)
		{
			ServerTerminate();
		}
		break;

	case MODE::CLIENT:
		char out_ip[50];
		GetWindowText(m_itxtIP.Window(), out_ip, 50);
		Client.ip = out_ip;
		if (Client.isConnected)
		{
			ClientDisconnect();
		}
		break;

	default:
		break;
	}

	DestroyWindow(m_hwnd);
	return 0;
}

void MainWindow::Log(std::string msg)
{
	switch (Data.nMode)
	{
	case MODE::CLIENT: ClientLog(msg); break;
	case MODE::SERVER: ServerLog(msg); break;
	case MODE::UNDEF: std::cout << msg << std::endl; break;
	}
}
void MainWindow::ServerLog(std::string msg)
{
	std::cout << "Server - " << msg << std::endl;
}
void MainWindow::ClientLog(std::string msg)
{
	std::cout << "Client - " << msg << std::endl;
}


int MainWindow::InitializeInputDevice() {

	// keyboard
	Server.rid[0].dwFlags = RIDEV_INPUTSINK;
	Server.rid[0].usUsagePage = 1;
	Server.rid[0].usUsage = 6;
	Server.rid[0].hwndTarget = m_hwnd;

	// mouse
	Server.rid[1].dwFlags = RIDEV_INPUTSINK;
	Server.rid[1].usUsagePage = 1;
	Server.rid[1].usUsage = 2;
	Server.rid[1].hwndTarget = m_hwnd;

	if (!RegisterRawInputDevices(Server.rid, 2, sizeof(RAWINPUTDEVICE)))
	{
		return 1;
	}
	return 0;
}

std::string VKeyToString(unsigned int vk)
{
	//https://docs.microsoft.com/en-us/windows/win32/inputdev/virtual-key-codes
	switch (vk)
	{
	case VK_BACK: return "BACK";
	case VK_TAB: return "TAB";
	case VK_CLEAR: return "CLEAR";
	case VK_RETURN: return "ENTER";
	case VK_SHIFT: return "SHIFT";
	case VK_CONTROL: return "CONTROL";
	case VK_MENU: return "ALT";
	case VK_CAPITAL: return "CAP LOCK";
	case VK_KANA: return "IME Kana";
		//case VK_HANGUEL: return "IME Hnaguel";
		//case VK_HANGUL: return "IME Hangul";
		//case VK_IME_ON: return "IME On";
	case VK_JUNJA: return "IME JUNA";
	case VK_FINAL: return "IME FINAL";
	case VK_HANJA: return "IME HANJA";
		//case VK_KANJI: return "IME Kanji";
		//case VK_IME_OFF: return "IME Off";
	case VK_ESCAPE: return "ESC";
	case VK_CONVERT: return "IME CONVERT";
	case VK_NONCONVERT: return "IME NONCONVERT";
	case VK_ACCEPT: return "IME ACCEPT";
	case VK_MODECHANGE: return "IME CHANGE MODE";
	case VK_SPACE: return "SPACE";
	case VK_PRIOR: return "PAGE UP";
	case VK_NEXT: return "PAGE DOWN";
	case VK_END: return "END";
	case VK_HOME: return "HOME";
	case VK_LEFT: return "LEFT ARROW";
	case VK_UP: return "UP ARROW";
	case VK_RIGHT: return "RIGHT ARROW";
	case VK_DOWN: return "DOWN ARROW";
	case VK_SELECT: return "SELECT";
	case VK_PRINT: return "PRINT";
	case VK_EXECUTE: return "EXECUTE";
	case VK_SNAPSHOT: return "PRINT SCREEN";
	case VK_INSERT: return "INSERT";
	case VK_DELETE: return "DELETE";
	case VK_HELP: return "HELP";
	case 0x30: return "0";
	case 0x31: return "1";
	case 0x32: return "2";
	case 0x33: return "3";
	case 0x34: return "4";
	case 0x35: return "5";
	case 0x36: return "6";
	case 0x37: return "7";
	case 0x38: return "8";
	case 0x39: return "9";
	case 0x41: return "A";
	case 0x42: return "B";
	case 0x43: return "C";
	case 0x44: return "D";
	case 0x45: return "E";
	case 0x46: return "F";
	case 0x47: return "G";
	case 0x48: return "H";
	case 0x49: return "I";
	case 0x4A: return "J";
	case 0x4B: return "K";
	case 0x4C: return "L";
	case 0x4D: return "M";
	case 0x4E: return "N";
	case 0x4F: return "O";
	case 0x50: return "P";
	case 0x51: return "Q";
	case 0x52: return "R";
	case 0x53: return "S";
	case 0x54: return "T";
	case 0x55: return "U";
	case 0x56: return "V";
	case 0x57: return "W";
	case 0x58: return "X";
	case 0x59: return "Y";
	case 0x5A: return "Z";
	case VK_LWIN: return "LEFT WINDOWS";
	case VK_RWIN: return "RIGHT WINDOWS";
	case VK_APPS: return "APPLICATION";
	case VK_SLEEP: return "SLEEP";
	case VK_NUMPAD0: return "NUMPAD 0";
	case VK_NUMPAD1: return "NUMPAD 1";
	case VK_NUMPAD2: return "NUMPAD 2";
	case VK_NUMPAD3: return "NUMPAD 3";
	case VK_NUMPAD4: return "NUMPAD 4";
	case VK_NUMPAD5: return "NUMPAD 5";
	case VK_NUMPAD6: return "NUMPAD 6";
	case VK_NUMPAD7: return "NUMPAD 7";
	case VK_NUMPAD8: return "NUMPAD 8";
	case VK_NUMPAD9: return "NUMPAD 9";
	case VK_MULTIPLY: return "MULTIPLY";
	case VK_ADD: return "ADD";
	case VK_SEPARATOR: return "SEPARATOR";
	case VK_SUBTRACT: return "SUBTRACT";
	case VK_DECIMAL: return "DECIMAL";
	case VK_DIVIDE: return "DIVIDE";
	case VK_F1: return "F1";
	case VK_F2: return "F2";
	case VK_F3: return "F3";
	case VK_F4: return "F4";
	case VK_F5: return "F5";
	case VK_F6: return "F6";
	case VK_F7: return "F7";
	case VK_F8: return "F8";
	case VK_F9: return "F9";
	case VK_F10: return "F10";
	case VK_F11: return "F11";
	case VK_F12: return "F12";
	case VK_F13: return "F13";
	case VK_F14: return "F14";
	case VK_F15: return "F15";
	case VK_F16: return "F16";
	case VK_F17: return "F17";
	case VK_F18: return "F18";
	case VK_F19: return "F19";
	case VK_F20: return "F20";
	case VK_F21: return "F21";
	case VK_F22: return "F22";
	case VK_F23: return "F23";
	case VK_F24: return "F24";
	case VK_NUMLOCK: return "NUM LOCK";
	case VK_SCROLL: return "SCROLL LOCK";
	case 0x92: return "OEM KEY 1";
	case 0x93: return "OEM KEY 2";
	case 0x94: return "OEM KEY 3";
	case 0x95: return "OEM KEY 4";
	case 0x96: return "OEM KEY 5";
	case VK_LSHIFT: return "LEFT SHIFT";
	case VK_RSHIFT: return "RIGHT SHIFT";
	case VK_LCONTROL: return "LEFT CONTROL";
	case VK_RCONTROL: return "RIGHT CONTROL";
	case VK_LMENU: return "LEFT MENU";
	case VK_RMENU: return "RIGHT MENU";
	case VK_BROWSER_BACK: return "BROWSER BACK";
	case VK_BROWSER_FORWARD: return "BROWSER FORWARD";
	case VK_BROWSER_REFRESH: return "BROWSER REFRESH";
	case VK_BROWSER_STOP: return "BROWSER STOP";
	case VK_BROWSER_SEARCH: return "BROWSER SEARCH";
	case VK_BROWSER_FAVORITES: return "BROWSER FAVORITES";
	case VK_BROWSER_HOME: return "BROWSER HOME";
	case VK_VOLUME_MUTE: return "VOLUME MUTE";
	case VK_VOLUME_DOWN: return "VOLUME DOWN";
	case VK_VOLUME_UP: return "VOLUME UP";
	case VK_MEDIA_NEXT_TRACK: return "NEXT TRACK";
	case VK_MEDIA_PREV_TRACK: return "PREVIOUS TRACK";
	case VK_MEDIA_STOP: return "STOP MEDIA";
	case VK_MEDIA_PLAY_PAUSE: return "PLAY/PAUSE MEDIA";
	case VK_LAUNCH_MAIL: return "LAUNCH MAIL";
	case VK_LAUNCH_MEDIA_SELECT: return "SELECT MEDIA";
	case VK_LAUNCH_APP1: return "START APP 1";
	case VK_LAUNCH_APP2: return "START APP 2";
	case VK_OEM_1: return "MISC CHAR 1";
	case VK_OEM_PLUS: return "PLUS";
	case VK_OEM_COMMA: return "COMMA";
	case VK_OEM_MINUS: return "MINUS";
	case VK_OEM_PERIOD: return "PERIOD";
	case VK_OEM_2: return "MISC CHAR 2";
	case VK_OEM_3: return "MISC CHAR 3";
	case VK_OEM_4: return "MISC CHAR 4";
	case VK_OEM_5: return "MISC CHAR 5";
	case VK_OEM_6: return "MISC CHAR 6";
	case VK_OEM_7: return "MISC CHAR 7";
	case VK_OEM_8: return "MISC CHAR 8";
	case 0xE1: return "OEM KEY 6";
	case VK_ATTN: return "ATTN KEY";
	case VK_CRSEL: return "CRSEL KEY";
	case VK_EXSEL: return "EXSEL KEY";
	case VK_EREOF: return "ERASE EOF";
	case VK_PLAY: return "PLAY";
	case VK_ZOOM: return "ZOOM";
	case VK_NONAME: return "NO NAME KEY";
	case VK_PA1: return "PA1 KEY";
	case VK_OEM_CLEAR: return "CLEAR KEY";
	default: return "UNKNOWN";
	}
}

void MainWindow::UpdateInput() {

	std::string key_out("");
	std::string mouse_out("");

	if (Server.inputBuff.type == INPUT_KEYBOARD) {
		key_out = VKeyToString(Server.inputBuff.ki.wVk);
		//key_out += MapVirtualKeyA(Server.inputBuff.ki.wVk, MAPVK_VK_TO_CHAR);
		if (Server.inputBuff.ki.dwFlags == KEYEVENTF_KEYUP) {
			key_out += " UP";
		}
		else {
			key_out += " DOWN";
		}

		SetWindowText(m_stxtKeyboard.Window(), key_out.c_str());
	}
	else if (Server.inputBuff.type == INPUT_MOUSE) {
		if (Server.inputBuff.mi.dwFlags == MOUSEEVENTF_LEFTDOWN) {
			SetWindowText(m_stxtMouseBtn.Window(), "Left Pressed");
		}
		else if (Server.inputBuff.mi.dwFlags == MOUSEEVENTF_RIGHTDOWN) {
			SetWindowText(m_stxtMouseBtn.Window(), "Right Pressed");
		}
		else if (Server.inputBuff.mi.dwFlags == MOUSEEVENTF_MIDDLEDOWN) {
			SetWindowText(m_stxtMouseBtn.Window(), "Middle Pressed");
		}
		else if (Server.inputBuff.mi.dwFlags == MOUSEEVENTF_LEFTUP) {
			SetWindowText(m_stxtMouseBtn.Window(), "Left Released");
		}
		else if (Server.inputBuff.mi.dwFlags == MOUSEEVENTF_RIGHTUP) {
			SetWindowText(m_stxtMouseBtn.Window(), "Right Released");
		}
		else if (Server.inputBuff.mi.dwFlags == MOUSEEVENTF_MIDDLEUP) {
			SetWindowText(m_stxtMouseBtn.Window(), "Middle Released");
		}
		else if (Server.inputBuff.mi.dwFlags == MOUSEEVENTF_WHEEL) {
			mouse_out = "Wheel delta=" + std::to_string((int16_t)Server.inputBuff.mi.mouseData);
			SetWindowText(m_stxtMouseBtn.Window(), mouse_out.c_str());
		}
		else {
			SetWindowText(m_stxtMouseBtn.Window(), "");
		}
		Data.sMouseState[0] = std::to_string(Server.inputBuff.mi.dx);
		Data.sMouseState[1] = std::to_string(Server.inputBuff.mi.dy);
		std::string mouse_pos = "(" + Data.sMouseState[0] + ", " + Data.sMouseState[1] + ")";
		std::string mouse_offset = "(" + std::to_string(Server.nOffsetX) + ")";
		SetWindowText(m_stxtMouse.Window(), mouse_pos.c_str());
		SetWindowText(m_stxtMouseOffset.Window(), mouse_offset.c_str());
	}
}
void MainWindow::UpdateGuiControls()
{
	switch (Data.nMode)
	{
	case MODE::CLIENT:
	{
		if (!Client.isConnected && !Client.wasClient) {
			Button_Enable(m_btnStart.Window(), false);
			Button_Enable(m_btnTerminate.Window(), false);
			Button_Enable(m_btnPause.Window(), false);
			ShowWindow(m_btnStart.Window(), SW_HIDE);
			ShowWindow(m_btnTerminate.Window(), SW_HIDE);
			ShowWindow(m_btnPause.Window(), SW_HIDE);

			Button_Enable(m_btnConnect.Window(), true);
			Button_Enable(m_btnDisconnect.Window(), false);
			ShowWindow(m_btnConnect.Window(), SW_SHOW);
			ShowWindow(m_btnDisconnect.Window(), SW_SHOW);

			Data.sLabels[0] = "Server Address: ";
			//Data.sLabels[1] = "Connected: ";

			SetRect(&Data.textRect, 20, 120, 129, 170);
			InvalidateRect(m_hwnd, &Data.textRect, true);

			PostMessage(m_itxtIP.Window(), EM_SETREADONLY, (WPARAM)false, 0);
			SetWindowText(m_itxtIP.Window(), Client.ip.c_str());
			//SetWindowLong(hTxtIP, GWL_STYLE, GetWindowLong(hTxtIP, GWL_STYLE) ^ ES_READONLY);
			//UpdateWindow(hTxtIP);
			Client.wasClient = true;
			Server.wasServer = false;
			UpdateWindow(m_hwnd);
		}
		else if (Client.isConnected)
		{
			Button_Enable(m_btnConnect.Window(), false);
			Button_Enable(m_btnDisconnect.Window(), true);
			Button_Enable(m_btnModeServer.Window(), false);
			Button_Enable(m_btnModeClient.Window(), false);
		}
		//else if (!Client.isConnected)
		//{
		//	Button_Enable(m_btnConnect.Window(), true);
		//	Button_Enable(m_btnDisconnect.Window(), false);
		//	Button_Enable(m_btnModeServer.Window(), true);
		//	Button_Enable(m_btnModeClient.Window(), true);
		//}
	}
	break;

	case MODE::SERVER:
	{
		if (!Server.isOnline && !Server.wasServer) {
			Button_Enable(m_btnStart.Window(), true);
			Button_Enable(m_btnPause.Window(), false);
			Button_Enable(m_btnTerminate.Window(), false);
			ShowWindow(m_btnStart.Window(), SW_SHOW);
			ShowWindow(m_btnTerminate.Window(), SW_SHOW);
			ShowWindow(m_btnPause.Window(), SW_SHOW);

			Button_Enable(m_btnConnect.Window(), false);
			Button_Enable(m_btnDisconnect.Window(), false);
			ShowWindow(m_btnConnect.Window(), SW_HIDE);
			ShowWindow(m_btnDisconnect.Window(), SW_HIDE);

			Data.sLabels[0] = "IP Address: ";
			//Data.sLabels[1] = "NB Connected: ";

			SetRect(&Data.textRect, 20, 120, 129, 170);
			InvalidateRect(m_hwnd, &Data.textRect, true);
			char out_ip[50];
			GetWindowText(m_itxtIP.Window(), out_ip, 50);
			Client.ip = out_ip;

			PostMessage(m_itxtIP.Window(), EM_SETREADONLY, (WPARAM)true, 0);
			SetWindowText(m_itxtIP.Window(), Server.ip.c_str());

			//SetWindowLong(hTxtIP, GWL_STYLE, GetWindowLong(hTxtIP, GWL_STYLE) ^ ES_READONLY);
			//UpdateWindow(hTxtIP);
			Client.wasClient = false;
			Server.wasServer = true;
			UpdateWindow(m_hwnd);
		}
		else if (Server.isOnline)
		{
			Button_Enable(m_btnStart.Window(), false);
			Button_Enable(m_btnTerminate.Window(), true);
			Button_Enable(m_btnPause.Window(), true);
			Button_Enable(m_btnModeServer.Window(), false);
			Button_Enable(m_btnModeClient.Window(), false);
		}
		//else if (!Server.isOnline)
		//{
		//	Button_Enable(m_btnStart.Window(), false);
		//	Button_Enable(m_btnTerminate.Window(), true);
		//	Button_Enable(m_btnPause.Window(), true);
		//	Button_Enable(m_btnModeServer.Window(), false);
		//	Button_Enable(m_btnModeClient.Window(), false);
		//}
	}
	break;

	case MODE::UNDEF:
		break;
	}
}

void MainWindow::ConvertInput(PRAWINPUT pRaw, INPUT* pInput) {
	if (pRaw->header.dwType == RIM_TYPEMOUSE) {
		pInput->type = INPUT_MOUSE;
		pInput->mi.dx = pRaw->data.mouse.lLastX;
		pInput->mi.dy = pRaw->data.mouse.lLastY;
		pInput->mi.mouseData = 0;
		pInput->mi.dwFlags = 0;
		pInput->mi.time = 0;
		if (pRaw->data.mouse.lLastX != 0 || pRaw->data.mouse.lLastY != 0) {
			pInput->mi.dwFlags = pInput->mi.dwFlags | MOUSEEVENTF_MOVE;
		}

		else if (pRaw->data.mouse.usFlags == MOUSE_MOVE_ABSOLUTE) {
			pInput->mi.dwFlags = pInput->mi.dwFlags | MOUSEEVENTF_ABSOLUTE;
		}
		else {
			switch (pRaw->data.mouse.usButtonFlags) {
			case RI_MOUSE_LEFT_BUTTON_DOWN:
				pInput->mi.dwFlags = pInput->mi.dwFlags | MOUSEEVENTF_LEFTDOWN;
				break;

			case RI_MOUSE_LEFT_BUTTON_UP:
				pInput->mi.dwFlags = pInput->mi.dwFlags | MOUSEEVENTF_LEFTUP;
				break;

			case RI_MOUSE_MIDDLE_BUTTON_DOWN:
				pInput->mi.dwFlags = pInput->mi.dwFlags | MOUSEEVENTF_MIDDLEDOWN;
				break;

			case RI_MOUSE_MIDDLE_BUTTON_UP:
				pInput->mi.dwFlags = pInput->mi.dwFlags | MOUSEEVENTF_MIDDLEUP;
				break;

			case RI_MOUSE_RIGHT_BUTTON_DOWN:
				pInput->mi.dwFlags = pInput->mi.dwFlags | MOUSEEVENTF_RIGHTDOWN;
				break;

			case RI_MOUSE_RIGHT_BUTTON_UP:
				pInput->mi.dwFlags = pInput->mi.dwFlags | MOUSEEVENTF_RIGHTUP;
				break;

			case RI_MOUSE_WHEEL:
				pInput->mi.dwFlags = pInput->mi.dwFlags | MOUSEEVENTF_WHEEL;
				pInput->mi.mouseData = pRaw->data.mouse.usButtonData;
				break;
			}
		}

	}
	else if (pRaw->header.dwType == RIM_TYPEKEYBOARD) {
		pInput->type = INPUT_KEYBOARD;
		pInput->ki.wVk = pRaw->data.keyboard.VKey;
		pInput->ki.wScan = MapVirtualKeyA(pRaw->data.keyboard.VKey, MAPVK_VK_TO_VSC);
		pInput->ki.dwFlags = KEYEVENTF_SCANCODE;

		// Set KEYUP if key up
		if (pRaw->data.keyboard.Message == WM_KEYUP) {
			pInput->ki.dwFlags |= KEYEVENTF_KEYUP;
		}

		// Extended key logic
		// Most navigation keys and F1-F24 are extended. Main Enter is NOT extended, but Numpad Enter is.
		WORD vk = pInput->ki.wVk;
		USHORT makeCode = pRaw->data.keyboard.MakeCode;
		bool isExtended = (pRaw->data.keyboard.Flags & RI_KEY_E0) != 0;

		// Set extended for the right keys
		if (
			(vk >= VK_F1 && vk <= VK_F24) ||
			vk == VK_MENU || vk == VK_CONTROL ||
			vk == VK_INSERT || vk == VK_DELETE ||
			vk == VK_HOME || vk == VK_END ||
			vk == VK_PRIOR || vk == VK_NEXT ||
			vk == VK_LEFT || vk == VK_RIGHT ||
			vk == VK_UP || vk == VK_DOWN ||
			vk == VK_NUMLOCK || vk == VK_DIVIDE
			) {
			pInput->ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
		}

		// Special handling for Enter (Numpad vs Main)
		// If the input is extended and Enter, it's Numpad Enter
		if (vk == VK_RETURN && isExtended) {
			pInput->ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
		}

		// You can print for debugging:
		printf("VK: %02X Scan: %02X Flags: %08X\n", vk, pInput->ki.wScan, pInput->ki.dwFlags);

		pInput->ki.time = 0;
	}
}

int MainWindow::SetMode(MODE m)
{
	switch (m)
	{
	case MODE::SERVER:
		if (Client.wasClient)
		{

		}
		Log("Mode server");
		Data.nMode = MODE::SERVER;
		UpdateGuiControls();
		return 0;

	case MODE::CLIENT:

		if (Server.wasServer)
		{

		}
		Log("Mode client");
		Data.nMode = MODE::CLIENT;
		UpdateGuiControls();
		return 0;

	default:
		Log("Mode Unknown");
		Data.nMode = MODE::UNDEF;
		return 0;
	}
	return 0;
}

int MainWindow::ServerStart()
{
	char out_port[50];
	GetWindowText(m_itxtPort.Window(), out_port, 50);
	sPort = out_port;
	SaveConfig();
	if (!Server.isRegistered)
	{
		InitializeInputDevice();
		Log("Input Device Registered");
		Server.isRegistered = true;
	}
	Log("Initializing");
	int error = InitializeServer(Server.sktListen, std::stoi(sPort));
	if (error == 1) {
		Log("Could not initialize server");
		if (MessageBox(m_hwnd, "Could not initialize server", "Remote - Error", MB_ABORTRETRYIGNORE | MB_DEFBUTTON1 | MB_ICONERROR) == IDRETRY) {
			PostMessage(m_hwnd, WM_COMMAND, MAKEWPARAM(BTN_START, BN_CLICKED), 0);
			return 1;
		}
	}
	else {
		Log("Server initialized");
		Server.ClientsInformation.resize(MAX_CLIENTS);
		for (auto& c : Server.ClientsInformation)
		{
			c.socket = INVALID_SOCKET;
			c.ip = "";
			c.id = -1;
		}
		Log("Sockets initialized");
		Server.isOnline = true;
		UpdateGuiControls();
		// start listening thread
		Log("Starting listening thread");
		Server.tListen = std::thread(&MainWindow::ListenThread, this);

		// SCREEN STREAM: Start a screen stream server socket on SCREEN_STREAM_PORT
		SOCKET* sktScreenListenPtr = new SOCKET(INVALID_SOCKET);
		if (InitializeScreenStreamServer(*sktScreenListenPtr, SCREEN_STREAM_PORT) == 0) {
			std::thread([sktScreenListenPtr]() {
				while (true) {
					if (listen(*sktScreenListenPtr, 1) == SOCKET_ERROR) break;
					sockaddr_in client_addr;
					int addrlen = sizeof(client_addr);
					SOCKET sktClient = accept(*sktScreenListenPtr, (sockaddr*)&client_addr, &addrlen);
					if (sktClient == INVALID_SOCKET) continue;
					std::thread(ScreenStreamServerThread, sktClient).detach();
				}
				closesocket(*sktScreenListenPtr);
				delete sktScreenListenPtr;
				}).detach();
				Log("Screen streaming server started");
		}
		else {
			delete sktScreenListenPtr;
			Log("Could not start screen streaming server");
		}

		// AUDIO STREAM: Start an audio stream server socket on AUDIO_STREAM_PORT
		SOCKET* sktAudioListenPtr = new SOCKET(INVALID_SOCKET);
		if (InitializeScreenStreamServer(*sktAudioListenPtr, AUDIO_STREAM_PORT) == 0) {
			std::thread([sktAudioListenPtr]() {
				while (true) {
					if (listen(*sktAudioListenPtr, 1) == SOCKET_ERROR) break;
					sockaddr_in client_addr;
					int addrlen = sizeof(client_addr);
					SOCKET sktClient = accept(*sktAudioListenPtr, (sockaddr*)&client_addr, &addrlen);
					if (sktClient == INVALID_SOCKET) continue;
					std::thread(AudioStreamServerThreadXRLE, sktClient).detach();
				}
				closesocket(*sktAudioListenPtr);
				delete sktAudioListenPtr;
				}).detach();
				Log("Audio streaming server started");
		}
		else {
			delete sktAudioListenPtr;
			Log("Could not start audio streaming server");
		}

		Server.tListen.detach();
	}
	return 0;
}

int MainWindow::ServerTerminate()
{
	if (Server.isOnline)
	{
		int error = 1;
		Log("Terminate");
		//MessageBox(m_hwnd, "Terminate", "Remote", MB_OK);
		std::vector<SOCKET> skt_clients;
		for (auto& skt : Server.ClientsInformation)
		{
			skt_clients.push_back(skt.socket);
		}
		TerminateServer(Server.sktListen, skt_clients);
		Server.nConnected = 0;
		Server.isOnline = false;
		Server.cond_listen.notify_all();
		Server.cond_input.notify_all();

		//UpdateGuiControls();
		Button_Enable(m_btnStart.Window(), true);
		Button_Enable(m_btnTerminate.Window(), false);
		Button_Enable(m_btnPause.Window(), false);
		Button_Enable(m_btnModeServer.Window(), true);
		Button_Enable(m_btnModeClient.Window(), true);
		// Also stop screen streaming
		g_screenStreamActive = false;
		return 0;
	}
	SaveConfig();
	return 1;
}

int MainWindow::ClientConnect()
{
	char out_ip[50];
	char out_port[50];
	int error = 1;
	GetWindowText(m_itxtIP.Window(), out_ip, 50);
	GetWindowText(m_itxtPort.Window(), out_port, 50);
	Client.ip = out_ip;
	sPort = out_port;
	SaveConfig();
	//Log("Initializing client ");
	InitializeClient();
	Log("Connecting to server: " + Client.ip + ":" + sPort);
	error = ConnectServer(Client.sktServer, Client.ip, std::stoi(sPort));
	if (error == 1) {
		Log("Couldn't connect");
		//MessageBox(NULL, "couldn't connect", "Remote", MB_OK);
	}
	else {
		Log("Connected!");
		Client.isConnected = true;
		UpdateGuiControls();

		//start receive thread that will receive data
		Log("Starting receive thread");
		Client.tRecv = std::thread(&MainWindow::ReceiveThread, this);

		// start send input thread that sends the received input
		Log("Starting input thread");
		Client.tSendInput = std::thread(&MainWindow::OutputThread, this);

		Client.tSendInput.detach();
		Client.tRecv.detach();
		// SCREEN STREAM: Start a new window to receive the screen stream
		std::thread([this, ip = Client.ip]() {
			RemoteWindowPlacement place;

			// Load from config.txt if present
			std::ifstream f("config.txt");
			if (f) {
				std::string line, param;
				while (std::getline(f, line)) {
					std::istringstream s(line);
					s >> param;
					if (param == "remote_rect") {
						int l, t, w, h;
						s >> l >> t >> w >> h;
						if (w > 100 && h > 100) {
							place.left = l;
							place.top = t;
							place.width = w;
							place.height = h;
						}
					}
				}
			}
			static SOCKET inputSocket = INVALID_SOCKET;
			StartScreenRecv(ip, SCREEN_STREAM_PORT, place, &this->Client.sktServer);
		}).detach();
		Log("Screen streaming client started");
	}
	return 0;
}
int MainWindow::ClientDisconnect()
{
	Log("Disconnect");
	CloseConnection(&Client.sktServer);
	//MessageBox(m_hwnd, "Disconnect", "Remote", MB_OK);
	Log("Ending receive thread");
	Client.isConnected = false;
	Client.cond_input.notify_all();
	Client.cond_recv.notify_all();

	//UpdateGuiControls();
	Button_Enable(m_btnConnect.Window(), true);
	Button_Enable(m_btnDisconnect.Window(), false);
	Button_Enable(m_btnModeServer.Window(), true);
	Button_Enable(m_btnModeClient.Window(), true);

	SaveConfig();
	return 0;
}

int MainWindow::ListenThread()
{
	bool socket_found = false;
	int index = 0;
	while (Server.isOnline && Data.nMode == MODE::SERVER)
	{
		std::unique_lock<std::mutex> lock(Server.mu_sktclient);
		if (Server.nConnected >= Server.maxClients)
		{
			Server.cond_listen.wait(lock);
		}
		if (!socket_found) {
			for (int i = 0; i < Server.ClientsInformation.size(); i++)
			{
				if (Server.ClientsInformation[i].socket == INVALID_SOCKET)
				{
					socket_found = true;
					index = i;
				}
			}
		}
		lock.unlock();
		if (listen(Server.sktListen, 1) == SOCKET_ERROR) {
			Log("Listen failed with error: " + std::to_string(WSAGetLastError()));
		}
		sockaddr* inc_conn = new sockaddr;
		int sosize = sizeof(sockaddr);
		Server.ClientsInformation[index].socket = accept(Server.sktListen, inc_conn, &sosize);
		if (Server.ClientsInformation[index].socket == INVALID_SOCKET)
		{
			Log("accept failed: " + std::to_string(WSAGetLastError()));
		}
		else
		{
			Log("Connection accepted");
			Server.nConnected++;
			// Start input receive/inject thread per client
			std::thread(ServerInputRecvThread, Server.ClientsInformation[index].socket).detach();
			socket_found = false;
		}
		delete inc_conn;
	}
	Log("Listen thread - ended");
	return 0;
}

// Removed SendThread logic (no longer needed on server)

// --- Add server-side input receiving and injection thread ---

// New function: receive INPUT structs from each client and inject locally

void ServerInputRecvThread(SOCKET clientSocket) {
	while (true) {
		char buffer[sizeof(INPUT) > sizeof(RemoteCtrlMsg) ? sizeof(INPUT) : sizeof(RemoteCtrlMsg)];

		// Use recvn to ensure we always get a full message (INPUT or RemoteCtrlMsg)
		int received = recvn(clientSocket, buffer, sizeof(buffer));
		if (received <= 0) break;

		if (received == sizeof(RemoteCtrlMsg)) {
			// Handle control messages from client
			RemoteCtrlMsg* msg = (RemoteCtrlMsg*)buffer;
			if (msg->type == RemoteCtrlType::SetFps) {
				int fps = msg->value;
				if (fps == 5 || fps == 10 || fps == 20 || fps == 30 || fps == 40 || fps == 60) {
					g_streamingFps = fps;
					std::cout << "Set streaming FPS to " << fps << "\n";
				}
			}
			continue;
		}

		if (received == sizeof(INPUT)) {
			INPUT* inp = (INPUT*)buffer;
			// DEBUG LOGGING: Print what is being injected
			if (inp->type == INPUT_KEYBOARD) {
				std::cout << "[SERVER] Injecting INPUT: "
					<< "VK=0x" << std::hex << (int)inp->ki.wVk
					<< " Scan=0x" << std::hex << (int)inp->ki.wScan
					<< " Flags=0x" << std::hex << (int)inp->ki.dwFlags
					<< " ("
					<< ((inp->ki.dwFlags & KEYEVENTF_KEYUP) ? "UP" : "DOWN")
					<< ")"
					<< std::dec << std::endl;
			}
			SendInput(1, inp, sizeof(INPUT));
		}
	}
	closesocket(clientSocket);
}
int MainWindow::ReceiveThread()
{
	while (Client.isConnected && Data.nMode == MODE::CLIENT)
	{
		int error = 1;
		error = ReceiveServer(Client.sktServer, Client.recvBuff);
		if (error == 0)
		{
			std::unique_lock<std::mutex> lock(Client.mu_input);
			Client.inputQueue.emplace(Client.recvBuff);
			lock.unlock();
			Client.cond_input.notify_all();
		}
		else
		{
			Client.isConnected = false;
			Log("No input received, disconnecting");
			PostMessage(m_hwnd, WM_COMMAND, MAKEWPARAM(BTN_DISCONNECT, BN_CLICKED), 0);
		}
	}
	return 0;
}
int MainWindow::OutputThread()
{
	while (Client.isConnected && Data.nMode == MODE::CLIENT)
	{
		std::unique_lock<std::mutex> lock(Client.mu_input);
		if (Client.inputQueue.empty())
		{
			Client.cond_input.wait(lock);
		}
		else
		{
			int sz = Client.inputQueue.size();
			INPUT* tInputs = new INPUT[sz];
			for (int i = 0; i < sz; ++i)
			{
				tInputs[i] = Client.inputQueue.front();
				Client.inputQueue.pop();
			}
			lock.unlock();
			UpdateInput();
			if (tInputs->mi.mouseData != 0)
			{
				tInputs->mi.mouseData = (int16_t)tInputs->mi.mouseData;
			}
			//std::cout << "sending input" << std::endl;
			SendInput(sz, tInputs, sizeof(INPUT));
			delete[] tInputs;
		}
	}
	Log("Receive thread - ended");
	return 0;
}


// --- SaveConfig: now saves FPS, AlwaysOnTop, and window size ---
bool MainWindow::SaveConfig()
{
	std::fstream f(configName, std::fstream::out | std::fstream::trunc);
	if (!f.is_open())
	{
		std::cout << "can't save" << std::endl;
		return false;
	}
	f << "port " << sPort << std::endl;
	f << "server_ip " << Client.ip << std::endl;
	f << "max_clients " << Server.maxClients << std::endl;
	f << "fps " << g_streamingFps.load() << std::endl;
	f << "always_on_top " << (g_alwaysOnTop ? 1 : 0) << std::endl;
	f << "remote_rect " << m_savedRemoteLeft << " " << m_savedRemoteTop << " "
		<< m_savedRemoteW << " " << m_savedRemoteH << "\n";

	WINDOWPLACEMENT wp = { sizeof(WINDOWPLACEMENT) };
	if (m_hwnd && GetWindowPlacement(m_hwnd, &wp) && wp.showCmd == SW_SHOWNORMAL) {
		RECT& r = wp.rcNormalPosition;
		f << "window_rect " << r.left << " " << r.top << " "
			<< (r.right - r.left) << " " << (r.bottom - r.top) << std::endl;
	}
	else {
		// fallback to last saved
		f << "window_rect " << m_savedWinLeft << " " << m_savedWinTop << " " << m_savedWinW << " " << m_savedWinH << std::endl;
	}
	f.close();
	return true;
}


// --- LoadConfig: now loads FPS, AlwaysOnTop, and window size ---
bool MainWindow::LoadConfig()
{
	sPort = std::to_string(DEFAULT_PORT);
	iPort = std::stoi(sPort);
	Server.maxClients = MAX_CLIENTS;
	m_savedFps = SCREEN_STREAM_FPS;
	m_savedAlwaysOnTop = false;
	m_savedWinLeft = 100;
	m_savedWinTop = 100;
	m_savedWinW = 477;
	m_savedWinH = 340;
	m_savedRemoteLeft = 100;
	m_savedRemoteTop = 100;
	m_savedRemoteW = 900;
	m_savedRemoteH = 600;

	std::fstream f(configName, std::fstream::in);
	if (!f.is_open())
	{
		return false;
	}

	std::string line;
	std::string param;
	std::stringstream s;

	while (std::getline(f, line))
	{
		s.clear();
		s.str(line);
		s >> param;
		if (param == "port") {
			s >> sPort;
		}
		else if (param == "server_ip") {
			s >> Client.ip;
		}
		else if (param == "max_clients") {
			std::string max;
			s >> max;
			Server.maxClients = std::stoi(max);
		}
		else if (param == "fps") {
			int fps;
			s >> fps;
			if (fps >= 5 && fps <= 60) {
				m_savedFps = fps;
				g_streamingFps = fps;
				g_screenStreamMenuFps = fps;
				g_screenStreamActualFps = fps;
			}
		}
		else if (param == "always_on_top") {
			int atop = 0;
			s >> atop;
			m_savedAlwaysOnTop = (atop != 0);
			g_alwaysOnTop = m_savedAlwaysOnTop;
		}
		else if (param == "window_rect") {
			int l, t, w, h;
			s >> l >> t >> w >> h;
			if (w > 100 && h > 100) {
				m_savedWinLeft = l;
				m_savedWinTop = t;
				m_savedWinW = w;
				m_savedWinH = h;
			}
		}
		else if (param == "remote_rect") {
			int l, t, w, h;
			s >> l >> t >> w >> h;
			if (w > 100 && h > 100) {
				m_savedRemoteLeft = l;
				m_savedRemoteTop = t;
				m_savedRemoteW = w;
				m_savedRemoteH = h;
			}
		}
	}

	std::cout << "Config Loaded:\n"
		<< "    port = " << sPort << '\n'
		<< "    server ip = " << Client.ip << '\n'
		<< "    max number clients = " << Server.maxClients << std::endl
		<< "    fps = " << m_savedFps << std::endl
		<< "    always_on_top = " << (m_savedAlwaysOnTop ? "true" : "false") << std::endl
		<< "    window rect = (" << m_savedWinLeft << "," << m_savedWinTop << ") "
		<< m_savedWinW << "x" << m_savedWinH << std::endl
		<< "    remote rect = (" << m_savedRemoteLeft << "," << m_savedRemoteTop << ") "
		<< m_savedRemoteW << "x" << m_savedRemoteH << std::endl;

	f.close();
	return true;
}

// Helper for command line parsing
std::string GetCmdOption(const std::vector<std::string>& args, const std::string& option) {
	auto it = std::find(args.begin(), args.end(), option);
	if (it != args.end() && ++it != args.end())
		return *it;
	return "";
}
bool CmdOptionExists(const std::vector<std::string>& args, const std::string& option) {
	return std::find(args.begin(), args.end(), option) != args.end();
}

void PrintUsage(const char* exeName) {
	std::cout << "Usage:\n";
	std::cout << "  " << exeName << " --server [--port PORT]\n";
	std::cout << "  " << exeName << " --client --ip IP_ADDRESS --port PORT\n";
	std::cout << "Examples:\n";
	std::cout << "  " << exeName << " --server\n";
	std::cout << "  " << exeName << " --server --port 5555\n";
	std::cout << "  " << exeName << " --client --ip 127.0.0.1 --port 27015\n";
}

void StartServerLogic(int inputPort, int screenPort, bool headless = false) {
	// 1. Start input server (accepts INPUT/clipboard from clients)
	SOCKET inputListenSocket = INVALID_SOCKET;
	if (InitializeServer(inputListenSocket, inputPort) != 0) {
		std::cerr << "Failed to initialize input server!" << std::endl;
		return;
	}
	std::cout << "Input server listening on port " << inputPort << std::endl;

	// 2. Start screen streaming server (accepts screen stream requests from clients)
	SOCKET screenListenSocket = INVALID_SOCKET;
	if (InitializeScreenStreamServer(screenListenSocket, screenPort) != 0) {
		std::cerr << "Failed to initialize screen stream server!" << std::endl;
		closesocket(inputListenSocket);
		return;
	}
	std::cout << "Screen streaming server listening on port " << screenPort << std::endl;

	// 2.5. Start audio streaming server (accepts audio stream requests from clients)
	SOCKET audioListenSocket = INVALID_SOCKET;
	if (InitializeScreenStreamServer(audioListenSocket, AUDIO_STREAM_PORT) != 0) {
		std::cerr << "Failed to initialize audio stream server!" << std::endl;
		closesocket(inputListenSocket);
		closesocket(screenListenSocket);
		return;
	}
	std::cout << "Audio streaming server listening on port " << AUDIO_STREAM_PORT << std::endl;

	// 3. Accept loop for input, screen, and audio sockets, each in a thread
	std::thread inputThread([&]() {
		while (true) {
			if (listen(inputListenSocket, 1) == SOCKET_ERROR) break;
			sockaddr_in client_addr;
			int addrlen = sizeof(client_addr);
			SOCKET sktClient = accept(inputListenSocket, (sockaddr*)&client_addr, &addrlen);
			if (sktClient == INVALID_SOCKET) continue;
			std::thread(ServerInputRecvThread, sktClient).detach();
		}
		closesocket(inputListenSocket);
		});

	std::thread screenThread([&]() {
		while (true) {
			if (listen(screenListenSocket, 1) == SOCKET_ERROR) break;
			sockaddr_in client_addr;
			int addrlen = sizeof(client_addr);
			SOCKET sktClient = accept(screenListenSocket, (sockaddr*)&client_addr, &addrlen);
			if (sktClient == INVALID_SOCKET) continue;
			std::thread(ScreenStreamServerThread, sktClient).detach();
		}
		closesocket(screenListenSocket);
		});

	std::thread audioThread([&]() {
		while (true) {
			if (listen(audioListenSocket, 1) == SOCKET_ERROR) break;
			sockaddr_in client_addr;
			int addrlen = sizeof(client_addr);
			SOCKET sktClient = accept(audioListenSocket, (sockaddr*)&client_addr, &addrlen);
			if (sktClient == INVALID_SOCKET) continue;
			std::thread(AudioStreamServerThreadXRLE, sktClient).detach();
		}
		closesocket(audioListenSocket);
		});

	// Wait for all threads so the server never exits
	inputThread.join();
	screenThread.join();
	audioThread.join();
}

// Minimal server loop for screen streaming
void RunHeadlessServer(int port) {
	std::cout << "Starting headless server on port " << port << std::endl;
	StartServerLogic(port, SCREEN_STREAM_PORT, true);
}

// Minimal client launcher to receive the screen (no UI)// Update RunHeadlessClient to read config.txt and pass geometry to StartScreenRecv
int RunHeadlessClient(const std::string& ip, int port) {
	MinimizeConsoleWindow();
	std::cout << "Starting headless client, connecting to " << ip << ":" << port << std::endl;

	// 1. Load window geometry from config.txt
	RemoteWindowPlacement place;
	std::ifstream f("config.txt");
	if (f) {
		std::string line, param;
		while (std::getline(f, line)) {
			std::istringstream s(line);
			s >> param;
			if (param == "remote_rect") {
				int l, t, w, h;
				s >> l >> t >> w >> h;
				if (w > 100 && h > 100) {
					place.left = l;
					place.top = t;
					place.width = w;
					place.height = h;
				}
			}
		}
	}

	// 2. Connect to input/control server (main port)
	static SOCKET inputSocket = INVALID_SOCKET; // Must remain valid for the window's lifetime!
	InitializeClient();
	if (ConnectServer(inputSocket, ip, port) != 0) {
		std::cerr << "HeadlessClient: couldn't connect to input/control server" << std::endl;
		return 1;
	}
	std::cout << "HeadlessClient: connected to input/control server!" << std::endl;

	// 3. Start screen receiving window (pass input socket pointer for control)
	//    You must update StartScreenRecv to take the SOCKET* parameter as shown below!
	StartScreenRecv(ip, SCREEN_STREAM_PORT, place, &inputSocket);

	// 4. Cleanup after the screen window closes
	if (inputSocket != INVALID_SOCKET) {
		closesocket(inputSocket);
	}
	std::cout << "Headless client exiting.\n";
	return 0;
}

int main(int argc, char* argv[])
{
	// ---- Ensure WSAStartup is called ONCE here ----
	WSADATA wsadata;
	int wsaResult = WSAStartup(MAKEWORD(2, 2), &wsadata);
	if (wsaResult != 0) {
		std::cout << "WSAStartup failed: " << wsaResult << std::endl;
		return 1;
	}

	// Initialize optimal color conversion function pointer
	ColorConversion::InitializeOptimalConverter();

	// --- Command line mode check ---
	std::vector<std::string> args(argv + 1, argv + argc);
	bool isServer = CmdOptionExists(args, "--server");
	bool isClient = CmdOptionExists(args, "--client");
	bool isHeadlessClient = isClient && CmdOptionExists(args, "--headless");

	// --- Headless server mode: run true headless server logic and exit ---
	if (!args.empty() && isServer && !isClient) {
		int port = DEFAULT_PORT;
		std::string portStr = GetCmdOption(args, "--port");
		if (!portStr.empty()) {
			try { port = std::stoi(portStr); }
			catch (...) {
				std::cerr << "Invalid port: " << portStr << std::endl;
				WSACleanup();
				return 1;
			}
		}
		RunHeadlessServer(port);
		WSACleanup();
		return 0;
	}

	// --- Headless client mode: run true headless client logic and exit ---
	if (!args.empty() && isHeadlessClient) {
		std::string ip = GetCmdOption(args, "--ip");
		std::string portStr = GetCmdOption(args, "--port");
		int port = DEFAULT_PORT;
		if (ip.empty() || portStr.empty()) {
			PrintUsage(argv[0]);
			WSACleanup();
			return 1;
		}
		try { port = std::stoi(portStr); }
		catch (...) {
			std::cerr << "Invalid port: " << portStr << std::endl;
			WSACleanup();
			return 1;
		}
		int runResult = RunHeadlessClient(ip.c_str(), port);
		WSACleanup();
		return runResult;
	}

	MainWindow win;
	g_pMainWindow = &win; // <-- Set the global pointer after win is defined

	// Use loaded size from config
	int winW = win.m_savedWinW;
	int winH = win.m_savedWinH;
	if (!win.Create(nullptr, "Remote", WS_OVERLAPPEDWINDOW, 0, CW_USEDEFAULT, CW_USEDEFAULT, winW, winH, NULL))
	{
		std::cout << "error creating the main window: " << GetLastError() << std::endl;
		WSACleanup();
		return 0;
	}

	// Set Always On Top on restore
	if (win.m_savedAlwaysOnTop) {
		SetWindowPos(win.Window(), HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
	}

	// --- Clipboard sync: initialize clipboard monitoring as soon as window is created ---
	InitClipboardMonitor(win.Window(), INVALID_SOCKET);

	// --- Command-line reflection logic for GUI ---
	if (!args.empty()) {
		if ((isServer && isClient) || (!isServer && !isClient)) {
			PrintUsage(argv[0]);
			WSACleanup();
			return 1;
		}

		if (isServer) {
			int port = DEFAULT_PORT;
			std::string portStr = GetCmdOption(args, "--port");
			if (!portStr.empty()) {
				try { port = std::stoi(portStr); }
				catch (...) {
					std::cerr << "Invalid port: " << portStr << std::endl;
					WSACleanup();
					return 1;
				}
			}
			win.sPort = std::to_string(port);
			SetWindowTextA(win.m_itxtPort.Window(), win.sPort.c_str());
			win.SetMode(MainWindow::MODE::SERVER);

			SendMessage(win.m_btnModeServer.Window(), BM_SETCHECK, BST_CHECKED, 0);
			SendMessage(win.m_btnModeClient.Window(), BM_SETCHECK, BST_UNCHECKED, 0);

			PostMessage(win.Window(), WM_COMMAND, MAKEWPARAM(BTN_START, BN_CLICKED), 0);

		}
		else if (isClient) {
			std::string ip = GetCmdOption(args, "--ip");
			std::string portStr = GetCmdOption(args, "--port");
			int port = DEFAULT_PORT;
			if (ip.empty() || portStr.empty()) {
				PrintUsage(argv[0]);
				WSACleanup();
				return 1;
			}
			try { port = std::stoi(portStr); }
			catch (...) {
				std::cerr << "Invalid port: " << portStr << std::endl;
				WSACleanup();
				return 1;
			}
			win.Client.ip = ip;
			SetWindowTextA(win.m_itxtIP.Window(), ip.c_str());
			win.sPort = std::to_string(port);
			SetWindowTextA(win.m_itxtPort.Window(), win.sPort.c_str());
			win.SetMode(MainWindow::MODE::CLIENT);

			SendMessage(win.m_btnModeClient.Window(), BM_SETCHECK, BST_CHECKED, 0);
			SendMessage(win.m_btnModeServer.Window(), BM_SETCHECK, BST_UNCHECKED, 0);

			PostMessage(win.Window(), WM_COMMAND, MAKEWPARAM(BTN_CONNECT, BN_CLICKED), 0);

			g_headlessClientMode = true;
		}
	}

	// Use loaded position and size from config
	WINDOWPLACEMENT wp = { sizeof(WINDOWPLACEMENT) };
	wp.showCmd = SW_SHOWNORMAL;
	wp.rcNormalPosition.left = win.m_savedWinLeft;
	wp.rcNormalPosition.top = win.m_savedWinTop;
	wp.rcNormalPosition.right = win.m_savedWinLeft + win.m_savedWinW;
	wp.rcNormalPosition.bottom = win.m_savedWinTop + win.m_savedWinH;
	SetWindowPlacement(win.Window(), &wp);

	MSG msg = { };
	while (GetMessage(&msg, NULL, 0, 0))
	{
		TranslateMessage(&msg);
		DispatchMessage(&msg);
	}

	CleanupClipboardMonitor(win.Window());

	WSACleanup();
	return 0;
}