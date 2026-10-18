#include "file_api.h"
#include "fallback_ui.h"
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <vector>

// ==================== 前端目录布局 ====================
//
// LittleFS 只维护一个前端根目录 /frontend，其内部结构与 URL 根路径 1:1 对应：
//
//   GET /                        -> /frontend/index.html
//   GET /assets/index-a1b2c3.js  -> /frontend/assets/index-a1b2c3.js
//   GET /favicon.ico             -> /frontend/favicon.ico
//
// 因此任意打包器（Vite / Rollup / webpack）以 base="/" 产出的整个 dist 目录
// 都能整体上传，新增文件不需要改固件。
//
// 若某文件旁边存在同名 .gz（如 /frontend/assets/index-a1b2c3.js.gz），则优先
// 下发压缩版本并附带 Content-Encoding: gzip。打包产物是纯文本，压缩后通常
// 只剩三分之一左右，对 ESP32 的 WiFi 吞吐是很实际的优化。
//
// /frontend/assets/** 视为内容寻址的不可变资源，返回长效缓存头；其余（含
// index.html）一律 no-cache，保证重新部署后立刻生效。

static const char kFrontendRoot[] = "/frontend";
static const char kIndexPath[] = "/frontend/index.html";
static const char kUploadTemp[] = "/frontend-upload.tmp";

static const size_t kMaxAssetBytes = 512 * 1024;  // 单文件上限
static const size_t kMinFreeBytes = 24 * 1024;    // 剩余空间低于此值即拒绝写入
static const size_t kMaxEntries = 160;            // 列表返回条目上限
static const uint8_t kMaxDepth = 5;               // 目录遍历深度上限
static const uint8_t kDirQueue = 12;               // 遍历队列长度上限

static AsyncWebServer* webServer = nullptr;
static bool mounted = false;
static bool uploadOk = false;
static String uploadTarget, uploadError;
static size_t uploadBytes = 0;
static File uploadFile;
// 上传状态是全局的（暂存文件只有一个），并发上传会互相覆盖。这里只允许
// 一个请求写入；被拒绝的请求用 _tempObject 打标记，在完成回调里回 409，
// 不参与全局上传状态（否则会覆盖正在上传的文件目标）。
static AsyncWebServerRequest* uploadOwner = nullptr;
// 客户端中途断连时库不会回调 final，owner 可能永远留在旧请求上。超过该
// 时长没有任何数据块即视为已放弃，允许新上传接管，避免设备卡死在"忙"。
static uint32_t uploadOwnerAt = 0;
static const uint32_t kUploadOwnerTimeoutMs = 30000;
static const uintptr_t kUploadRejectedMarker = 1;

// 上传写缓冲：上传开始时动态申请 64KB（攒批落盘，把"每个 TCP 分片一次
// flash 事务"降为"每 64KB 一次"），结束/中止即释放——不常驻占用堆，
// 避免挤压 WiFi 驱动在客户端关联阶段的运行时分配（静态常驻 64KB 曾导致
// AP 可搜到但关联不上）。申请失败回退到 8KB 静态兜底。
static const size_t kUploadBufPreferred = 65536;
static uint8_t uploadBufFallback[8192];
static uint8_t* uploadBuf = nullptr;
static size_t uploadBufSize = 0;
static size_t uploadBufLen = 0;

static void releaseUploadBuffer();

static void acquireUploadBuffer() {
  // 上一次上传若因客户端断连没有收到 final 回调，缓冲不会被释放；
  // 这里先回收，避免每次中断都泄漏 64KB 堆。
  releaseUploadBuffer();
  uploadBuf = static_cast<uint8_t*>(malloc(kUploadBufPreferred));
  if (uploadBuf) {
    uploadBufSize = kUploadBufPreferred;
  } else {
    uploadBuf = uploadBufFallback;
    uploadBufSize = sizeof(uploadBufFallback);
    Serial.println("[FS] 64KB upload buffer alloc failed; using 8KB fallback");
  }
}

