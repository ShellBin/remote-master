#include "file_api.h"
#include "fallback_ui.h"
#include <LittleFS.h>
#include <ArduinoJson.h>

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
static const uint8_t kDirQueue = 12;              // 遍历队列长度上限

static WebServer* webServer = nullptr;
static bool mounted = false;
static bool uploadOk = false;
static String uploadTarget, uploadError;
static size_t uploadBytes = 0;
static File uploadFile;

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
  return LittleFS.totalBytes() - LittleFS.usedBytes();
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

// 递归清空目录：每轮只开一个目录句柄并取第一个子项，删除后重新扫描。
// 边删边扫的写法让目录内容多少都不占用堆内存，同时句柄占用恒定为 2。
// 目录本身用 rmdir 删除（空目录），remove 在部分版本的 LittleFS 上对目录不可靠。
static bool clearDirectory(const String& path, uint8_t depth) {
  File entry = LittleFS.open(path);
  if (!entry) return true;  // 已不存在，视为成功
  if (!entry.isDirectory()) {
    entry.close();
    return LittleFS.remove(path);
  }
  entry.close();
  if (depth > kMaxDepth) return false;
  while (true) {
    File scan = LittleFS.open(path);
    if (!scan) break;
    File child = scan.openNextFile();
    if (!child) {
      scan.close();
      break;
    }
    String childPath = child.path();
    bool isDirectory = child.isDirectory();
    child.close();
    scan.close();
    if (isDirectory) {
      if (!clearDirectory(childPath, depth + 1)) return false;
    } else if (!LittleFS.remove(childPath)) {
      Serial.printf("[FS] remove file failed: %s\n", childPath.c_str());
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

static void sendJson(int status, JsonDocument& doc) {
  String body;
  serializeJson(doc, body);
  webServer->send(status, "application/json; charset=utf-8", body);
}

static void sendError(int status, const char* message) {
  JsonDocument doc;
  doc["error"] = message;
  sendJson(status, doc);
}

static void handleList() {
  JsonDocument doc;
  doc["mounted"] = true;
  doc["totalBytes"] = LittleFS.totalBytes();
  doc["usedBytes"] = LittleFS.usedBytes();
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
  sendJson(200, doc);
}
static void handleDownload(const String& path) {
  File file = LittleFS.open(path, "r");
  if (!file || file.isDirectory()) {
    if (file) file.close();
    sendError(404, "File not found");
    return;
  }
  webServer->sendHeader("Cache-Control", "no-cache");
  webServer->streamFile(file, mimeFor(path));
  file.close();
}

static void handleDelete(const String& path) {
  if (!writablePath(path) && !legacyAssetPath(path)) {
    sendError(403, "Only /frontend and legacy root assets are deletable");
    return;
  }
  if (!LittleFS.exists(path)) {
    sendError(404, "Path not found");
    return;
  }
  if (!clearDirectory(path, 0)) {
    sendError(500, "Delete failed; check serial log for details");
    return;
  }
  JsonDocument doc;
  doc["ok"] = true;
  doc["path"] = path;
  sendJson(200, doc);
}

static void handleUpload(HTTPUpload& upload) {
  if (upload.status == UPLOAD_FILE_START) {
    uploadOk = false;
    uploadBytes = 0;
    uploadError = "";
    uploadFile = File();  // 释放上一次上传残留的句柄，避免误关旧文件
    uploadTarget = webServer->arg("path");
    Serial.printf("[FS] upload start: %s\n", uploadTarget.c_str());
    if (!mounted) uploadError = "Filesystem not mounted";
    else if (!uploadPath(uploadTarget)) uploadError = "Only /frontend/<file> is writable";
    else if (freeBytes() < kMinFreeBytes) uploadError = "Not enough free space";
    if (!uploadError.isEmpty()) {
      Serial.printf("[FS] upload refused: %s (%s)\n", uploadError.c_str(), uploadTarget.c_str());
      return;
    }
    uploadFile = LittleFS.open(kUploadTemp, "w");
    uploadOk = static_cast<bool>(uploadFile);
    if (!uploadOk) uploadError = "Cannot open staging file";
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (!uploadOk) return;
    uploadBytes += upload.currentSize;
    // 写入目标先落到暂存文件，全部写完再改名，掉电不会留下半个资源文件。
    if (!uploadFile || uploadBytes > kMaxAssetBytes ||
        uploadFile.write(upload.buf, upload.currentSize) != upload.currentSize) {
      uploadOk = false;
      uploadError = uploadBytes > kMaxAssetBytes ? "File too large" : "Write failed (filesystem full?)";
      Serial.printf("[FS] upload write error: %s (bytes=%u)\n", uploadError.c_str(), uploadBytes);
      if (uploadFile) uploadFile.close();
      LittleFS.remove(kUploadTemp);
    }
  } else if (upload.status == UPLOAD_FILE_END) {
    if (uploadFile) {
      uploadFile.flush();
      uploadFile.close();
    }
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
    Serial.printf("[FS] upload end: %s (ok=%d, bytes=%u, err=%s)\n",
                  uploadTarget.c_str(), uploadOk, uploadBytes, uploadError.c_str());
  } else if (upload.status == UPLOAD_FILE_ABORTED) {
    uploadOk = false;
    uploadError = "Upload aborted";
    Serial.printf("[FS] upload aborted: %s\n", uploadTarget.c_str());
    if (uploadFile) uploadFile.close();
    LittleFS.remove(kUploadTemp);
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

static void serveFallbackLanding() {
  webServer->sendHeader("Cache-Control", "no-store");
  webServer->send_P(200, "text/html; charset=utf-8", kFallbackHtml);
}

// 静态资源下发：/frontend 目录映射到 URL 根路径，未命中时回退入口页。
// 设备上只保留 .gz 压缩版（构建时删除原文件），因此若客户端不支持 gzip
// 则返回 406。openAsset 优先返回 .gz；若某文件未压缩（过小/收益不足）
// 则返回原文件，encoded=false。
void fileApiServeStatic(const String& uri) {
  if (!webServer) return;
  if (webServer->method() != HTTP_GET && webServer->method() != HTTP_HEAD) {
    sendError(405, "Method not allowed");
    return;
  }
  if (uri.startsWith("/api/")) {
    sendError(404, "Unknown endpoint");
    return;
  }
  String relative = requestPath(uri);
  if (relative.isEmpty()) {
    sendError(400, "Bad request path");
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
    if (relative == "/index.html") serveFallbackLanding();
    else sendError(404, "Asset not found; deploy the frontend first");
    return;
  }

  // 不支持 gzip 的客户端退回原文件（设备上 .gz 与原文件同时保留）。
  bool acceptGzip = webServer->hasHeader("Accept-Encoding") &&
                    webServer->header("Accept-Encoding").indexOf("gzip") >= 0;
  if (encoded && !acceptGzip) {
    file.close();
    file = LittleFS.open(String(kFrontendRoot) + relative, "r");
    if (!file) {
      sendError(404, "Asset not found; deploy the frontend first");
      return;
    }
    encoded = false;
  }

  bool immutable = relative.startsWith("/assets/");
  webServer->sendHeader("Cache-Control", immutable ? "public, max-age=31536000, immutable" : "no-cache");
  webServer->sendHeader("X-Content-Type-Options", "nosniff");
  if (encoded) webServer->sendHeader("Content-Encoding", "gzip");
  webServer->streamFile(file, mimeFor(relative));
  file.close();
}

// ==================== 路由注册 ====================

void fileApiBegin(WebServer& server) {
  webServer = &server;
  // begin() 在已挂载时同样返回成功；totalBytes() 作为兜底判据。
  mounted = LittleFS.begin(false) || LittleFS.totalBytes() > 0;

  server.on("/api/files", HTTP_GET, [] {
    if (!mounted) {
      sendError(503, "Filesystem not initialized");
      return;
    }
    String path = webServer->arg("path");
    if (path.isEmpty()) {
      handleList();
      return;
    }
    if (!readablePath(path)) {
      sendError(403, "Protected path");
      return;
    }
    handleDownload(path);
  });

  server.on(
    "/api/files", HTTP_POST,
    [] {
      JsonDocument doc;
      doc["ok"] = uploadOk;
      if (uploadOk) {
        doc["path"] = uploadTarget;
        doc["bytes"] = uploadBytes;
      } else {
        doc["error"] = uploadError.isEmpty() ? "Upload failed" : uploadError;
        if (!uploadTarget.isEmpty()) doc["path"] = uploadTarget;
      }
      sendJson(uploadOk ? 200 : 400, doc);
    },
    [] { handleUpload(webServer->upload()); });

  server.on("/api/files", HTTP_DELETE, [] {
    if (!mounted) {
      sendError(503, "Filesystem not initialized");
      return;
    }
    handleDelete(webServer->arg("path"));
  });
}
