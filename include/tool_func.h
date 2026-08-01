#ifndef TOOL_FUNC_H
#define TOOL_FUNC_H
#include "tool_base.h"

// 四个工具函数实现
void tool_add(float a, float b);
void tool_mul(float a, float b);
void tool_compare(float a, float b);
void tool_count_letter(const char* str, const char* letter);

void init_tools();

#endif