static void releaseUploadBuffer() {
  if (uploadBuf && uploadBuf != uploadBufFallback) free(uploadBuf);
  uploadBuf = nullptr;
  uploadBufSize = 0;
  uploadBufLen = 0;
}

// ==================== FS 用量缓存 ====================

static uint32_t fsCacheTotal = 0, fsCacheUsed = 0, fsCacheAt = 0;
static const uint32_t kFsCacheMs = 2000;

void fsUsageRefresh(bool force) {
  if (!mounted) { fsCacheTotal = fsCacheUsed = 0; return; }
  uint32_t now = millis();
  if (!force && fsCacheAt && now - fsCacheAt < kFsCacheMs) return;
  fsCacheAt = now;
  fsCacheTotal = LittleFS.totalBytes();
  fsCacheUsed = LittleFS.usedBytes();
}

void fsUsageCached(uint32_t& totalBytes, uint32_t& usedBytes) {
  fsUsageRefresh(false);
  totalBytes = fsCacheTotal;
  usedBytes = fsCacheUsed;
}

// ==================== MIME ====================

struct MimeEntry {
  const char* extension;
  const char* type;
};
static const MimeEntry kMimeTypes[] = {
  {"html", "text/html; charset=utf-8"},
  {"htm", "text/html; charset=utf-8"},
  {"js", "text/javascript; charset=utf-8"},
  {"mjs", "text/javascript; charset=utf-8"},
  {"css", "text/css; charset=utf-8"},
  {"json", "application/json; charset=utf-8"},
  {"map", "application/json; charset=utf-8"},
  {"webmanifest", "application/manifest+json"},
  {"svg", "image/svg+xml"},
  {"png", "image/png"},
  {"jpg", "image/jpeg"},
  {"jpeg", "image/jpeg"},
  {"gif", "image/gif"},
  {"webp", "image/webp"},
  {"avif", "image/avif"},
  {"ico", "image/x-icon"},
  {"bmp", "image/bmp"},
  {"woff", "font/woff"},
  {"woff2", "font/woff2"},
  {"ttf", "font/ttf"},
  {"otf", "font/otf"},
  {"txt", "text/plain; charset=utf-8"},
  {"xml", "application/xml"},
  {"wasm", "application/wasm"},
  {"mp4", "video/mp4"},
  {"webm", "video/webm"},
  {"mp3", "audio/mpeg"},
  {"pdf", "application/pdf"},
};

static const char* mimeFor(const String& path) {
  int dot = path.lastIndexOf('.');
  if (dot < 0) return "application/octet-stream";
  String extension = path.substring(dot + 1);
  extension.toLowerCase();
  for (const MimeEntry& entry : kMimeTypes)
    if (extension == entry.extension) return entry.type;
  return "application/octet-stream";
}

static bool hasSuffix(const String& path) {
  return path.lastIndexOf('.') > path.lastIndexOf('/');
}

// ==================== 路径校验 ====================

// 归一化浏览器请求路径；任何可疑字符一律拒绝而不是静默修正。
static String requestPath(const String& uri) {
  if (uri.isEmpty() || uri[0] != '/') return "";
  if (uri.length() > 120) return "";
  if (uri.indexOf('\\') >= 0 || uri.indexOf('%') >= 0) return "";
  for (size_t i = 0; i < uri.length(); ++i) {
    unsigned char c = static_cast<unsigned char>(uri[i]);
    if (c < 0x20 || c >= 0x7F) return "";
  }
  return uri;
}

