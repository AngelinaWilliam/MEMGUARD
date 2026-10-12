// MemGuard X - automatic Pawn function instrumentation (source-to-source).
//
// For every function *definition* found in a Pawn source file, inserts
//     MEMGUARD_FUNC("<name>");
// directly after the opening brace of the function body, on the same line, so compiler error
// line numbers stay identical to the original file. MEMGUARD_FUNC (pawn/memguard.inc) declares
// a local whose destructor closes the profiler frame on every exit path, so return statements,
// return values, nesting and recursion are left exactly as written.
//
// The scanner understands comments, strings (including raw \"...\" strings and packed !"..."),
// character literals and preprocessor lines. It never modifies the input; it returns new text.
// Functions it cannot handle safely are reported as skipped with a reason, never guessed.
#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace memguard::instrument {

struct Options
{
	std::string macro = "MEMGUARD_FUNC";
	std::string prefix; ///< Prepended to every zone name (e.g. "gm:").
	std::vector<std::string> exclude; ///< Function names to skip; '*' wildcard allowed.
	std::vector<std::string> include; ///< If non-empty, only matching names are instrumented.
	bool onlyPublic = false; ///< Instrument only `public` functions (callbacks, timers, commands).
	bool allowEmit = false; ///< Also instrument functions containing #emit / __emit (unsafe if they
							///< address locals by hand-computed frame offsets).
};

struct FunctionInfo
{
	std::string name; ///< Function identifier.
	std::string zone; ///< Zone name used in the profiler.
	int line = 0; ///< 1-based line of the opening brace.
	bool isPublic = false;
	bool instrumented = false;
	std::string skipReason; ///< Set when instrumented == false.
};

struct Result
{
	bool ok = false;
	std::string error; ///< Set when ok == false (output is then empty).
	int errorLine = 0;
	std::string output;
	std::vector<FunctionInfo> functions;
	bool includesMemguard = false; ///< Source contains #include <memguard> or "memguard".

	int instrumentedCount() const;
};

Result instrumentSource(std::string_view source, const Options& options);

/// Simple wildcard match ('*' matches any run of characters).
bool wildcardMatch(std::string_view pattern, std::string_view text);

} // namespace memguard::instrument
