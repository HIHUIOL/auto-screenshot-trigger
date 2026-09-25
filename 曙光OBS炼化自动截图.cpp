#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <iostream>
#include <string>
#include <fstream>
#include <vector>
#include <iomanip>
#include <algorithm>
#include <shlwapi.h>
#include <chrono>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <map>
#include <atomic>
#include <shlobj.h>
#include <filesystem>
#include <dshow.h>
#include <deque>
#include <sstream>
#include <opencv2/opencv.hpp>
#include <gdiplus.h>

// MMCSS 多媒体调度相关头文件
#include <avrt.h>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "Shlwapi.lib")
#pragma comment(lib, "Shell32.lib")
#pragma comment(lib, "strmiids.lib")
#pragma comment(lib, "avrt.lib")
#pragma comment(lib, "gdiplus.lib")

// ================= 缺失的GUID定义 =================
#ifndef MEDIASUBTYPE_I420
static const GUID MEDIASUBTYPE_I420 =
{ 0x30323449, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71 } };
#endif

using namespace Gdiplus;

namespace fs = std::filesystem;

// ================= 全局配置 =================
const std::wstring CONFIG_NAME = L"曙光截图配置.ini";
const std::wstring SAMPLE_IMAGE = L"曙光截图样本.png";
const std::wstring DEFAULT_SAVE_DIR = L"ScreenShot";
const int BASE_WIDTH = 1920;
const int BASE_HEIGHT = 1080;

struct AppConfig {
    int x = 0, y = 0, w = 0, h = 0;
    int coin_x = 1712, coin_y = 34, coin_w = 125, coin_h = 25;
    int 相似度阈值 = 98;
    int 颜色容差 = 15;
    int 连续判定帧数 = 4;
    std::wstring 保存路径 = DEFAULT_SAVE_DIR;
    int 线程池大小 = 4;
    int 摄像头索引 = 0;
    bool 启用MMCSS = true;
    int 亮度 = 50;
    int 对比度 = 50;
    int 饱和度 = 50;
    int 色调 = 50;
    int 视频格式 = 0;
    int 目标宽度 = 1920;
    int 目标高度 = 1080;
    int 目标帧率 = 30;
    int 缓冲大小 = 1;
    int 亮度最大值 = 100;
    int 对比度最大值 = 100;
    int 饱和度最大值 = 100;
    int 色调最大值 = 100;
    int JPEG质量 = 90;  // 新增：JPEG质量，可在配置文件中修改
} g_config;

// ================= 摄像头参数调节（滑条实时调节）=================
// 参数调节窗口当前使用的设备句柄：滑条回调需要访问该句柄下发参数
static cv::VideoCapture* g_paramCap = nullptr;

// 滑条回调：把四个色彩参数实时下发到摄像头（硬件级生效，零延迟）
static void OnColorParamChanged(int, void*) {
    if (!g_paramCap) return;
    g_paramCap->set(cv::CAP_PROP_BRIGHTNESS, g_config.亮度);
    g_paramCap->set(cv::CAP_PROP_CONTRAST, g_config.对比度);
    g_paramCap->set(cv::CAP_PROP_SATURATION, g_config.饱和度);
    g_paramCap->set(cv::CAP_PROP_HUE, g_config.色调);
}

struct CameraCapability {
    int width;
    int height;
    int fps;
    std::wstring formatName;
    int fourcc;
};

struct WorkerTask { uint64_t id; cv::Mat frame; };
struct WorkerResult { uint64_t id; double sim; cv::Mat frame; };

std::queue<WorkerTask> g_workerQueue;
std::mutex g_workerMtx;
std::condition_variable g_workerCv;

std::map<uint64_t, WorkerResult> g_reorderBuffer;
std::mutex g_reorderMtx;
std::condition_variable g_reorderCv;

std::atomic<bool> g_running{ false };
std::atomic<uint64_t> g_nextProduceId{ 1 };
std::atomic<uint64_t> g_nextExpectedId{ 1 };

std::atomic<double> g_lastSim{ 0.0 };
std::atomic<bool> g_isLocked{ false };
std::atomic<int> g_savedFileCount{ 0 };
std::atomic<int> g_currentHits{ 0 };

int g_nextFileIndex = 1;

std::mutex g_displayMtx;
cv::Mat g_displayFrame;
std::atomic<bool> g_debugWindowRunning{ false };
std::atomic<bool> g_debugWindowQuit{ false };
std::atomic<bool> g_debugWindowClosed{ false };

// ================= 中文绘制辅助函数 =================
void putChineseText(cv::Mat& img, const std::wstring& text, cv::Point org,
    int fontSize = 24, cv::Scalar color = cv::Scalar(0, 255, 0),
    int thickness = 2) {
    Gdiplus::Bitmap bitmap(img.cols, img.rows, img.step, PixelFormat24bppRGB, img.data);
    Gdiplus::Graphics graphics(&bitmap);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    graphics.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAlias);
    Gdiplus::FontFamily fontFamily(L"微软雅黑");
    Gdiplus::Font font(&fontFamily, fontSize, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
    Gdiplus::SolidBrush brush(Gdiplus::Color(255, (BYTE)color[2], (BYTE)color[1], (BYTE)color[0]));
    Gdiplus::PointF point((REAL)org.x, (REAL)org.y);
    graphics.DrawString(text.c_str(), -1, &font, point, &brush);
}

// 在指定区域叠加半透明深色底板：让 HUD 文字在任何画面上都清晰可读
void drawHudBackground(cv::Mat& img, const cv::Rect& rect, double alpha = 0.55) {
    cv::Rect roi = rect & cv::Rect(0, 0, img.cols, img.rows);
    if (roi.area() <= 0) return;
    cv::Mat region = img(roi);
    cv::Mat dark = region.clone();
    dark.setTo(cv::Scalar(16, 16, 16));          // 近黑底色
    cv::addWeighted(dark, alpha, region, 1.0 - alpha, 0, region);
}

// ================= MMCSS 多媒体调度类 =================
class MMCSSManager {
private:
    HANDLE m_hTask = nullptr;
    DWORD m_taskIndex = 0;
    bool m_registered = false;
public:
    ~MMCSSManager() { Unregister(); }
    bool Register(LPCWSTR taskName = L"Capture", LPCWSTR taskClass = L"Capture") {
        if (m_registered) return true;
        m_hTask = AvSetMmThreadCharacteristicsW(taskClass, &m_taskIndex);
        if (m_hTask == 0) return false;
        AvSetMmThreadPriority(m_hTask, AVRT_PRIORITY_HIGH);
        m_registered = true;
        return true;
    }
    void Unregister() {
        if (m_hTask != nullptr && m_hTask != 0) {
            AvRevertMmThreadCharacteristics(m_hTask);
            m_hTask = nullptr;
        }
        m_registered = false;
        m_taskIndex = 0;
    }
};

// ================= 基础工具 =================
std::wstring GetAppDir() {
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(NULL, exePath, MAX_PATH);
    PathRemoveFileSpecW(exePath);
    return std::wstring(exePath);
}

std::string WStrToUTF8(const std::wstring& wstr) {
    if (wstr.empty()) return "";
    int size = WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), NULL, 0, NULL, NULL);
    std::string str(size, 0);
    WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), &str[0], size, NULL, NULL);
    return str;
}

std::wstring UTF8ToWStr(const std::string& str) {
    if (str.empty()) return L"";
    int size = MultiByteToWideChar(CP_UTF8, 0, &str[0], (int)str.size(), NULL, 0);
    std::wstring wstr(size, 0);
    MultiByteToWideChar(CP_UTF8, 0, &str[0], (int)str.size(), &wstr[0], size);
    return wstr;
}

void EnsureDirectory(const std::wstring& path) {
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        CreateDirectoryW(path.c_str(), NULL);
    }
}

cv::Mat ReadImage(const std::wstring& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return cv::Mat();
    std::vector<uchar> buffer((std::istreambuf_iterator<char>(file)), {});
    return cv::imdecode(buffer, cv::IMREAD_COLOR);
}

void WriteImage(const std::wstring& path, const cv::Mat& img) {
    std::vector<uchar> buf;
    std::vector<int> params = { cv::IMWRITE_JPEG_QUALITY, g_config.JPEG质量 };
    cv::imencode(".jpg", img, buf, params);
    std::ofstream file(path, std::ios::binary);
    if (file) file.write(reinterpret_cast<const char*>(buf.data()), buf.size());
}

int GetNextImageIndex(const std::wstring& dirPath) {
    int maxIdx = 0;
    if (!fs::exists(dirPath)) return 1;
    for (const auto& entry : fs::directory_iterator(dirPath)) {
        if (entry.is_regular_file() && entry.path().extension() == L".jpg") {
            try {
                int idx = std::stoi(entry.path().stem().wstring());
                if (idx > maxIdx) maxIdx = idx;
            }
            catch (...) {}
        }
    }
    return maxIdx + 1;
}

std::string FormatDuration(std::chrono::seconds seconds) {
    long long s = seconds.count();
    long long h = s / 3600;
    long long m = (s % 3600) / 60;
    long long sec = s % 60;
    char buf[64];
    snprintf(buf, sizeof(buf), "%02lld:%02lld:%02lld", h, m, sec);
    return std::string(buf);
}

std::string GetFormatName(int format) {
    switch (format) {
    case 0: return "YUY2";
    case 1: return "MJPG";
    case 2: return "NV12";
    case 3: return "RGB24";
    case 4: return "I420";
    case 5: return "H264";
    default: return "未知";
    }
}

std::wstring GetFormatNameW(int format) {
    switch (format) {
    case 0: return L"YUY2";
    case 1: return L"MJPG";
    case 2: return L"NV12";
    case 3: return L"RGB24";
    case 4: return L"I420";
    case 5: return L"H264";
    default: return L"未知";
    }
}

// ================= 控制台状态显示辅助函数 =================
void UpdateStatusLine(const std::string& status, int statusLineRow = 0) {
    HANDLE hConsole = GetStdHandle(STD_OUTPUT_HANDLE);
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    if (!GetConsoleScreenBufferInfo(hConsole, &csbi)) return;
    int consoleWidth = csbi.srWindow.Right - csbi.srWindow.Left + 1;
    std::string displayStr = status;
    if ((int)displayStr.length() > consoleWidth) {
        displayStr = displayStr.substr(0, consoleWidth);
    }
    COORD originalPos = csbi.dwCursorPosition;
    COORD statusPos = { 0, (SHORT)statusLineRow };
    SetConsoleCursorPosition(hConsole, statusPos);
    DWORD written;
    FillConsoleOutputCharacterA(hConsole, ' ', consoleWidth, statusPos, &written);
    FillConsoleOutputAttribute(hConsole, csbi.wAttributes, consoleWidth, statusPos, &written);
    SetConsoleCursorPosition(hConsole, statusPos);
    std::cout << displayStr;
    SetConsoleCursorPosition(hConsole, originalPos);
}