// 只接受 /frontend 及其子路径。分段校验同时覆盖目录穿越、空段与
// LittleFS 的 64 字节对象名限制（CONFIG_LITTLEFS_OBJ_NAME_LEN）。
// 段内字符限定 [A-Za-z0-9._-]：上传走 query 参数会被 URL 解码，而下载走
// 原始 URI（含 % 一律拒绝）——白名单保证"能上传的一定能下载/访问"。
static bool validSegmentChars(const String& path, int start, int end) {
  for (int i = start; i < end; ++i) {
    char c = path[i];
    if (!isalnum(static_cast<unsigned char>(c)) && c != '.' && c != '_' && c != '-')
      return false;
  }
  return true;
}

static bool writablePath(const String& path) {
  if (path.length() > 120) return false;
  if (path != kFrontendRoot && !path.startsWith("/frontend/")) return false;
  if (path.endsWith("/")) return false;
  int start = 1;
  while (start < static_cast<int>(path.length())) {
    int slash = path.indexOf('/', start);
    int end = slash < 0 ? path.length() : slash;
    int length = end - start;
    if (length < 1 || length > 64) return false;
    if (length == 1 && path[start] == '.') return false;
    if (length == 2 && path[start] == '.' && path[start + 1] == '.') return false;
    if (path != kFrontendRoot && !validSegmentChars(path, start, end)) return false;
    start = end + 1;
  }
  return true;
}

// 旧版本把前端三件套直接放在文件系统根目录。新方案只认 /frontend，
// 这里保留只读 + 可删除能力，升级后能把残留一次清掉，而不是永远占着空间。
static bool legacyAssetPath(const String& path) {
  return path == "/index.html" || path == "/app.js" || path == "/style.css";
}

// 学习到的原始脉冲仅可按固定命名读取，且永远不可写。
static bool capturePath(const String& path) {
  if (!path.startsWith("/captures/") || !path.endsWith(".json")) return false;
  String name = path.substring(10, path.length() - 5);
  if (name.isEmpty() || name.length() > 24) return false;
  for (size_t i = 0; i < name.length(); ++i) {
    char c = name[i];
    if (!isalnum(static_cast<unsigned char>(c)) && c != '-') return false;
  }
  return true;
}

static bool readablePath(const String& path) {
  return writablePath(path) || capturePath(path) || legacyAssetPath(path);
}

// 可写目标：/frontend 下的某个文件，或 /captures/ 下的采样文件。
static bool uploadPath(const String& path) {
  if (capturePath(path)) return true;
  return path.length() > strlen(kFrontendRoot) && writablePath(path);
}

static size_t freeBytes() {
  // 用缓存值：上传是连续过程，2 秒内的用量偏差不影响 kMinFreeBytes 保护判断。
  uint32_t total, used;
  fsUsageCached(total, used);
  return total - used;
}

// ==================== 目录操作 ====================

// 广度优先遍历 /frontend。任意时刻只持有一个目录句柄（子目录入队后再处理，
// 此时父目录已关闭），避免多层 File 同时打开时触达文件描述符上限。
static void appendFiles(JsonArray out, size_t& count) {
  String queue[kDirQueue];
  uint8_t level[kDirQueue] = {0};
  uint8_t head = 0, tail = 0;
  queue[tail++] = String(kFrontendRoot);
  while (head < tail && count < kMaxEntries) {
    String dir = queue[head];
    uint8_t depth = level[head];
    head++;
    File handle = LittleFS.open(dir);
    if (!handle || !handle.isDirectory()) {
      if (handle) handle.close();
      continue;
    }
    File child = handle.openNextFile();
    while (child && count < kMaxEntries) {
      String path = child.path();
      bool isDirectory = child.isDirectory();
      size_t size = isDirectory ? 0 : child.size();
      child.close();
      if (isDirectory) {
        if (depth + 1 < kMaxDepth && tail < kDirQueue) {
          queue[tail] = path;
          level[tail] = depth + 1;
          tail++;
        }
      } else {
        JsonObject item = out.add<JsonObject>();
        item["path"] = path;
        item["size"] = size;
        count++;
      }
      if (count < kMaxEntries) child = handle.openNextFile();
    }
    handle.close();
  }
}

