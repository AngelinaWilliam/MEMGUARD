#include "test_framework.hpp"

#include "../src/instrument/pawn_instrumenter.hpp"

#include <algorithm>
#include <string>

using namespace memguard::instrument;

namespace {

const FunctionInfo* findFn(const Result& r, const std::string& name)
{
	for (const FunctionInfo& f : r.functions)
	{
		if (f.name == name)
		{
			return &f;
		}
	}
	return nullptr;
}

std::size_t countLines(const std::string& s)
{
	return static_cast<std::size_t>(std::count(s.begin(), s.end(), '\n'));
}

std::size_t countOf(const std::string& s, const std::string& needle)
{
	std::size_t n = 0;
	for (std::size_t p = s.find(needle); p != std::string::npos; p = s.find(needle, p + 1))
	{
		++n;
	}
	return n;
}

} // namespace

TEST(instrument_basic_functions_and_line_numbers)
{
	const std::string src =
		"#include <open.mp>\n"
		"#include <memguard>\n"
		"\n"
		"stock Float:Speed(Float:x)\n"
		"{\n"
		"    if (x > 1.0) return x * 2.0;\n"
		"    return x;\n"
		"}\n"
		"public OnGameModeInit()\n"
		"{\n"
		"    return 1;\n"
		"}\n"
		"main() { print(\"hi\"); }\n";
	const Result r = instrumentSource(src, Options {});
	CHECK(r.ok);
	CHECK(r.includesMemguard);
	CHECK_EQ(r.functions.size(), static_cast<std::size_t>(3));
	CHECK_EQ(r.instrumentedCount(), 3);
	CHECK_EQ(countLines(r.output), countLines(src)); // line numbers preserved
	// inserted in front of the first statement, keeping its indentation and line
	CHECK_STR_CONTAINS(r.output, "{\n    MEMGUARD_FUNC(\"Speed\"); if (x > 1.0) return x * 2.0;");
	CHECK_STR_CONTAINS(r.output, "public OnGameModeInit()\n{\n    MEMGUARD_FUNC(\"OnGameModeInit\"); return 1;");
	CHECK_STR_CONTAINS(r.output, "main() { MEMGUARD_FUNC(\"main\"); print(\"hi\"); }");
	const FunctionInfo* f = findFn(r, "OnGameModeInit");
	CHECK(f && f->isPublic && f->line == 10);
	CHECK(findFn(r, "Speed") && !findFn(r, "Speed")->isPublic);
	// return statements are untouched
	CHECK_EQ(countOf(r.output, "return"), countOf(src, "return"));
}

TEST(instrument_ignores_non_functions)
{
	const std::string src =
		"native Foo(a);\n"
		"forward Bar(a);\n"
		"enum E_DATA { E_A, E_B }\n"
		"enum (<<= 1) { FLAG_A = 1, FLAG_B }\n"
		"new gArr[3] = { 1, 2, 3 };\n"
		"new const gNested[][2] = { {1, 2}, {3, 4} };\n"
		"static gS[2] = {0, 0};\n"
		"#define BLOCK(%0) { %0 }\n"
		"public gVar = 5;\n"
		"Real() { return 1; }\n";
	const Result r = instrumentSource(src, Options {});
	CHECK(r.ok);
	CHECK_EQ(r.functions.size(), static_cast<std::size_t>(1));
	CHECK(findFn(r, "Real") != nullptr);
	CHECK_EQ(countOf(r.output, "MEMGUARD_FUNC"), static_cast<std::size_t>(1));
}

TEST(instrument_strings_comments_and_chars_do_not_confuse_braces)
{
	const std::string src =
		"/* Fake() { } in a comment */\n"
		"// Fake2() {\n"
		"A()\n"
		"{\n"
		"    new s[] = \"} { not code \\\" still string }\";\n"
		"    new r[] = \\\"C:\\path\\}\";\n" // raw string
		"    new p[] = !\"packed {\";\n"
		"    new c = '}', d = '\\'', e = '{';\n"
		"    return s[0] + r[0] + p[0] + c + d + e;\n"
		"}\n"
		"B() { return 2; }\n";
	const Result r = instrumentSource(src, Options {});
	CHECK(r.ok);
	CHECK_EQ(r.functions.size(), static_cast<std::size_t>(2));
	CHECK(findFn(r, "A") && findFn(r, "B"));
	CHECK(!findFn(r, "Fake") && !findFn(r, "Fake2"));
}

TEST(instrument_skips_emit_operator_and_already_instrumented)
{
	const std::string src =
		"Asm()\n"
		"{\n"
		"#emit LOAD.S.pri 12\n"
		"    return 0;\n"
		"}\n"
		"Asm2() { __emit(load.s.pri 12); }\n"
		"stock operator~(const Tag:t[], c) { }\n"
		"Done() { MEMGUARD_FUNC(\"Done\"); return 1; }\n"
		"Plain() { return 1; }\n";
	const Result r = instrumentSource(src, Options {});
	CHECK(r.ok);
	CHECK(findFn(r, "Asm") && !findFn(r, "Asm")->instrumented);
	CHECK_STR_CONTAINS(findFn(r, "Asm")->skipReason, "#emit");
	CHECK(findFn(r, "Asm2") && !findFn(r, "Asm2")->instrumented);
	CHECK(r.functions.size() >= 4);
	CHECK(findFn(r, "Done") && !findFn(r, "Done")->instrumented);
	CHECK(findFn(r, "Plain") && findFn(r, "Plain")->instrumented);
	CHECK_EQ(countOf(r.output, "MEMGUARD_FUNC(\"Done\")"), static_cast<std::size_t>(1));
	Options allow;
	allow.allowEmit = true;
	const Result r2 = instrumentSource(src, allow);
	CHECK(findFn(r2, "Asm")->instrumented);
}