void SaveConfig() {
    std::ofstream out(GetAppDir() + L"\\" + CONFIG_NAME, std::ios::binary);
    unsigned char bom[] = { 0xEF, 0xBB, 0xBF }; out.write((char*)bom, 3);
    auto writeLine = [&](const std::wstring& key, const std::wstring& val) {
        std::string line = WStrToUTF8(key + L"=" + val + L"\n");
        out.write(line.c_str(), line.size());
        };
    writeLine(L"[配置]", L"");
    writeLine(L"目标_x", std::to_wstring(g_config.x));
    writeLine(L"目标_y", std::to_wstring(g_config.y));
    writeLine(L"目标_w", std::to_wstring(g_config.w));
    writeLine(L"目标_h", std::to_wstring(g_config.h));
    writeLine(L"金币_x", std::to_wstring(g_config.coin_x));
    writeLine(L"金币_y", std::to_wstring(g_config.coin_y));
    writeLine(L"金币_w", std::to_wstring(g_config.coin_w));
    writeLine(L"金币_h", std::to_wstring(g_config.coin_h));
    writeLine(L"相似度阈值", std::to_wstring(g_config.相似度阈值));
    writeLine(L"颜色容差", std::to_wstring(g_config.颜色容差));
    writeLine(L"连续判定帧数", std::to_wstring(g_config.连续判定帧数));
    writeLine(L"保存路径", g_config.保存路径);
    writeLine(L"线程池大小", std::to_wstring(g_config.线程池大小));
    writeLine(L"摄像头索引", std::to_wstring(g_config.摄像头索引));
    writeLine(L"启用MMCSS", g_config.启用MMCSS ? L"1" : L"0");
    writeLine(L"亮度", std::to_wstring(g_config.亮度));
    writeLine(L"对比度", std::to_wstring(g_config.对比度));
    writeLine(L"饱和度", std::to_wstring(g_config.饱和度));
    writeLine(L"色调", std::to_wstring(g_config.色调));
    writeLine(L"视频格式", std::to_wstring(g_config.视频格式));
    writeLine(L"目标宽度", std::to_wstring(g_config.目标宽度));
    writeLine(L"目标高度", std::to_wstring(g_config.目标高度));
    writeLine(L"目标帧率", std::to_wstring(g_config.目标帧率));
    writeLine(L"缓冲大小", std::to_wstring(g_config.缓冲大小));
    writeLine(L"JPEG质量", std::to_wstring(g_config.JPEG质量));
}

bool LoadConfig() {
    std::ifstream in(GetAppDir() + L"\\" + CONFIG_NAME, std::ios::binary);
    if (!in.is_open()) return false;
    char bom[3]; in.read(bom, 3);
    if (!(bom[0] == (char)0xEF && bom[1] == (char)0xBB && bom[2] == (char)0xBF)) in.seekg(0);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        size_t pos = line.find('='); if (pos == std::string::npos) continue;
        std::wstring key = UTF8ToWStr(line.substr(0, pos));
        std::wstring val = UTF8ToWStr(line.substr(pos + 1));
        try {
            if (key == L"相似度阈值") g_config.相似度阈值 = std::stoi(val);
            else if (key == L"颜色容差") g_config.颜色容差 = std::stoi(val);
            else if (key == L"连续判定帧数") g_config.连续判定帧数 = std::stoi(val);
            else if (key == L"保存路径") g_config.保存路径 = val;
            else if (key == L"线程池大小") g_config.线程池大小 = std::stoi(val);
            else if (key == L"摄像头索引") g_config.摄像头索引 = std::stoi(val);
            else if (key == L"启用MMCSS") g_config.启用MMCSS = (std::stoi(val) != 0);
            else if (key == L"亮度") g_config.亮度 = std::stoi(val);
            else if (key == L"对比度") g_config.对比度 = std::stoi(val);
            else if (key == L"饱和度") g_config.饱和度 = std::stoi(val);
            else if (key == L"色调") g_config.色调 = std::stoi(val);
            else if (key == L"视频格式") g_config.视频格式 = std::stoi(val);
            else if (key == L"目标宽度") g_config.目标宽度 = std::stoi(val);
            else if (key == L"目标高度") g_config.目标高度 = std::stoi(val);
            else if (key == L"目标帧率") g_config.目标帧率 = std::stoi(val);
            else if (key == L"缓冲大小") g_config.缓冲大小 = std::stoi(val);
            else if (key == L"JPEG质量") g_config.JPEG质量 = std::stoi(val);
            else if (key == L"目标_x") g_config.x = std::stoi(val);
            else if (key == L"目标_y") g_config.y = std::stoi(val);
            else if (key == L"目标_w") g_config.w = std::stoi(val);
            else if (key == L"目标_h") g_config.h = std::stoi(val);
            else if (key == L"金币_x") g_config.coin_x = std::stoi(val);
            else if (key == L"金币_y") g_config.coin_y = std::stoi(val);
            else if (key == L"金币_w") g_config.coin_w = std::stoi(val);
            else if (key == L"金币_h") g_config.coin_h = std::stoi(val);
        }
        catch (...) { continue; }
    }
    EnsureDirectory(GetAppDir() + L"\\" + g_config.保存路径);
    return (g_config.w > 0);
}

// ================= 核心算法 =================
double CalculateSimilarityRaw(const cv::Mat& m1, const cv::Mat& m2, int tolerance) {
    if (m1.empty() || m2.empty() || m1.cols != m2.cols || m1.rows != m2.rows) return 0.0;
    long long matches = 0;
    int totalPixels = m1.cols * m1.rows;
    for (int r = 0; r < m1.rows; ++r) {
        const cv::Vec3b* p1 = m1.ptr<cv::Vec3b>(r);
        const cv::Vec3b* p2 = m2.ptr<cv::Vec3b>(r);
        for (int c = 0; c < m1.cols; ++c) {
            if (abs(p1[c][0] - p2[c][0]) <= tolerance &&
                abs(p1[c][1] - p2[c][1]) <= tolerance &&
                abs(p1[c][2] - p2[c][2]) <= tolerance) {
                matches++;
            }
        }
    }
    return (double)matches / totalPixels * 100.0;
}

double CalculateRobustSimilarity(const cv::Mat& m1, const cv::Mat& m2, int tolerance) {
    if (m1.empty() || m2.empty() || m1.size() != m2.size()) return 0.0;
    cv::Mat gray1, gray2;
    if (m1.channels() == 3) cv::cvtColor(m1, gray1, cv::COLOR_BGR2GRAY); else gray1 = m1.clone();
    if (m2.channels() == 3) cv::cvtColor(m2, gray2, cv::COLOR_BGR2GRAY); else gray2 = m2.clone();
    cv::GaussianBlur(gray1, gray1, cv::Size(3, 3), 0);
    cv::GaussianBlur(gray2, gray2, cv::Size(3, 3), 0);
    cv::Mat diff;
    cv::absdiff(gray1, gray2, diff);
    int matchCount = 0;
    int totalPixels = diff.rows * diff.cols;
    for (int r = 0; r < diff.rows; ++r) {
        const uchar* p = diff.ptr<uchar>(r);
        for (int c = 0; c < diff.cols; ++c) {
            if (p[c] <= tolerance) matchCount++;
        }
    }
    return (double)matchCount / totalPixels * 100.0;
}

