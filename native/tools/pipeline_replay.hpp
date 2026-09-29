#pragma once
#include <cstddef>
#include <string>
#include <sqlite3.h>
extern sqlite3* database;
extern size_t models, pairs, draws, variants;
extern std::string resource;
extern unsigned materialIndex, shapeIndex;
void require(bool condition, const char* reason);
void sql(const char* text);
void replay(const void* memory, size_t size, bool allowDraw);
void reset();
size_t config_count();
void record_begin(void* data, size_t size);
size_t record_end();
void emit_quad();
