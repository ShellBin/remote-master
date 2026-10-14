#pragma once
#include <WebServer.h>

// 注册 /api/files 与 /api/fs/initialize 路由。
void fileApiBegin(WebServer& server);

// 静态资源下发：/frontend 目录映射到 URL 根路径，未命中时回退入口页。
// 需在 fileApiBegin 之后、server.begin() 之前用 server.onNotFound 挂载。
void fileApiServeStatic(const String& uri);