// ================= 调试窗口显示线程 =================
void DebugDisplayThread() {
    MMCSSManager mmcss;
    mmcss.Register(L"DisplayThread", L"Capture");
    cv::namedWindow("监控预览 (调试)", cv::WINDOW_NORMAL | cv::WINDOW_GUI_NORMAL);
    cv::resizeWindow("监控预览 (调试)", BASE_WIDTH, BASE_HEIGHT);
    g_debugWindowRunning.store(true);
    while (!g_debugWindowQuit.load()) {
        cv::Mat frameToShow;
        {
            std::lock_guard<std::mutex> lock(g_displayMtx);
            if (!g_displayFrame.empty()) frameToShow = g_displayFrame.clone();
        }
        if (!frameToShow.empty()) {
            cv::imshow("监控预览 (调试)", frameToShow);
        }
        else {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        int key = cv::waitKey(16);
        try {
            if (cv::getWindowProperty("监控预览 (调试)", cv::WND_PROP_VISIBLE) < 1) {
                g_debugWindowClosed.store(true);
                g_debugWindowQuit.store(true);
                break;
            }
        }
        catch (...) {
            g_debugWindowClosed.store(true);
            g_debugWindowQuit.store(true);
            break;
        }
        if (key == 27) {
            g_debugWindowQuit.store(true);
            break;
        }
    }
    try {
        if (cv::getWindowProperty("监控预览 (调试)", cv::WND_PROP_VISIBLE) >= 0) {
            cv::destroyWindow("监控预览 (调试)");
        }
    }
    catch (...) {}
    g_debugWindowRunning.store(false);
}

// ================= 后台严格顺序异步保存线程 =================
void AsyncWriterThread() {
    MMCSSManager mmcss;
    mmcss.Register(L"WriterThread", L"Capture");
    std::wstring dir = GetAppDir() + L"\\" + g_config.保存路径;
    bool isReadyToCapture = true;
    cv::Mat dedupeBuffer;
    bool isBufferInitialized = false;
    int consecutiveHits = 0;
    while (true) {
        WorkerResult result;
        bool hasTask = false;
        {
            std::unique_lock<std::mutex> lock(g_reorderMtx);
            g_reorderCv.wait(lock, [] { return g_reorderBuffer.count(g_nextExpectedId) > 0 || !g_running; });
            if (g_reorderBuffer.count(g_nextExpectedId) > 0) {
                result = g_reorderBuffer[g_nextExpectedId];
                g_reorderBuffer.erase(g_nextExpectedId);
                g_nextExpectedId++;
                hasTask = true;
            }
            else if (!g_running) {
                break;
            }
        }
        if (hasTask) {
            g_lastSim.store(result.sim);
            if (result.sim >= (double)g_config.相似度阈值) {
                consecutiveHits++;
                g_currentHits.store(consecutiveHits);
                if (consecutiveHits >= g_config.连续判定帧数 && isReadyToCapture) {
                    bool shouldSave = true;
                    cv::Rect dedupeRoi;
                    if (g_config.coin_w > 0) {
                        dedupeRoi = cv::Rect(g_config.coin_x, g_config.coin_y, g_config.coin_w, g_config.coin_h);
                    }
                    else {
                        dedupeRoi = cv::Rect(0, 0, result.frame.cols, result.frame.rows);
                    }
                    dedupeRoi &= cv::Rect(0, 0, result.frame.cols, result.frame.rows);
                    cv::Mat currentDedupeMat = result.frame(dedupeRoi).clone();
                    if (!isBufferInitialized) {
                        if (g_nextFileIndex > 1) {
                            std::wstring lastImagePath = dir + L"\\" + std::to_wstring(g_nextFileIndex - 1) + L".jpg";
                            cv::Mat lastDiskMat = ReadImage(lastImagePath);
                            if (!lastDiskMat.empty() && lastDiskMat.size() == result.frame.size()) {
                                cv::Mat lastDedupeMat = lastDiskMat(dedupeRoi);
                                double simDedupe = CalculateRobustSimilarity(currentDedupeMat, lastDedupeMat, g_config.颜色容差);
                                if (simDedupe >= (double)g_config.相似度阈值) shouldSave = false;
                            }
                        }
                        isBufferInitialized = true;
                    }
                    else {
                        if (!dedupeBuffer.empty() && dedupeBuffer.size() == currentDedupeMat.size()) {
                            double simDedupe = CalculateRobustSimilarity(currentDedupeMat, dedupeBuffer, g_config.颜色容差);
                            if (simDedupe >= (double)g_config.相似度阈值) shouldSave = false;
                        }
                    }
                    if (shouldSave) {
                        std::wstring p = dir + L"\\" + std::to_wstring(g_nextFileIndex++) + L".jpg";
                        WriteImage(p, result.frame);
                    }
                    dedupeBuffer = currentDedupeMat;
                    isReadyToCapture = false;
                    g_isLocked.store(true);
                }
            }
            else {
                consecutiveHits = 0;
                g_currentHits.store(0);
                isReadyToCapture = true;
                g_isLocked.store(false);
            }
            g_savedFileCount.store(g_nextFileIndex - 1);
        }
    }
}

// ================= 通过DirectShow枚举所有摄像头设备 =================
std::vector<std::pair<int, std::wstring>> EnumerateCameraDevices() {
    std::vector<std::pair<int, std::wstring>> devices;
    ICreateDevEnum* pDevEnum = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_SystemDeviceEnum, nullptr, CLSCTX_INPROC, IID_ICreateDevEnum, (void**)&pDevEnum);
    if (FAILED(hr)) {
        for (int i = 0; i <= 20; i++) {
            cv::VideoCapture cap(i, cv::CAP_DSHOW);
            if (cap.isOpened()) {
                devices.push_back({ i, L"摄像头 #" + std::to_wstring(i) });
                cap.release();
            }
        }
        return devices;
    }
    IEnumMoniker* pEnum = nullptr;
    hr = pDevEnum->CreateClassEnumerator(CLSID_VideoInputDeviceCategory, &pEnum, 0);
    if (hr != S_OK || !pEnum) {
        pDevEnum->Release();
        for (int i = 0; i <= 20; i++) {
            cv::VideoCapture cap(i, cv::CAP_DSHOW);
            if (cap.isOpened()) {
                devices.push_back({ i, L"摄像头 #" + std::to_wstring(i) });
                cap.release();
            }
        }
        return devices;
    }
    IMoniker* pMoniker = nullptr;
    ULONG cFetched;
    int index = 0;
    while (pEnum->Next(1, &pMoniker, &cFetched) == S_OK) {
        IPropertyBag* pPropBag = nullptr;
        hr = pMoniker->BindToStorage(0, 0, IID_IPropertyBag, (void**)&pPropBag);
        if (SUCCEEDED(hr)) {
            VARIANT varName;
            VariantInit(&varName);
            hr = pPropBag->Read(L"FriendlyName", &varName, 0);
            if (SUCCEEDED(hr)) {
                std::wstring deviceName = varName.bstrVal;
                devices.push_back({ index, deviceName });
                VariantClear(&varName);
            }
            pPropBag->Release();
        }
        pMoniker->Release();
        index++;
    }
    pEnum->Release();
    pDevEnum->Release();
    return devices;
}

// ================= 检测采集卡参数范围 =================
void DetectCameraParamRange(cv::VideoCapture& cap) {
    double brightness = cap.get(cv::CAP_PROP_BRIGHTNESS);
    double contrast = cap.get(cv::CAP_PROP_CONTRAST);
    double saturation = cap.get(cv::CAP_PROP_SATURATION);
    double hue = cap.get(cv::CAP_PROP_HUE);
    if (brightness > 100) {
        g_config.亮度最大值 = 255;
        if (g_config.亮度 == 50) g_config.亮度 = 128;
    }
    if (contrast > 100) {
        g_config.对比度最大值 = 255;
        if (g_config.对比度 == 50) g_config.对比度 = 128;
    }
    if (saturation > 100) {
        g_config.饱和度最大值 = 255;
        if (g_config.饱和度 == 50) g_config.饱和度 = 128;
    }
    if (hue > 100) {
        g_config.色调最大值 = 255;
        if (g_config.色调 == 50) g_config.色调 = 128;
    }
    std::cout << "[检测] 参数范围: 亮度0-" << g_config.亮度最大值
        << " 对比度0-" << g_config.对比度最大值
        << " 饱和度0-" << g_config.饱和度最大值
        << " 色调0-" << g_config.色调最大值 << "\n";
}

// ================= 应用摄像头参数设置 =================
void ApplyCameraSettings(cv::VideoCapture& cap) {
    cap.set(cv::CAP_PROP_BRIGHTNESS, g_config.亮度);
    cap.set(cv::CAP_PROP_CONTRAST, g_config.对比度);
    cap.set(cv::CAP_PROP_SATURATION, g_config.饱和度);
    cap.set(cv::CAP_PROP_HUE, g_config.色调);
    cap.set(cv::CAP_PROP_BUFFERSIZE, g_config.缓冲大小);
    switch (g_config.视频格式) {
    case 0:
        cap.set(cv::CAP_PROP_FOURCC, cv::VideoWriter::fourcc('Y', 'U', 'Y', '2'));
        break;
    case 1:
        cap.set(cv::CAP_PROP_FOURCC, cv::VideoWriter::fourcc('M', 'J', 'P', 'G'));
        break;
    case 2:
        cap.set(cv::CAP_PROP_FOURCC, cv::VideoWriter::fourcc('N', 'V', '1', '2'));
        break;
    case 3:
        cap.set(cv::CAP_PROP_FOURCC, cv::VideoWriter::fourcc('R', 'G', 'B', '2'));
        break;
    case 4:
        cap.set(cv::CAP_PROP_FOURCC, cv::VideoWriter::fourcc('I', '4', '2', '0'));
        break;
    case 5:
        cap.set(cv::CAP_PROP_FOURCC, cv::VideoWriter::fourcc('H', '2', '6', '4'));
        break;
    default:
        cap.set(cv::CAP_PROP_FOURCC, cv::VideoWriter::fourcc('M', 'J', 'P', 'G'));
        break;
    }
}

// ================= 检测摄像头能力（使用DirectShow API） =================
std::vector<CameraCapability> DetectCameraCapabilities(int cameraIndex) {
    std::vector<CameraCapability> capabilities;
    std::cout << "\n[检测] 正在查询摄像头支持的能力...\n";
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ICreateDevEnum* pDevEnum = nullptr;
    hr = CoCreateInstance(CLSID_SystemDeviceEnum, nullptr, CLSCTX_INPROC, IID_ICreateDevEnum, (void**)&pDevEnum);
    if (FAILED(hr)) {
        CoUninitialize();
        return capabilities;
    }
    IEnumMoniker* pEnum = nullptr;
    hr = pDevEnum->CreateClassEnumerator(CLSID_VideoInputDeviceCategory, &pEnum, 0);
    if (hr != S_OK || !pEnum) {
        pDevEnum->Release();
        CoUninitialize();
        return capabilities;
    }
    IMoniker* pMoniker = nullptr;
    ULONG cFetched;
    int currentIndex = 0;
    while (pEnum->Next(1, &pMoniker, &cFetched) == S_OK) {
        if (currentIndex == cameraIndex) {
            IBaseFilter* pFilter = nullptr;
            hr = pMoniker->BindToObject(nullptr, nullptr, IID_IBaseFilter, (void**)&pFilter);
            if (SUCCEEDED(hr)) {
                IEnumPins* pEnumPins = nullptr;
                hr = pFilter->EnumPins(&pEnumPins);
                if (SUCCEEDED(hr)) {
                    IPin* pPin = nullptr;
                    while (pEnumPins->Next(1, &pPin, &cFetched) == S_OK) {
                        PIN_DIRECTION dir;
                        pPin->QueryDirection(&dir);
                        if (dir == PINDIR_OUTPUT) {
                            IAMStreamConfig* pStreamConfig = nullptr;
                            hr = pPin->QueryInterface(IID_IAMStreamConfig, (void**)&pStreamConfig);
                            if (SUCCEEDED(hr)) {
                                int count = 0, size = 0;
                                hr = pStreamConfig->GetNumberOfCapabilities(&count, &size);
                                if (SUCCEEDED(hr) && count > 0) {
                                    std::cout << "  发现 " << count << " 种原始配置\n";
                                    for (int i = 0; i < count; i++) {
                                        BYTE* buffer = new BYTE[size];
                                        AM_MEDIA_TYPE* pmt = nullptr;
                                        hr = pStreamConfig->GetStreamCaps(i, &pmt, buffer);
                                        if (SUCCEEDED(hr) && pmt) {
                                            if (pmt->majortype == MEDIATYPE_Video && pmt->formattype == FORMAT_VideoInfo) {
                                                VIDEOINFOHEADER* pVih = (VIDEOINFOHEADER*)pmt->pbFormat;
                                                CameraCapability cap_info;
                                                cap_info.width = pVih->bmiHeader.biWidth;
                                                cap_info.height = pVih->bmiHeader.biHeight;
                                                cap_info.fps = 10000000 / pVih->AvgTimePerFrame;
                                                if (pmt->subtype == MEDIASUBTYPE_MJPG) {
                                                    cap_info.formatName = L"MJPG";
                                                    cap_info.fourcc = cv::VideoWriter::fourcc('M', 'J', 'P', 'G');
                                                }
                                                else if (pmt->subtype == MEDIASUBTYPE_YUY2) {
                                                    cap_info.formatName = L"YUY2";
                                                    cap_info.fourcc = cv::VideoWriter::fourcc('Y', 'U', 'Y', '2');
                                                }
                                                else if (pmt->subtype == MEDIASUBTYPE_NV12) {
                                                    cap_info.formatName = L"NV12";
                                                    cap_info.fourcc = cv::VideoWriter::fourcc('N', 'V', '1', '2');
                                                }
                                                else if (pmt->subtype == MEDIASUBTYPE_RGB24) {
                                                    cap_info.formatName = L"RGB24";
                                                    cap_info.fourcc = cv::VideoWriter::fourcc('R', 'G', 'B', '2');
                                                }
                                                else if (pmt->subtype == MEDIASUBTYPE_I420) {
                                                    cap_info.formatName = L"I420";
                                                    cap_info.fourcc = cv::VideoWriter::fourcc('I', '4', '2', '0');
                                                }
                                                else {
                                                    DWORD fourcc = pmt->subtype.Data1;
                                                    char fourcc_str[5];
                                                    fourcc_str[0] = (fourcc >> 0) & 0xFF;
                                                    fourcc_str[1] = (fourcc >> 8) & 0xFF;
                                                    fourcc_str[2] = (fourcc >> 16) & 0xFF;
                                                    fourcc_str[3] = (fourcc >> 24) & 0xFF;
                                                    fourcc_str[4] = '\0';
                                                    cap_info.formatName = L"其他(" + std::wstring(fourcc_str, fourcc_str + 4) + L")";
                                                    cap_info.fourcc = fourcc;
                                                }
                                                capabilities.push_back(cap_info);
                                                std::wcout << L"  [" << capabilities.size() << L"] " << cap_info.formatName << L" "
                                                    << cap_info.width << L"x" << cap_info.height
                                                    << L" @" << cap_info.fps << L"fps\n";
                                            }
                                            if (pmt->cbFormat > 0) CoTaskMemFree(pmt->pbFormat);
                                            if (pmt->pUnk) pmt->pUnk->Release();
                                            CoTaskMemFree(pmt);
                                        }
                                        delete[] buffer;
                                    }
                                }
                                pStreamConfig->Release();
                            }
                        }
                        pPin->Release();
                    }
                    pEnumPins->Release();
                }
                pFilter->Release();
            }
            break;
        }
        pMoniker->Release();
        currentIndex++;
    }
    pEnum->Release();
    pDevEnum->Release();
    CoUninitialize();
    std::cout << "\n[完成] 检测到 " << capabilities.size() << " 种配置\n";
    return capabilities;
}