// 递归清空目录：每层先单遍扫描收集全部子项（句柄随取随关），再统一删除。
// 旧实现每删一个文件就重新扫描一遍目录，n 个文件是 O(n²) 次 flash 目录
// 读取，几十上百个文件时单次 HTTP 请求能把 loop() 堵死几十秒。
// 目录本身用 rmdir 删除（空目录），remove 在部分版本的 LittleFS 对目录不可靠。
struct ChildEntry {
  String path;
  bool isDirectory;
};
static const size_t kMaxDirChildren = 256;  // 单层目录扫描的内存上限保护

static bool clearDirectory(const String& path, uint8_t depth) {
  File entry = LittleFS.open(path);
  if (!entry) return true;  // 已不存在，视为成功
  if (!entry.isDirectory()) {
    entry.close();
    return LittleFS.remove(path);
  }
  entry.close();
  if (depth > kMaxDepth) return false;

  std::vector<ChildEntry> children;
  File scan = LittleFS.open(path);
  if (!scan) return false;
  File child = scan.openNextFile();
  while (child && children.size() < kMaxDirChildren) {
    ChildEntry item;
    item.path = child.path();
    item.isDirectory = child.isDirectory();
    child.close();
    children.push_back(item);
    child = scan.openNextFile();
  }
  scan.close();

  for (const ChildEntry& item : children) {
    if (item.isDirectory) {
      if (!clearDirectory(item.path, depth + 1)) return false;
    } else if (!LittleFS.remove(item.path)) {
      Serial.printf("[FS] remove file failed: %s\n", item.path.c_str());
      return false;
    }
  }
  if (!LittleFS.rmdir(path) && !LittleFS.remove(path)) {
    Serial.printf("[FS] rmdir failed: %s\n", path.c_str());
    return false;
  }
  return true;
}

// LittleFS 的 mkdir 对已存在的目录返回 true，因此无需先 exists() 判断。
static bool ensureParentDirectories(const String& path) {
  int start = 1;
  while (true) {
    int slash = path.indexOf('/', start);
    if (slash < 0) return true;
    if (!LittleFS.mkdir(path.substring(0, slash))) return false;
    start = slash + 1;
  }
}

// ==================== 文件读写 API ====================

static void sendJson(AsyncWebServerRequest* request, int status, JsonDocument& doc) {
  String body;
  serializeJson(doc, body);
  request->send(status, "application/json; charset=utf-8", body);
}

static void sendError(AsyncWebServerRequest* request, int status, const char* message) {
  JsonDocument doc;
  doc["error"] = message;
  sendJson(request, status, doc);
}

static void handleList(AsyncWebServerRequest* request) {
  JsonDocument doc;
  uint32_t total, used;
  fsUsageCached(total, used);
  doc["mounted"] = true;
  doc["totalBytes"] = total;
  doc["usedBytes"] = used;
  doc["frontendRoot"] = kFrontendRoot;
  JsonArray files = doc["files"].to<JsonArray>();
  size_t count = 0;
  appendFiles(files, count);
  File captures = LittleFS.open("/captures");
  if (captures && captures.isDirectory()) {
    File file = captures.openNextFile();
    while (file && count < kMaxEntries) {
      String path = file.path();
      size_t size = file.size();
      file.close();
      if (capturePath(path)) {
        JsonObject item = files.add<JsonObject>();
        item["path"] = path;
        item["size"] = size;
        count++;
      }
      if (count < kMaxEntries) file = captures.openNextFile();
    }
    captures.close();
  } else if (captures) {
    captures.close();
  }
  for (const char* legacy : {"/index.html", "/app.js", "/style.css"}) {
    if (count >= kMaxEntries) break;
    File file = LittleFS.open(legacy);
    if (!file || file.isDirectory()) {
      if (file) file.close();
      continue;
    }
    size_t size = file.size();
    file.close();
    JsonObject item = files.add<JsonObject>();
    item["path"] = legacy;
    item["size"] = size;
    item["legacy"] = true;
    count++;
  }
  doc["truncated"] = count >= kMaxEntries;
  sendJson(request, 200, doc);
}

