// Minimal self-contained test framework (no external dependencies).
#pragma once

#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace mgtest {

struct TestCase
{
	const char* name;
	std::function<void()> fn;
};

inline std::vector<TestCase>& registry()
{
	static std::vector<TestCase> tests;
	return tests;
}

inline int& failures()
{
	static int count = 0;
	return count;
}

struct Registrar
{
	Registrar(const char* name, std::function<void()> fn) { registry().push_back(TestCase { name, std::move(fn) }); }
};

inline void fail(const char* file, int line, const std::string& what)
{
	++failures();
	std::fprintf(stderr, "    FAILED %s:%d: %s\n", file, line, what.c_str());
}

} // namespace mgtest

#define MG_CAT2(a, b) a##b
#define MG_CAT(a, b) MG_CAT2(a, b)

#define TEST(name)                                                                       \
	static void name();                                                                  \
	static ::mgtest::Registrar MG_CAT(mg_registrar_, name)(#name, &name);               \
	static void name()

#define CHECK(cond)                                                                      \
	do                                                                                   \
	{                                                                                    \
		if (!(cond))                                                                     \
			::mgtest::fail(__FILE__, __LINE__, "CHECK(" #cond ")");                      \
	} while (0)

#define CHECK_EQ(a, b)                                                                   \
	do                                                                                   \
	{                                                                                    \
		const auto mg_a = (a);                                                           \
		const auto mg_b = (b);                                                           \
		if (!(mg_a == mg_b))                                                             \
			::mgtest::fail(__FILE__, __LINE__,                                           \
				"CHECK_EQ(" #a ", " #b ") -> " + std::to_string(mg_a) + " vs " + std::to_string(mg_b)); \
	} while (0)

#define CHECK_STR_CONTAINS(haystack, needle)                                             \
	do                                                                                   \
	{                                                                                    \
		const std::string mg_h = (haystack);                                             \
		if (mg_h.find(needle) == std::string::npos)                                      \
			::mgtest::fail(__FILE__, __LINE__, std::string("expected to find \"") + (needle) + "\""); \
	} while (0)