// ================= 摄像头参数调节（滑条实时调节 + 非阻塞按键）=================
void AdjustCameraParams() {
    std::cout << "\n[参数调节] 打开摄像头进行实时预览调节...\n\n";
    Gdiplus::GdiplusStartupInput gdiplusStartupInput;
    ULONG_PTR gdiplusToken;
    Gdiplus::GdiplusStartup(&gdiplusToken, &gdiplusStartupInput, NULL);

    cv::VideoCapture cap(g_config.摄像头索引, cv::CAP_DSHOW);
    if (!cap.isOpened()) {
        std::cout << "[错误] 无法打开摄像头！\n\n";
        Gdiplus::GdiplusShutdown(gdiplusToken);
        return;
    }
    cap.set(cv::CAP_PROP_FRAME_WIDTH, g_config.目标宽度);
    cap.set(cv::CAP_PROP_FRAME_HEIGHT, g_config.目标高度);
    cap.set(cv::CAP_PROP_FPS, g_config.目标帧率);
    cap.set(cv::CAP_PROP_BUFFERSIZE, g_config.缓冲大小);
    DetectCameraParamRange(cap);
    ApplyCameraSettings(cap);

    // 让滑条回调能够把参数实时下发到当前设备
    g_paramCap = &cap;

    // 预览窗口：只显示画面 + HUD，不放任何控件，避免遮挡视频内容
    const std::string windowTitle = "预览 [ESC=保存退出]";
    cv::namedWindow(windowTitle, cv::WINDOW_NORMAL | cv::WINDOW_GUI_NORMAL);
    cv::resizeWindow(windowTitle, g_config.目标宽度, g_config.目标高度);

    // 控制面板窗口：独立承载四条滑条，与预览画面分离
    const std::string panelTitle = "参数调节面板";
    const int panelWidth = 520;
    // 面板窗口内，滑条会占据顶部约 100px；自绘图像只占剩余高度，避免被缩放导致文字模糊
    const int panelContentHeight = 160;
    const int panelHeight = 300;
    cv::namedWindow(panelTitle, cv::WINDOW_NORMAL | cv::WINDOW_GUI_NORMAL);
    cv::resizeWindow(panelTitle, panelWidth, panelHeight);

    // 四个色彩参数滑条：绑定到面板窗口，拖动即触发回调、硬件级实时生效
    cv::createTrackbar("亮度", panelTitle, &g_config.亮度, g_config.亮度最大值, OnColorParamChanged);
    cv::createTrackbar("对比度", panelTitle, &g_config.对比度, g_config.对比度最大值, OnColorParamChanged);
    cv::createTrackbar("饱和度", panelTitle, &g_config.饱和度, g_config.饱和度最大值, OnColorParamChanged);
    cv::createTrackbar("色调", panelTitle, &g_config.色调, g_config.色调最大值, OnColorParamChanged);

    // 分辨率预设：先用常见档位，按 A 键检测后再用真实探测结果覆盖
    std::vector<cv::Size> resPresets = {
        cv::Size(1920, 1080), cv::Size(1600, 900),
        cv::Size(1280, 720),  cv::Size(1024, 768)
    };
    // 把当前分辨率也并入预设，否则按 R 会丢失当前值
    if (std::find(resPresets.begin(), resPresets.end(),
        cv::Size(g_config.目标宽度, g_config.目标高度)) == resPresets.end()) {
        resPresets.push_back(cv::Size(g_config.目标宽度, g_config.目标高度));
    }
    // resPresets 当前对应的视频格式编号；-1 表示尚未按格式探测（分辨率列表需跟随格式）
    int resPresetsFormat = -1;

    // 摄像头实际支持的视频格式编号（首次按 F 时惰性探测并缓存，避免切到不支持的格式导致黑屏）
    std::vector<int> supportedFormats;

    // 把能力探测返回的格式名映射为内部编号（与 ApplyCameraSettings 的 switch 保持一致）
    auto FormatNameToIndex = [](const std::wstring& name) -> int {
        if (name == L"YUY2")  return 0;
        if (name == L"MJPG")  return 1;
        if (name == L"NV12")  return 2;
        if (name == L"RGB24") return 3;
        if (name == L"I420")  return 4;
        if (name == L"H264")  return 5;
        return -1;
        };
    // 从能力列表提取「去重 + 升序」的格式编号
    auto CollectSupportedFormats = [&](const std::vector<CameraCapability>& caps) {
        supportedFormats.clear();
        for (const auto& c : caps) {
            int fmt = FormatNameToIndex(c.formatName);
            if (fmt >= 0 && std::find(supportedFormats.begin(), supportedFormats.end(), fmt) == supportedFormats.end())
                supportedFormats.push_back(fmt);
        }
        std::sort(supportedFormats.begin(), supportedFormats.end());
        };

    // 内部格式编号 -> 能力探测使用的格式名（与 FormatNameToIndex 互逆）
    auto FormatIndexToName = [](int idx) -> std::wstring {
        switch (idx) {
        case 0: return L"YUY2";
        case 1: return L"MJPG";
        case 2: return L"NV12";
        case 3: return L"RGB24";
        case 4: return L"I420";
        case 5: return L"H264";
        default: return L"";
        }
        };
    // 按「当前视频格式」从能力列表重建分辨率预设（不同格式支持的分辨率不同）
    auto CollectResolutionsForFormat = [&](const std::vector<CameraCapability>& caps) {
        if (caps.empty()) return;                       // 探测失败：保留现有列表，不清空
        std::wstring want = FormatIndexToName(g_config.视频格式);
        std::vector<cv::Size> out;
        for (const auto& c : caps) {
            if (!want.empty() && c.formatName != want) continue;   // 只要当前格式的能力
            cv::Size s(c.width, c.height);
            if (std::find(out.begin(), out.end(), s) == out.end()) out.push_back(s);
        }
        if (out.empty()) {                              // 该格式没探到（或格式名未知）-> 退回全部并去重
            for (const auto& c : caps) {
                cv::Size s(c.width, c.height);
                if (std::find(out.begin(), out.end(), s) == out.end()) out.push_back(s);
            }
        }
        // 保证当前分辨率也在列表内，避免切换时列表里找不到当前位置
        cv::Size cur(g_config.目标宽度, g_config.目标高度);
        if (std::find(out.begin(), out.end(), cur) == out.end()) out.push_back(cur);
        resPresets = out;
        };

    // 切换视频格式：FOURCC 往往需要重新打开设备才能真正生效，因此统一走“重开+验证”路径
    auto TrySwitchFormat = [&](int newFormat) -> bool {
        int oldFormat = g_config.视频格式;
        g_config.视频格式 = newFormat;
        cap.release();
        cap.open(g_config.摄像头索引, cv::CAP_DSHOW);
        if (!cap.isOpened()) {
            // 新格式打不开设备：回滚到旧格式并重新打开
            g_config.视频格式 = oldFormat;
            cap.open(g_config.摄像头索引, cv::CAP_DSHOW);
            if (cap.isOpened()) ApplyCameraSettings(cap);
            return false;
        }
        cap.set(cv::CAP_PROP_FRAME_WIDTH, g_config.目标宽度);
        cap.set(cv::CAP_PROP_FRAME_HEIGHT, g_config.目标高度);
        cap.set(cv::CAP_PROP_FPS, g_config.目标帧率);
        ApplyCameraSettings(cap);
        cv::Mat testFrame;
        if (cap.read(testFrame) && !testFrame.empty()) return true;
        // 新格式无画面：完整回滚（格式/宽高/帧率/缓冲/色彩），避免设置残留导致预览异常
        g_config.视频格式 = oldFormat;
        cap.release();
        cap.open(g_config.摄像头索引, cv::CAP_DSHOW);
        if (cap.isOpened()) {
            cap.set(cv::CAP_PROP_FRAME_WIDTH, g_config.目标宽度);
            cap.set(cv::CAP_PROP_FRAME_HEIGHT, g_config.目标高度);
            cap.set(cv::CAP_PROP_FPS, g_config.目标帧率);
            cap.set(cv::CAP_PROP_BUFFERSIZE, g_config.缓冲大小);
            ApplyCameraSettings(cap);
        }
        return false;
    };

    cv::Mat frame;
    bool settingsChanged = false;
    std::wstring hudMsg;                                   // 最近一次操作的反馈，短暂显示在画面上
    auto hudMsgTime = std::chrono::high_resolution_clock::now();
    std::deque<std::chrono::high_resolution_clock::time_point> frameTimestamps;
    double displayFps = 0.0;
    double actualCameraFps = cap.get(cv::CAP_PROP_FPS);

    // 方向键改用 GetAsyncKeyState 直读：waitKey 对方向键的扩展键码依赖后端，
    // 且滑条（Windows 子控件）持有焦点时会吞掉方向键，导致 waitKey 收不到。
    // 注意 GetAsyncKeyState 是电平检测，需配合下面的 prevXxx 做边沿检测，否则长按会连续触发。
    bool prevUp = false, prevDown = false, prevLeft = false, prevRight = false;

    // 设置画面上短暂显示的反馈消息（供各按键分支与方向切换 lambda 共用）
    auto setHud = [&](const std::wstring& msg) {
        hudMsg = msg;
        hudMsgTime = std::chrono::high_resolution_clock::now();
        };

    // 按方向切换视频格式：dir=+1 下一个，dir=-1 上一个；首次调用会惰性探测支持的格式
    auto SwitchFormatByDir = [&](int dir) {
        if (supportedFormats.empty()) {
            setHud(L"正在探测支持的视频格式...");
            CollectSupportedFormats(DetectCameraCapabilities(g_config.摄像头索引));
            // 探测失败则退回全部格式，保证功能可用
            if (supportedFormats.empty()) supportedFormats = { 0, 1, 2, 3, 4, 5 };
        }
        size_t n = supportedFormats.size();
        auto it = std::find(supportedFormats.begin(), supportedFormats.end(), g_config.视频格式);
        size_t cur = (it == supportedFormats.end()) ? 0 : (size_t)(it - supportedFormats.begin());
        // +n 再取模，避免反向时下标为负
        size_t idx = (dir > 0) ? (cur + 1) % n : (cur + n - 1) % n;
        int next = supportedFormats[idx];
        if (TrySwitchFormat(next)) {
            settingsChanged = true;
            setHud(L"格式已切换: " + GetFormatNameW(g_config.视频格式));
            resPresetsFormat = -1;   // 格式已变，分辨率列表需按新格式重新探测
        }
        else {
            setHud(L"切换失败，已回滚到: " + GetFormatNameW(g_config.视频格式));
        }
        frameTimestamps.clear();
        };

    // 按方向切换分辨率：dir=+1 下一个，dir=-1 上一个；列表跟随当前视频格式
    auto SwitchResolutionByDir = [&](int dir) {
        if (resPresetsFormat != g_config.视频格式) {
            setHud(L"正在探测支持的分辨率...");
            CollectResolutionsForFormat(DetectCameraCapabilities(g_config.摄像头索引));
            resPresetsFormat = g_config.视频格式;
        }
        size_t n = resPresets.size();
        if (n == 0) return;
        auto it = std::find(resPresets.begin(), resPresets.end(),
            cv::Size(g_config.目标宽度, g_config.目标高度));
        size_t cur = (it == resPresets.end()) ? 0 : (size_t)(it - resPresets.begin());
        size_t idx = (dir > 0) ? (cur + 1) % n : (cur + n - 1) % n;
        cv::Size target = resPresets[idx];
        cap.set(cv::CAP_PROP_FRAME_WIDTH, target.width);
        cap.set(cv::CAP_PROP_FRAME_HEIGHT, target.height);
        cv::resizeWindow(windowTitle, target.width, target.height);
        cv::Mat testFrame;
        if (cap.read(testFrame) && !testFrame.empty()) {
            g_config.目标宽度 = target.width;
            g_config.目标高度 = target.height;
            settingsChanged = true;
            setHud(L"分辨率: " + std::to_wstring(target.width) + L"x" + std::to_wstring(target.height));
        }
        else {
            setHud(L"该分辨率无画面，请尝试其他档位");
        }
        frameTimestamps.clear();
        };

    std::cout << "\n按键说明（无需输入数值，全部单键操作）:\n";
    std::cout << "  拖动「参数调节面板」上的滑条: 实时调节 亮度/对比度/饱和度/色调\n";
    std::cout << "  ↑/↓=切换视频格式  ←/→=切换分辨率（双向）\n";
    std::cout << "  +/-=调整帧率  [=缓冲-1  ]=缓冲+1\n";
    std::cout << "  0=恢复默认  A=自动检测能力  ESC/S=保存退出\n\n";

    while (true) {
        if (!cap.read(frame) || frame.empty()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));   // 读帧失败时避免忙等空转
            continue;
        }
        // 统计平滑后的实时帧率
        auto now = std::chrono::high_resolution_clock::now();
        frameTimestamps.push_back(now);
        auto oneSecondAgo = now - std::chrono::seconds(1);
        while (!frameTimestamps.empty() && frameTimestamps.front() < oneSecondAgo) {
            frameTimestamps.pop_front();
        }
        double currentFps = (double)frameTimestamps.size();
        displayFps = (displayFps == 0.0) ? currentFps : displayFps * 0.8 + currentFps * 0.2;

        // HUD：统一深色半透明底板 + 统一字号，保证在任何画面上都清晰（配色分级：正常=绿，偏低=黄，异常=红）
        bool fpsLow = displayFps < g_config.目标帧率 * 0.9;
        bool fpsBad = displayFps < g_config.目标帧率 * 0.5;
        cv::Scalar fpsColor = fpsBad ? cv::Scalar(80, 80, 255)
            : (fpsLow ? cv::Scalar(0, 220, 255) : cv::Scalar(120, 255, 120));
        std::wstring line1 = L"格式 " + GetFormatNameW(g_config.视频格式) +
            L"    分辨率 " + std::to_wstring(frame.cols) + L"x" + std::to_wstring(frame.rows);
        std::wstring line2 = L"帧率 " + std::to_wstring((int)displayFps) + L" / " + std::to_wstring(g_config.目标帧率) +
            L"    FPS    缓冲 " + std::to_wstring(g_config.缓冲大小) + L" 帧";
        // 底板覆盖两行文字区域，提升可读性
        drawHudBackground(frame, cv::Rect(6, 6, 480, 62));
        putChineseText(frame, line1, cv::Point(14, 12), 18, cv::Scalar(255, 255, 255), 1);
        putChineseText(frame, line2, cv::Point(14, 40), 18, fpsColor, 1);
        if (fpsLow) {
            drawHudBackground(frame, cv::Rect(6, 72, 460, 30));
            putChineseText(frame, L"帧率偏低，请检查 USB 带宽 / CPU / 分辨率",
                cv::Point(14, 78), 15, cv::Scalar(0, 220, 255), 1);
        }
        // 操作反馈仅在最近 2 秒内显示（hudMsg 本身已是宽字符，直接绘制，无需再转 UTF-8）
        if (!hudMsg.empty() &&
            std::chrono::duration_cast<std::chrono::seconds>(now - hudMsgTime).count() < 2) {
            cv::Rect msgRect(6, frame.rows - 44, 460, 34);
            drawHudBackground(frame, msgRect, 0.62);
            putChineseText(frame, hudMsg, cv::Point(14, frame.rows - 38), 17,
                cv::Scalar(120, 230, 255), 1);
        }

        // 自绘面板底板：滑条下方显示各参数数值与当前配置，弥补原生滑条无文字信息的缺点
        cv::Mat panel(panelContentHeight, panelWidth, CV_8UC3, cv::Scalar(28, 28, 30));
        putChineseText(panel, L"色彩参数（拖动上方滑条实时生效）",
            cv::Point(12, 6), 16, cv::Scalar(180, 180, 180), 1);
        std::wstring paramLine = L"亮度 " + std::to_wstring(g_config.亮度) + L" / " + std::to_wstring(g_config.亮度最大值) +
            L"    对比度 " + std::to_wstring(g_config.对比度) + L" / " + std::to_wstring(g_config.对比度最大值);
        std::wstring paramLine2 = L"饱和度 " + std::to_wstring(g_config.饱和度) + L" / " + std::to_wstring(g_config.饱和度最大值) +
            L"    色调 " + std::to_wstring(g_config.色调) + L" / " + std::to_wstring(g_config.色调最大值);
        std::wstring stateLine = L"格式 " + GetFormatNameW(g_config.视频格式) +
            L"   分辨率 " + std::to_wstring(g_config.目标宽度) + L"x" + std::to_wstring(g_config.目标高度) +
            L"   帧率 " + std::to_wstring(g_config.目标帧率) +
            L"   缓冲 " + std::to_wstring(g_config.缓冲大小);
        putChineseText(panel, paramLine, cv::Point(12, 34), 15, cv::Scalar(255, 255, 255), 1);
        putChineseText(panel, paramLine2, cv::Point(12, 58), 15, cv::Scalar(255, 255, 255), 1);
        putChineseText(panel, stateLine, cv::Point(12, 88), 14, cv::Scalar(120, 220, 255), 1);
        putChineseText(panel, L"↑↓=格式  ←→=分辨率  +/-=帧率  [ ]=缓冲  0=默认  ESC=退出",
            cv::Point(12, 116), 13, cv::Scalar(150, 150, 150), 1);
        cv::imshow(panelTitle, panel);

        cv::imshow(windowTitle, frame);
        int key = cv::waitKey(1);

        // 方向键：直读物理按键状态并做边沿检测（仅在“刚按下”的那一帧触发一次）
        bool nowUp = (GetAsyncKeyState(VK_UP) & 0x8000) != 0;
        bool nowDown = (GetAsyncKeyState(VK_DOWN) & 0x8000) != 0;
        bool nowLeft = (GetAsyncKeyState(VK_LEFT) & 0x8000) != 0;
        bool nowRight = (GetAsyncKeyState(VK_RIGHT) & 0x8000) != 0;
        if (nowUp && !prevUp)            SwitchFormatByDir(-1);      // ↑ 上一个视频格式
        else if (nowDown && !prevDown)   SwitchFormatByDir(+1);      // ↓ 下一个视频格式
        else if (nowLeft && !prevLeft)   SwitchResolutionByDir(-1);  // ← 上一个分辨率
        else if (nowRight && !prevRight) SwitchResolutionByDir(+1);  // → 下一个分辨率
        // 每帧同步一次状态，供下一帧判断“刚按下”
        prevUp = nowUp; prevDown = nowDown; prevLeft = nowLeft; prevRight = nowRight;

        if (key == 27 || key == 's' || key == 'S') {
            break;                                        // 保存并退出
        }
        else if (key == '+' || key == '=' || key == '-') {
            if (key == '-') g_config.目标帧率 = (g_config.目标帧率 > 6) ? g_config.目标帧率 - 5 : 1;
            else            g_config.目标帧率 = (g_config.目标帧率 < 236) ? g_config.目标帧率 + 5 : 240;
            cap.set(cv::CAP_PROP_FPS, g_config.目标帧率);
            actualCameraFps = cap.get(cv::CAP_PROP_FPS);
            settingsChanged = true;
            setHud(L"目标帧率: " + std::to_wstring(g_config.目标帧率) +
                L" (实际 " + std::to_wstring((int)actualCameraFps) + L")");
            frameTimestamps.clear();
        }
        else if (key == '[' || key == ']') {
            if (key == '[') g_config.缓冲大小 = (g_config.缓冲大小 > 1) ? g_config.缓冲大小 - 1 : 1;
            else            g_config.缓冲大小 = (g_config.缓冲大小 < 10) ? g_config.缓冲大小 + 1 : 10;
            cap.set(cv::CAP_PROP_BUFFERSIZE, g_config.缓冲大小);
            settingsChanged = true;
            setHud(L"缓冲大小: " + std::to_wstring(g_config.缓冲大小));
        }
        else if (key == '0') {
            // 恢复默认：参数范围是 0-255 的采集卡取中点 128，否则取 50
            g_config.亮度 = (g_config.亮度最大值 == 255) ? 128 : 50;
            g_config.对比度 = (g_config.对比度最大值 == 255) ? 128 : 50;
            g_config.饱和度 = (g_config.饱和度最大值 == 255) ? 128 : 50;
            g_config.色调 = (g_config.色调最大值 == 255) ? 128 : 50;
            g_config.视频格式 = 0;
            g_config.目标宽度 = 1920;
            g_config.目标高度 = 1080;
            g_config.目标帧率 = 30;
            g_config.缓冲大小 = 1;
            // 同步滑条位置，否则界面显示会与实际值脱节
            cv::setTrackbarPos("亮度", panelTitle, g_config.亮度);
            cv::setTrackbarPos("对比度", panelTitle, g_config.对比度);
            cv::setTrackbarPos("饱和度", panelTitle, g_config.饱和度);
            cv::setTrackbarPos("色调", panelTitle, g_config.色调);
            ApplyCameraSettings(cap);
            cap.set(cv::CAP_PROP_FRAME_WIDTH, 1920);
            cap.set(cv::CAP_PROP_FRAME_HEIGHT, 1080);
            cap.set(cv::CAP_PROP_FPS, 30);
            cv::resizeWindow(windowTitle, 1920, 1080);
            actualCameraFps = cap.get(cv::CAP_PROP_FPS);
            settingsChanged = true;
            setHud(L"已恢复默认设置");
            frameTimestamps.clear();
        }
        else if (key == 'a' || key == 'A') {
            std::cout.flush();                            // 避免与 DetectCameraCapabilities 内部的 wcout 输出交错
            auto capabilities = DetectCameraCapabilities(g_config.摄像头索引);
            if (capabilities.empty()) {
                setHud(L"未检测到支持的能力");
            }
            else {
                std::cout << "\n[检测] 摄像头支持的能力（共 " << capabilities.size() << " 项）:\n";
                std::vector<cv::Size> detected;
                std::string lastFormat;
                for (size_t i = 0; i < capabilities.size(); i++) {
                    std::string fmt = WStrToUTF8(capabilities[i].formatName);
                    if (fmt != lastFormat) {
                        lastFormat = fmt;
                        std::cout << "\n[" << fmt << "]\n";
                    }
                    std::cout << "  " << capabilities[i].width << "x" << capabilities[i].height
                        << " @" << capabilities[i].fps << "fps\n";
                    cv::Size s(capabilities[i].width, capabilities[i].height);
                    if (std::find(detected.begin(), detected.end(), s) == detected.end()) {
                        detected.push_back(s);
                    }
                }
                std::cout << std::endl;
                if (!detected.empty()) {
                    // 按当前格式重建分辨率列表，之后按 R 只在这几种分辨率间切换
                    CollectResolutionsForFormat(capabilities);
                    resPresetsFormat = g_config.视频格式;
                }
                // 顺手刷新支持的格式列表，供 F 键使用
                CollectSupportedFormats(capabilities);
                setHud(L"已检测到 " + std::to_wstring(detected.size()) + L" 种分辨率，按 R 切换");
            }
            frameTimestamps.clear();
        }
        try {
            if (cv::getWindowProperty(windowTitle, cv::WND_PROP_VISIBLE) < 1) break;
        }
        catch (...) { break; }
    }

    g_paramCap = nullptr;
    try {
        if (cv::getWindowProperty(windowTitle, cv::WND_PROP_VISIBLE) >= 0) {
            cv::destroyWindow(windowTitle);
        }
    }
    catch (...) {}
    try {
        if (cv::getWindowProperty(panelTitle, cv::WND_PROP_VISIBLE) >= 0) {
            cv::destroyWindow(panelTitle);
        }
    }
    catch (...) {}
    cap.release();
    Gdiplus::GdiplusShutdown(gdiplusToken);

    if (settingsChanged) {
        SaveConfig();
        std::cout << "\n[成功] 摄像头参数已保存。\n";
        std::cout << "========================================\n";
        std::cout << "当前参数设置:\n";
        std::cout << "  亮度:   " << g_config.亮度 << "/" << g_config.亮度最大值 << "\n";
        std::cout << "  对比度: " << g_config.对比度 << "/" << g_config.对比度最大值 << "\n";
        std::cout << "  饱和度: " << g_config.饱和度 << "/" << g_config.饱和度最大值 << "\n";
        std::cout << "  色调:   " << g_config.色调 << "/" << g_config.色调最大值 << "\n";
        std::cout << "  视频格式: " << GetFormatName(g_config.视频格式) << "\n";
        std::cout << "  分辨率: " << g_config.目标宽度 << "x" << g_config.目标高度 << "\n";
        std::cout << "  帧率:   " << g_config.目标帧率 << " FPS\n";
        std::cout << "  缓冲:   " << g_config.缓冲大小 << " 帧\n";
        std::cout << "========================================\n\n";
    }
    else {
        std::cout << "\n[提示] 未修改任何参数。\n\n";
    }
}

