#pragma once
#include <cassert>
#define ASSERT_MSG(category, condition, ...) assert(condition)
#define DEBUG_ASSERT(condition) assert(condition)
#define DEBUG_ASSERT_MSG(category, condition, ...) assert(condition)
