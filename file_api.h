#pragma once
#include <ESPAsyncWebServer.h>

// 注册 /api/files 路由（列目录 / 上传 / 删除）。
void fileApiBegin(AsyncWebServer& server);

// 静态资源下发：/frontend 目录映射到 URL 根路径，未命中时回退入口页。
// 需在 fileApiBegin 之后、server.begin() 之前用 server.onNotFound 挂载。
void fileApiServeStatic(AsyncWebServerRequest* request);

// LittleFS 用量缓存。usedBytes() 底层是 lfs_fs_size，要遍历整个分区的
// metadata pair，文件多时一次调用就要几十毫秒且反复开关 flash cache，
// 绝不能放在 pushState 这类高频路径上。高频调用方用 fsUsageCached()，
// 缓存最多 2 秒；文件系统发生写/删后由操作方调 fsUsageRefresh(true) 立即刷新。
void fsUsageRefresh(bool force);
void fsUsageCached(uint32_t& totalBytes, uint32_t& usedBytes);
