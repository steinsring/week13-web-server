#ifndef STATIC_FILES_H
#define STATIC_FILES_H
#include "http_request.h"
int static_root_open(const char *path);
int static_files_serve(int client, int root, const HttpRequest *request);
const char *static_mime_type(const char *name);
#endif