// ================= 手动选择视频源设备 =================
void ManualSelectCamera() {
    std::cout << "\n[设备枚举] 正在通过DirectShow枚举所有可用视频源设备...\n\n";
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    auto devices = EnumerateCameraDevices();
    if (devices.empty()) {
        std::cout << "[失败] 未检测到任何可用的视频源设备！\n\n";
        CoUninitialize();
        return;
    }
    std::cout << "发现以下视频源设备:\n";
    std::cout << "========================================\n";
    for (size_t i = 0; i < devices.size(); i++) {
        std::wcout << L"  [" << i + 1 << L"] 索引 " << devices[i].first << L": " << devices[i].second << L"\n";
    }
    std::cout << "========================================\n";
    std::cout << "当前选中的设备索引: " << g_config.摄像头索引 << "\n\n";
    std::cout << "请选择要使用的设备编号 (1-" << devices.size() << ", 输入0取消): ";
    int choice;
    std::string input;
    std::getline(std::cin, input);
    try {
        choice = std::stoi(input);
    }
    catch (...) {
        std::cout << "[错误] 输入无效，操作取消。\n\n";
        CoUninitialize();
        return;
    }
    if (choice == 0) {
        std::cout << "[提示] 操作已取消，保持当前设备设置。\n\n";
        CoUninitialize();
        return;
    }
    if (choice < 1 || choice >(int)devices.size()) {
        std::cout << "[错误] 选择的编号超出范围，操作取消。\n\n";
        CoUninitialize();
        return;
    }
    int selectedIndex = devices[choice - 1].first;
    std::wcout << L"\n[测试] 正在测试设备: " << devices[choice - 1].second << L" (索引 " << selectedIndex << L")...\n";
    cv::VideoCapture cap(selectedIndex, cv::CAP_DSHOW);
    if (!cap.isOpened()) {
        std::cout << "[错误] 无法打开所选设备！\n\n";
        CoUninitialize();
        return;
    }
    cap.set(cv::CAP_PROP_FRAME_WIDTH, g_config.目标宽度);
    cap.set(cv::CAP_PROP_FRAME_HEIGHT, g_config.目标高度);
    cap.set(cv::CAP_PROP_BUFFERSIZE, g_config.缓冲大小);
    cv::Mat frame;
    bool gotFrame = false;
    for (int retry = 0; retry < 10; retry++) {
        cap >> frame;
        if (!frame.empty()) {
            gotFrame = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (!gotFrame) {
        std::cout << "[错误] 设备已打开但无法获取画面！\n\n";
        cap.release();
        CoUninitialize();
        return;
    }
    std::cout << "   -> 输出分辨率: " << frame.cols << "x" << frame.rows << "\n";
    if (frame.cols == g_config.目标宽度 && frame.rows == g_config.目标高度) {
        std::cout << "   -> [匹配] 分辨率符合" << g_config.目标宽度 << "x" << g_config.目标高度 << "要求\n";
    }
    else {
        std::cout << "   -> [警告] 分辨率不是" << g_config.目标宽度 << "x" << g_config.目标高度 << "，可能影响识别\n";
    }
    cap.release();
    g_config.摄像头索引 = selectedIndex;
    SaveConfig();
    std::wcout << L"\n[成功] 已选择设备: " << devices[choice - 1].second << L" (索引 " << selectedIndex << L")\n";
    std::cout << "[完成] 设备配置已保存。\n\n";
    CoUninitialize();
}

// ================= MMCSS 状态开关 =================
void ToggleMMCSS() {
    g_config.启用MMCSS = !g_config.启用MMCSS;
    SaveConfig();
    std::cout << "\n[MMCSS] 多媒体调度已" << (g_config.启用MMCSS ? "启用" : "禁用") << "，将在下次启动监控时生效。\n\n";
}

// ================= 通用画面预览函数（带刷新功能） =================
cv::Mat PreviewFrameWithRefresh(const std::string& windowTitle) {
    cv::VideoCapture cap(g_config.摄像头索引, cv::CAP_DSHOW);
    if (!cap.isOpened()) {
        std::cout << "\n[错误] 无法打开摄像源 (索引: " << g_config.摄像头索引 << ")\n" << std::endl;
        return cv::Mat();
    }
    cap.set(cv::CAP_PROP_FRAME_WIDTH, g_config.目标宽度);
    cap.set(cv::CAP_PROP_FRAME_HEIGHT, g_config.目标高度);
    cap.set(cv::CAP_PROP_BUFFERSIZE, g_config.缓冲大小);
    cap.set(cv::CAP_PROP_FPS, g_config.目标帧率);
    ApplyCameraSettings(cap);
    cv::Mat frame;
    cv::Mat lastValidFrame;
    bool hasValidFrame = false;
    cap >> frame;
    if (!frame.empty()) {
        lastValidFrame = frame.clone();
        hasValidFrame = true;
    }
    std::string fullTitle = windowTitle + " [0=刷新 | Space/Enter=确认 | ESC=取消]";
    cv::namedWindow(fullTitle, cv::WINDOW_NORMAL);
    cv::resizeWindow(fullTitle, g_config.目标宽度, g_config.目标高度);
    cv::Mat currentFrame;
    if (hasValidFrame) {
        currentFrame = lastValidFrame.clone();
    }
    else {
        currentFrame = cv::Mat::zeros(g_config.目标高度, g_config.目标宽度, CV_8UC3);
        cv::putText(currentFrame, "No Signal - Press R to refresh", cv::Point(g_config.目标宽度 / 2 - 200, g_config.目标高度 / 2), cv::FONT_HERSHEY_SIMPLEX, 1.5, cv::Scalar(0, 0, 255), 3);
    }
    std::cout << "\n[操作说明]\n";
    std::cout << "  - 按 0 键: 刷新画面\n";
    std::cout << "  - 按 空格/回车: 确认当前画面\n";
    std::cout << "  - 按 ESC 键: 取消操作\n";
    std::cout << "  - 鼠标拖拽: 框选区域\n\n";
    while (true) {
        cv::imshow(fullTitle, currentFrame);
        int key = cv::waitKey(100);
        if (key == '0') {
            std::cout << "[刷新] 正在重新获取画面...\n";
            cap >> frame;
            if (!frame.empty()) {
                currentFrame = frame.clone();
                lastValidFrame = frame.clone();
                hasValidFrame = true;
                std::cout << "[刷新] 获取到新画面\n";
            }
            else {
                std::cout << "[刷新] 仍未获取到有效画面，请检查视频源\n";
            }
        }
        else if (key == 32 || key == 13) {
            if (hasValidFrame) {
                cv::destroyWindow(fullTitle);
                cap.release();
                return lastValidFrame.clone();
            }
            else {
                std::cout << "[提示] 当前无有效画面，请先按 R 刷新\n";
            }
        }
        else if (key == 27) {
            cv::destroyWindow(fullTitle);
            cap.release();
            return cv::Mat();
        }
        try {
            if (cv::getWindowProperty(fullTitle, cv::WND_PROP_VISIBLE) < 1) {
                cap.release();
                return cv::Mat();
            }
        }
        catch (...) {
            cap.release();
            return cv::Mat();
        }
    }
}

// ================= 样本采集 =================
void CreateSample() {
    std::cout << "\n[样本采集] 正在打开摄像源...\n";
    cv::Mat frame = PreviewFrameWithRefresh("请框选目标基准区域");
    if (frame.empty()) {
        std::cout << "\n[提示] 操作已取消。\n" << std::endl;
        return;
    }
    cv::namedWindow("框选区域", cv::WINDOW_NORMAL);
    cv::resizeWindow("框选区域", g_config.目标宽度, g_config.目标高度);
    cv::Rect roi = cv::selectROI("框选区域", frame, false, false);
    cv::destroyWindow("框选区域");
    if (roi.width <= 0) {
        std::cout << "\n[提示] 区域无效，操作已取消。\n" << std::endl;
        return;
    }
    g_config.x = roi.x; g_config.y = roi.y; g_config.w = roi.width; g_config.h = roi.height;
    WriteImage(GetAppDir() + L"\\" + SAMPLE_IMAGE, frame(roi));
    SaveConfig();
    std::cout << "\n[成功] 基准样本已截取并更新保存。\n" << std::endl;
}

// ================= 金币判定区设置 =================
void SelectCoinArea() {
    std::cout << "\n[金币判定区] 正在打开摄像源...\n";
    cv::Mat frame = PreviewFrameWithRefresh("请框选金币判定区 (防重复保存专用)");
    if (frame.empty()) {
        std::cout << "\n[提示] 操作已取消。\n" << std::endl;
        return;
    }
    cv::namedWindow("框选金币区", cv::WINDOW_NORMAL);
    cv::resizeWindow("框选金币区", g_config.目标宽度, g_config.目标高度);
    cv::Rect roi = cv::selectROI("框选金币区", frame, false, false);
    cv::destroyWindow("框选金币区");
    if (roi.width <= 0) {
        std::cout << "\n[提示] 未框选，已清除金币判定区设置，恢复默认(将使用全图判断防重复)。\n" << std::endl;
        g_config.coin_x = 0; g_config.coin_y = 0; g_config.coin_w = 0; g_config.coin_h = 0;
    }
    else {
        g_config.coin_x = roi.x; g_config.coin_y = roi.y; g_config.coin_w = roi.width; g_config.coin_h = roi.height;
        std::cout << "\n[成功] 金币去重防并发区域已保存！\n" << std::endl;
    }
    SaveConfig();
}

// ================= 帮助信息 =================
void ShowHelp() {
    system("cls");
    std::cout << "===============================================\n";
    std::cout << "                  帮助说明                      \n";
    std::cout << "===============================================\n\n";
    std::cout << "【1】获取主目标基准样本\n";
    std::cout << "  - 打开摄像头预览画面，用鼠标框选需要识别的目标区域\n";
    std::cout << "  - 框选的区域将作为后续监控的匹配基准\n";
    std::cout << "  - 建议框选特征明显、不易变化的区域\n\n";
    std::cout << "【2】启动监控（纯后台运作）\n";
    std::cout << "  - 不显示调试窗口，只显示状态信息\n";
    std::cout << "  - 适合长时间稳定运行\n\n";
    std::cout << "【3】启动监控调试\n";
    std::cout << "  - 显示调试窗口，实时预览画面\n";
    std::cout << "  - 绿色框：主判定区域\n";
    std::cout << "  - 红色框：去重判定区（金币区）\n";
    std::cout << "  - 适合调试和验证配置\n\n";
    std::cout << "【4】选择视频源设备\n";
    std::cout << "  - 枚举系统中所有可用的视频输入设备\n";
    std::cout << "  - 包括物理摄像头和虚拟摄像头\n";
    std::cout << "  - 选择后会自动测试设备是否可用\n\n";
    std::cout << "【5】更改判定参数 (仅相似度阈值和连续判定帧数)\n";
    std::cout << "  - 相似度阈值：触发截图的最低相似度百分比（默认98%）\n";
    std::cout << "  - 连续判定帧数：连续多少帧满足条件才触发截图（默认4帧）\n";
    std::cout << "  - 颜色容差请在配置文件中修改\n\n";
    std::cout << "【6】设置金币判定区\n";
    std::cout << "  - 框选画面中金币数量显示区域\n";
    std::cout << "  - 用于防止重复截图\n";
    std::cout << "  - 不设置则使用全图判断防重复\n\n";
    std::cout << "【7】MMCSS多媒体调度\n";
    std::cout << "  - 开启后使用Windows多媒体调度优化性能\n";
    std::cout << "  - 建议保持开启\n\n";
    std::cout << "【8】摄像头参数调节\n";
    std::cout << "  - 拖动「参数调节面板」上的滑条实时调节亮度、对比度、饱和度、色调（硬件设置，零延迟）\n";
    std::cout << "  - 方向键 ↑/↓ 双向切换视频格式，←/→ 双向切换分辨率\n";
    std::cout << "  - +/- 调整帧率，[ ] 调整缓冲，0 恢复默认，A 检测能力\n";
    std::cout << "  - ESC保存退出，调节全程预览画面流畅不卡顿\n\n";
    std::cout << "===============================================\n";
    std::cout << "【监控状态说明】\n";
    std::cout << "  - [待机]：正常监控中，未触发截图\n";
    std::cout << "  - [锁定]：已触发截图，等待画面变化后解锁\n";
    std::cout << "  - 相似度：当前画面与基准样本的匹配度\n";
    std::cout << "  - 连中：连续满足条件的帧数\n";
    std::cout << "  - 帧延迟：每帧处理耗时\n";
    std::cout << "  - FPS：每秒处理的帧数\n";
    std::cout << "  - 已存：已保存的截图数量\n";
    std::cout << "  - 积压：等待处理的帧数（过大说明性能不足）\n";
    std::cout << "===============================================\n";
}

// ================= 监控主循环 (Producer) =================
void AutoCaptureLoop(bool debugMode) {
    if (!LoadConfig()) { std::cout << "\n[错误] 配置加载失败，请先获取样本！" << std::endl; return; }
    MMCSSManager mmcssMain;
    if (g_config.启用MMCSS) {
        mmcssMain.Register(L"CaptureMain", L"Capture");
    }
    cv::VideoCapture cap(g_config.摄像头索引, cv::CAP_DSHOW);
    if (!cap.isOpened()) {
        std::cout << "\n[错误] 无法打开摄像源 (索引: " << g_config.摄像头索引 << ")\n" << std::endl;
        return;
    }
    cap.set(cv::CAP_PROP_FRAME_WIDTH, g_config.目标宽度);
    cap.set(cv::CAP_PROP_FRAME_HEIGHT, g_config.目标高度);
    cap.set(cv::CAP_PROP_BUFFERSIZE, g_config.缓冲大小);
    cap.set(cv::CAP_PROP_FPS, g_config.目标帧率);
    cap.set(cv::CAP_PROP_AUTOFOCUS, 0);
    cap.set(cv::CAP_PROP_AUTO_EXPOSURE, 0);
    ApplyCameraSettings(cap);
    cv::Mat sample = ReadImage(GetAppDir() + L"\\" + SAMPLE_IMAGE);
    if (sample.empty()) { std::cout << "\n[错误] 未找到基准样本，请先获取样本。\n" << std::endl; return; }
    g_running = true;
    g_nextProduceId = 1;
    g_nextExpectedId = 1;
    g_lastSim = 0.0;
    g_isLocked = false;
    g_currentHits = 0;
    std::wstring dir = GetAppDir() + L"\\" + g_config.保存路径;
    g_nextFileIndex = GetNextImageIndex(dir);
    g_savedFileCount = g_nextFileIndex - 1;
    while (!g_workerQueue.empty()) g_workerQueue.pop();
    g_reorderBuffer.clear();
    std::vector<std::thread> pool;
    for (int i = 0; i < g_config.线程池大小; ++i) {
        pool.emplace_back([sample, i]() {
            MMCSSManager mmcssWorker;
            if (g_config.启用MMCSS) {
                std::wstring taskName = L"Worker_" + std::to_wstring(i);
                mmcssWorker.Register(taskName.c_str(), L"Capture");
            }
            while (true) {
                WorkerTask task;
                {
                    std::unique_lock<std::mutex> lock(g_workerMtx);
                    g_workerCv.wait(lock, [] { return !g_running || !g_workerQueue.empty(); });
                    if (!g_running && g_workerQueue.empty()) break;
                    task = g_workerQueue.front();
                    g_workerQueue.pop();
                }
                cv::Rect roi(g_config.x, g_config.y, g_config.w, g_config.h);
                roi &= cv::Rect(0, 0, task.frame.cols, task.frame.rows);
                cv::Mat cropped = task.frame(roi);
                double sim = CalculateSimilarityRaw(cropped, sample, g_config.颜色容差);
                {
                    std::lock_guard<std::mutex> lock(g_reorderMtx);
                    g_reorderBuffer[task.id] = { task.id, sim, task.frame };
                }
                g_reorderCv.notify_one();
            }
            });
    }
    std::thread writer(AsyncWriterThread);
    g_debugWindowQuit.store(false);
    g_debugWindowClosed.store(false);
    g_debugWindowRunning.store(false);
    std::thread displayThread;
    if (debugMode) {
        displayThread = std::thread(DebugDisplayThread);
        while (!g_debugWindowRunning.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    cv::Mat frame;
    system("cls");
    std::cout << "\n";
    std::cout << "监控运行中... [按 ESC 停止]\n";
    auto startTime = std::chrono::high_resolution_clock::now();
    auto lastLoopT = std::chrono::high_resolution_clock::now();
    std::deque<std::chrono::high_resolution_clock::time_point> frameTimestamps;
    while (true) {
        if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) {
            g_debugWindowQuit.store(true);
            break;
        }
        if (debugMode && g_debugWindowClosed.load()) {
            std::cout << "\n[提示] 调试窗口已关闭，正在停止监控...\n";
            g_debugWindowQuit.store(true);
            break;
        }
        size_t currentQueueSize = 0;
        { std::lock_guard<std::mutex> lock(g_workerMtx); currentQueueSize = g_workerQueue.size(); }
        if (currentQueueSize > 50) {
            g_running = false; g_workerCv.notify_all(); g_reorderCv.notify_all();
            g_debugWindowQuit.store(true);
            if (debugMode && displayThread.joinable()) displayThread.join();
            for (auto& th : pool) if (th.joinable()) th.join();
            if (writer.joinable()) writer.join();
            while (!g_workerQueue.empty()) g_workerQueue.pop();
            g_reorderBuffer.clear();
            std::cout << "\n\n[ERROR] 采集终止：消费侧严重积压，系统已执行安全 Fail-Fast。" << std::endl;
            exit(1);
        }
        if (!cap.read(frame) || frame.empty()) continue;
        if (debugMode && !g_debugWindowClosed.load()) {
            cv::Mat displayFrame = frame.clone();
            cv::rectangle(displayFrame, cv::Rect(g_config.x, g_config.y, g_config.w, g_config.h), cv::Scalar(0, 255, 0), 2);
            if (g_config.coin_w > 0) {
                cv::rectangle(displayFrame, cv::Rect(g_config.coin_x, g_config.coin_y, g_config.coin_w, g_config.coin_h), cv::Scalar(0, 0, 255), 2);
            }
            {
                std::lock_guard<std::mutex> lock(g_displayMtx);
                g_displayFrame = displayFrame;
            }
        }
        uint64_t currentId = g_nextProduceId++;
        { std::lock_guard<std::mutex> lock(g_workerMtx); g_workerQueue.push({ currentId, frame.clone() }); }
        g_workerCv.notify_one();
        auto now = std::chrono::high_resolution_clock::now();
        std::chrono::duration<double, std::milli> diff = now - lastLoopT;
        lastLoopT = now;
        frameTimestamps.push_back(now);
        auto oneSecondAgo = now - std::chrono::seconds(1);
        while (!frameTimestamps.empty() && frameTimestamps.front() < oneSecondAgo) {
            frameTimestamps.pop_front();
        }
        double fps = (double)frameTimestamps.size();
        auto elapsedStr = FormatDuration(std::chrono::duration_cast<std::chrono::seconds>(now - startTime));
        std::string statusMsg = g_isLocked.load() ? "[锁定]" : "[待机]";
        std::ostringstream statusStream;
        statusStream << statusMsg
            << " 运行:" << elapsedStr
            << " | 相似度:" << std::fixed << std::setprecision(1) << std::setw(5) << g_lastSim.load() << "%"
            << " | 连中:" << std::setfill(' ') << std::setw(2) << g_currentHits.load() << "/" << g_config.连续判定帧数
            << " | 帧延迟:" << std::setfill(' ') << std::setw(3) << (int)diff.count() << "ms"
            << " | FPS:" << std::setw(4) << std::setprecision(1) << fps
            << " | 已存:" << std::setw(4) << g_savedFileCount.load() << "张"
            << " | 积压:" << std::setw(2) << currentQueueSize;
        std::string statusStr = statusStream.str();
        UpdateStatusLine(statusStr, 0);
    }
    if (debugMode) {
        g_debugWindowQuit.store(true);
        if (displayThread.joinable()) displayThread.join();
    }
    std::cout << "\n\n[提示] 正在等待处理剩余队列，准备停止监控..." << std::endl;
    { std::lock_guard<std::mutex> lock(g_workerMtx); g_running = false; }
    g_workerCv.notify_all();
    for (auto& th : pool) if (th.joinable()) th.join();
    g_reorderCv.notify_all();
    if (writer.joinable()) writer.join();
    while (!g_workerQueue.empty()) g_workerQueue.pop();
    g_reorderBuffer.clear();
    std::cout << "[成功] 监控已安全停止，资源全部释放。\n" << std::endl;
}

int main() {
    SetProcessDPIAware(); setlocale(LC_ALL, "chs");
    Gdiplus::GdiplusStartupInput gdiplusStartupInput;
    ULONG_PTR gdiplusToken;
    Gdiplus::GdiplusStartup(&gdiplusToken, &gdiplusStartupInput, NULL);
    LoadConfig();
    while (true) {
        system("cls");
        std::cout << "===============================================\n";
        std::cout << "            曙光炼化自动截图                   \n";
        std::cout << "===============================================\n";
        std::cout << "  [1] 获取主目标基准样本 (带画面预览刷新)\n";
        std::cout << "  [2] 启动监控 (纯后台运作)\n";
        std::cout << "  [3] 启动监控调试 (带主目标绿框 / 去重红框)\n";
        std::cout << "  [4] 选择视频源设备 (当前: 索引 " << g_config.摄像头索引 << ")\n";
        std::cout << "  [5] 更改判定参数 (阈值:" << g_config.相似度阈值 << "%"
            << ", 连判:" << g_config.连续判定帧数 << "帧)\n";
        std::cout << "  [6] 设置金币判定区 (带画面预览刷新) (当前: "
            << (g_config.coin_w > 0 ? "已局部设置" : "未设置，默认全图") << ")\n";
        std::cout << "  [7] MMCSS多媒体调度: " << (g_config.启用MMCSS ? "已启用 [OK]" : "已禁用 [X]")
            << " (切换开关)\n";
        std::cout << "  [8] 摄像头参数调节 (分辨率:" << g_config.目标宽度 << "x" << g_config.目标高度
            << " 格式:" << GetFormatName(g_config.视频格式)
            << " 帧率:" << g_config.目标帧率
            << " 缓冲:" << g_config.缓冲大小 << ")\n";
        std::cout << "  [9] 帮助说明\n";
        std::cout << "===============================================\n";
        std::cout << "请输入操作指令: ";
        int c;
        if (!(std::cin >> c)) {
            std::cin.clear(); std::cin.ignore(10000, '\n'); continue;
        }
        std::cin.ignore(10000, '\n');
        switch (c) {
        case 1: CreateSample(); system("pause"); break;
        case 2: AutoCaptureLoop(false); system("pause"); break;
        case 3: AutoCaptureLoop(true); system("pause"); break;
        case 4: ManualSelectCamera(); system("pause"); break;
        case 5:
        {
            std::string input;
            std::cout << "输入新的相似度触发阈值 (当前 " << g_config.相似度阈值 << "%, 直接回车保持不变): ";
            std::getline(std::cin, input);
            if (!input.empty()) { try { g_config.相似度阈值 = std::stoi(input); } catch (...) {} }
            std::cout << "输入新的连续判定帧数 (当前 " << g_config.连续判定帧数 << "帧, 直接回车保持不变): ";
            std::getline(std::cin, input);
            if (!input.empty()) { try { g_config.连续判定帧数 = std::stoi(input); } catch (...) {} }
            SaveConfig();
            std::cout << "[成功] 参数更新完成。\n";
            system("pause");
            break;
        }
        case 6: SelectCoinArea(); system("pause"); break;
        case 7: ToggleMMCSS(); system("pause"); break;
        case 8: AdjustCameraParams(); system("pause"); break;
        case 9: ShowHelp(); system("pause"); break;
        default: break;
        }
    }
    Gdiplus::GdiplusShutdown(gdiplusToken);
}