static void handleDownload(AsyncWebServerRequest* request, const String& path) {
  File file = LittleFS.open(path, "r");
  if (!file || file.isDirectory()) {
    if (file) file.close();
    sendError(request, 404, "File not found");
    return;
  }
  AsyncWebServerResponse* response = request->beginResponse(file, path, mimeFor(path));
  response->addHeader("Cache-Control", "no-cache");
  request->send(response);
  // 不能在这里 file.close()：AsyncFileResponse 持有 File 句柄副本，真正的
  // 读取发生在 handler 返回之后；提前关闭会让 _sourceValid() 变 false，
  // 客户端收到 500 "Invalid data in handler"。句柄随响应对象析构关闭。
}

static void handleDelete(AsyncWebServerRequest* request, const String& path) {
  if (!writablePath(path) && !legacyAssetPath(path)) {
    sendError(request, 403, "Only /frontend and legacy root assets are deletable");
    return;
  }
  if (!LittleFS.exists(path)) {
    sendError(request, 404, "Path not found");
    return;
  }
  if (!clearDirectory(path, 0)) {
    sendError(request, 500, "Delete failed; check serial log for details");
    return;
  }
  fsUsageRefresh(true);
  JsonDocument doc;
  doc["ok"] = true;
  doc["path"] = path;
  sendJson(request, 200, doc);
}

// 把暂存缓冲刷到 flash；失败时返回 false。
static bool flushUploadBuffer() {
  if (!uploadBufLen) return true;
  size_t len = uploadBufLen;
  uploadBufLen = 0;
  return uploadFile.write(uploadBuf, len) == len;
}

static void failUpload(const char* reason, size_t bytes) {
  uploadOk = false;
  uploadError = reason;
  Serial.printf("[FS] upload write error: %s (bytes=%u)\n", uploadError.c_str(), bytes);
  uploadBufLen = 0;
  if (uploadFile) uploadFile.close();
  LittleFS.remove(kUploadTemp);
  uploadOwner = nullptr;
}