TEST(instrument_is_idempotent)
{
	const std::string src = "A() { return 1; }\nB(x) { if (x) return 2; return 3; }\n";
	const Result once = instrumentSource(src, Options {});
	const Result twice = instrumentSource(once.output, Options {});
	CHECK(once.ok && twice.ok);
	CHECK(once.output == twice.output);
	CHECK_EQ(twice.instrumentedCount(), 0);
}

TEST(instrument_names_tags_states_and_macro_headers)
{
	const std::string src =
		"stock bool:IsOk(v) { return v > 0; }\n"
		"CMD:help(playerid, params[]) { return 1; }\n"
		"hook OnPlayerConnect(playerid) { return 1; }\n"
		"Walk() <moving> { }\n"
		"Walk() <> { }\n"
		"Float:Dist(Float:a[3], Float:b[3]) { return a[0] - b[0]; }\n";
	const Result r = instrumentSource(src, Options {});
	CHECK(r.ok);
	CHECK_STR_CONTAINS(r.output, "MEMGUARD_FUNC(\"IsOk\")");
	CHECK_STR_CONTAINS(r.output, "MEMGUARD_FUNC(\"CMD:help\")");
	CHECK_STR_CONTAINS(r.output, "MEMGUARD_FUNC(\"hook OnPlayerConnect\")");
	CHECK_STR_CONTAINS(r.output, "MEMGUARD_FUNC(\"Walk<moving>\")");
	CHECK_STR_CONTAINS(r.output, "MEMGUARD_FUNC(\"Walk<>\")");
	CHECK_STR_CONTAINS(r.output, "MEMGUARD_FUNC(\"Dist\")");
}

TEST(instrument_filters)
{
	const std::string src = "public OnA() { return 1; }\nstock helper() { return 1; }\nstock other() { return 1; }\n";
	Options pub;
	pub.onlyPublic = true;
	Result r = instrumentSource(src, pub);
	CHECK_EQ(r.instrumentedCount(), 1);
	CHECK(findFn(r, "OnA")->instrumented);

	Options ex;
	ex.exclude = { "help*" };
	r = instrumentSource(src, ex);
	CHECK(!findFn(r, "helper")->instrumented);
	CHECK(findFn(r, "other")->instrumented);

	Options only;
	only.include = { "other" };
	only.prefix = "gm:";
	r = instrumentSource(src, only);
	CHECK_EQ(r.instrumentedCount(), 1);
	CHECK_STR_CONTAINS(r.output, "MEMGUARD_FUNC(\"gm:other\")");

	CHECK(wildcardMatch("On*", "OnPlayerConnect"));
	CHECK(wildcardMatch("*Player*", "OnPlayerConnect"));
	CHECK(!wildcardMatch("On*X", "OnPlayerConnect"));
}

TEST(instrument_preprocessor_lines_and_crlf)
{
	const std::string src =
		"#if defined FOO\r\n"
		"  #define X(%0) \\\r\n"
		"     { %0 }\r\n"
		"#endif\r\n"
		"public Cb(a)\r\n"
		"#if defined BAR\r\n"
		"#endif\r\n"
		"{\r\n"
		"\treturn a;\r\n"
		"}\r\n";
	const Result r = instrumentSource(src, Options {});
	CHECK(r.ok);
	CHECK_EQ(r.functions.size(), static_cast<std::size_t>(1));
	CHECK_STR_CONTAINS(r.output, "{\r\n\tMEMGUARD_FUNC(\"Cb\"); return a;\r\n");
	CHECK(findFn(r, "Cb") && findFn(r, "Cb")->line == 8);
}

TEST(instrument_reports_syntax_errors_instead_of_guessing)
{
	Result r = instrumentSource("A() { return 1;\n", Options {});
	CHECK(!r.ok);
	CHECK_STR_CONTAINS(r.error, "unbalanced");
	r = instrumentSource("A() { new s[] = \"unterminated; }\n", Options {});
	CHECK(!r.ok);
	r = instrumentSource("/* never closed\nA() {}\n", Options {});
	CHECK(!r.ok);
	r = instrumentSource("}\n", Options {});
	CHECK(!r.ok);
	CHECK(r.output.empty());
}

TEST(instrument_directive_before_first_statement_inserts_after_brace)
{
	const std::string src =
		"A()\n"
		"{\n"
		"#if defined X\n"
		"    foo();\n"
		"#endif\n"
		"    return 1;\n"
		"}\n"
		"B()\n"
		"{\n"
		"}\n";
	const Result r = instrumentSource(src, Options {});
	CHECK(r.ok);
	// must not land inside the #if branch
	CHECK_STR_CONTAINS(r.output, "A()\n{ MEMGUARD_FUNC(\"A\");\n#if defined X");
	CHECK_STR_CONTAINS(r.output, "B()\n{\nMEMGUARD_FUNC(\"B\"); }");
	CHECK_EQ(countLines(r.output), countLines(src));
}

TEST(instrument_skips_memguard_api_functions)
{
	const Result r = instrumentSource("stock MemGuard_ZoneId(const name[]) { return -1; }\nMine() { return 1; }\n", Options {});
	CHECK(r.ok);
	CHECK(!findFn(r, "MemGuard_ZoneId")->instrumented);
	CHECK(findFn(r, "Mine")->instrumented);
}