// AsyncWebSocket 风格的上传回调：替代原 WebServer 的 HTTPUpload 状态机。
// onUpload 按数据块调用（final==true 表示最后一块），我们只在主文件流
// 上处理（index==0 时初始化，final 时落盘到暂存文件并改名）。
static void onUpload(AsyncWebServerRequest* request, const String& filename,
                     size_t index, uint8_t* data, size_t len, bool final) {
  // 首次块：初始化（空文件也可能直接以 index==0、len==0、final==true 到达）
  if (index == 0) {
    if (uploadOwner && uploadOwner != request &&
        millis() - uploadOwnerAt < kUploadOwnerTimeoutMs) {
      request->_tempObject = reinterpret_cast<void*>(kUploadRejectedMarker);
      Serial.println("[FS] upload rejected: another upload is in progress");
      return;
    }
    uploadOwner = request;
    uploadOwnerAt = millis();
    uploadOk = false;
    uploadBytes = 0;
    acquireUploadBuffer();
    uploadError = "";
    uploadFile = File();
    uploadTarget = request->arg("path");
    Serial.printf("[FS] upload start: %s\n", uploadTarget.c_str());
    if (!mounted) uploadError = "Filesystem not mounted";
    else if (!uploadPath(uploadTarget)) uploadError = "Only /frontend/<file> is writable";
    else if (freeBytes() < kMinFreeBytes) uploadError = "Not enough free space";
    if (!uploadError.isEmpty()) {
      Serial.printf("[FS] upload refused: %s (%s)\n", uploadError.c_str(), uploadTarget.c_str());
      releaseUploadBuffer();
      uploadOwner = nullptr;
      return;
    }
    uploadFile = LittleFS.open(kUploadTemp, "w");
    uploadOk = static_cast<bool>(uploadFile);
    if (!uploadOk) { uploadError = "Cannot open staging file"; uploadOwner = nullptr; }
  }

  // 被拒绝的并发请求不参与全局状态，否则会覆盖正在上传的文件目标。
  if (request != uploadOwner) return;
  uploadOwnerAt = millis();  // 有数据块到达即刷新占用时间
  if (!uploadOk || !uploadFile) return;

  uploadBytes += len;
  if (uploadBytes > kMaxAssetBytes) {
    failUpload("File too large", uploadBytes);
    return;
  }

  // 写入目标先落到暂存文件，全部写完再改名，掉电不会留下半个资源文件。
  // 先在 RAM 里攒满缓冲再落盘，减少 flash 写事务次数（每次写都会关
  // cache、阻塞 ISR）。
  const uint8_t* src = data;
  size_t remaining = len;
  while (remaining) {
    size_t room = uploadBufSize - uploadBufLen;
    size_t chunk = remaining < room ? remaining : room;
    memcpy(uploadBuf + uploadBufLen, src, chunk);
    uploadBufLen += chunk;
    src += chunk;
    remaining -= chunk;
    if (uploadBufLen == uploadBufSize && !flushUploadBuffer()) {
      failUpload("Write failed (filesystem full?)", uploadBytes);
      return;
    }
  }

  // 最后一块：完成上传
  if (final) {
    if (uploadFile && !flushUploadBuffer())
      failUpload("Write failed (filesystem full?)", uploadBytes);
    if (uploadFile) {
      uploadFile.flush();
      uploadFile.close();
    }
    releaseUploadBuffer();
    if (uploadOk) {
      if (!ensureParentDirectories(uploadTarget)) {
        uploadOk = false;
        uploadError = "Cannot create parent directory";
      } else if (!LittleFS.rename(kUploadTemp, uploadTarget)) {
        uploadOk = false;
        uploadError = "Rename failed";
      }
    }
    if (!uploadOk) LittleFS.remove(kUploadTemp);
    fsUsageRefresh(true);
    Serial.printf("[FS] upload end: %s (ok=%d, bytes=%u, err=%s)\n",
                  uploadTarget.c_str(), uploadOk, uploadBytes, uploadError.c_str());
    uploadOwner = nullptr;
  }
}

// ==================== 静态资源下发 ====================

static bool openAsset(const String& relative, File& out, bool& encoded) {
  String target = String(kFrontendRoot) + relative;
  String compressed = target + ".gz";
  encoded = LittleFS.exists(compressed);
  out = LittleFS.open(encoded ? compressed : target, "r");
  if (out && out.isDirectory()) {
    out.close();
    return false;
  }
  return static_cast<bool>(out);
}

static void serveFallbackLanding(AsyncWebServerRequest* request) {
  AsyncWebServerResponse* response = request->beginResponse_P(
      200, "text/html; charset=utf-8", kFallbackHtml);
  response->addHeader("Cache-Control", "no-store");
  request->send(response);
}

// 静态资源下发：/frontend 目录映射到 URL 根路径，未命中时回退入口页。
// 设备上只保留 .gz 压缩版（构建时删除原文件），因此若客户端不支持 gzip
// 则返回 406。openAsset 优先返回 .gz；若某文件未压缩（过小/收益不足）
// 则返回原文件，encoded=false。
void fileApiServeStatic(AsyncWebServerRequest* request) {
  if (!webServer) return;
  if (request->method() != HTTP_GET && request->method() != HTTP_HEAD) {
    sendError(request, 405, "Method not allowed");
    return;
  }
  String uri = request->url();
  if (uri.startsWith("/api/")) {
    sendError(request, 404, "Unknown endpoint");
    return;
  }
  String relative = requestPath(uri);
  if (relative.isEmpty()) {
    sendError(request, 400, "Bad request path");
    return;
  }
  if (relative == "/") relative = "/index.html";
  else if (relative.endsWith("/")) relative += "index.html";

  File file;
  bool encoded = false;
  bool found = mounted && openAsset(relative, file, encoded);
  if (!found && !hasSuffix(relative)) {
    // 前端路由（history 模式）回退到入口页，例如 /dashboard 刷新后仍能打开。
    relative = "/index.html";
    found = mounted && openAsset(relative, file, encoded);
  }
  if (!found) {
    if (relative == "/index.html") serveFallbackLanding(request);
    else sendError(request, 404, "Asset not found; deploy the frontend first");
    return;
  }

  // 不支持 gzip 的客户端退回原文件（设备上 .gz 与原文件同时保留）。
  bool acceptGzip = request->hasHeader("Accept-Encoding") &&
                    request->header("Accept-Encoding").indexOf("gzip") >= 0;
  if (encoded && !acceptGzip) {
    file.close();
    file = LittleFS.open(String(kFrontendRoot) + relative, "r");
    if (!file) {
      sendError(request, 404, "Asset not found; deploy the frontend first");
      return;
    }
    encoded = false;
  }

  bool immutable = relative.startsWith("/assets/");
  AsyncWebServerResponse* response = request->beginResponse(
      file, uri, mimeFor(relative));
  response->addHeader("Cache-Control", immutable
      ? "public, max-age=31536000, immutable" : "no-cache");
  response->addHeader("X-Content-Type-Options", "nosniff");
  if (encoded) response->addHeader("Content-Encoding", "gzip");
  request->send(response);
  // 同 handleDownload：响应源由 AsyncFileResponse 持有，这里提前关闭会导致
  // 所有静态资源返回 500。句柄随响应对象析构关闭。
}

// ==================== 路由注册 ====================

void fileApiBegin(AsyncWebServer& server) {
  webServer = &server;
  // begin() 在已挂载时同样返回成功；totalBytes() 作为兜底判据。
  mounted = LittleFS.begin(false) || LittleFS.totalBytes() > 0;
  fsUsageRefresh(true);

  server.on("/api/files", HTTP_GET, [](AsyncWebServerRequest* request) {
    if (!mounted) {
      sendError(request, 503, "Filesystem not initialized");
      return;
    }
    String path = request->arg("path");
    if (path.isEmpty()) {
      handleList(request);
      return;
    }
    if (!readablePath(path)) {
      sendError(request, 403, "Protected path");
      return;
    }
    handleDownload(request, path);
  });

  server.on("/api/files", HTTP_POST,
    [](AsyncWebServerRequest* request) {
      // 此回调在所有数据接收完毕后调用（onUpload 已处理落盘）。
      if (request->_tempObject == reinterpret_cast<void*>(kUploadRejectedMarker)) {
        request->_tempObject = nullptr;
        sendError(request, 409, "Another upload is in progress");
        return;
      }
      JsonDocument doc;
      doc["ok"] = uploadOk;
      if (uploadOk) {
        doc["path"] = uploadTarget;
        doc["bytes"] = uploadBytes;
      } else {
        doc["error"] = uploadError.isEmpty() ? "Upload failed" : uploadError;
        if (!uploadTarget.isEmpty()) doc["path"] = uploadTarget;
      }
      sendJson(request, uploadOk ? 200 : 400, doc);
    },
    [](AsyncWebServerRequest* request, const String& filename,
       size_t index, uint8_t* data, size_t len, bool final) {
      onUpload(request, filename, index, data, len, final);
    });

  server.on("/api/files", HTTP_DELETE, [](AsyncWebServerRequest* request) {
    if (!mounted) {
      sendError(request, 503, "Filesystem not initialized");
      return;
    }
    handleDelete(request, request->arg("path"));
  });
}
