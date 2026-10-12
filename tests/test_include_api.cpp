// Keeps pawn/memguard.inc in sync with the natives the component registers
// (src/component/pawn_bridge.cpp): same names, same parameter counts, and a
// MEMGUARD_DISABLE stub for every public native.
#include "test_framework.hpp"

#include <fstream>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <string>

#ifndef MEMGUARDX_SOURCE_DIR
#error "MEMGUARDX_SOURCE_DIR must be defined"
#endif

namespace {

std::string slurp(const std::string& rel)
{
	std::ifstream in(std::string(MEMGUARDX_SOURCE_DIR) + "/" + rel, std::ios::binary);
	std::stringstream ss;
	ss << in.rdbuf();
	return ss.str();
}

int countParams(const std::string& args)
{
	if (args.find_first_not_of(" \t") == std::string::npos)
	{
		return 0;
	}
	int n = 1;
	for (char c : args)
	{
		n += c == ',' ? 1 : 0;
	}
	return n;
}

} // namespace

TEST(include_api_matches_registered_natives)
{
	const std::string inc = slurp("pawn/memguard.inc");
	const std::string bridge = slurp("src/component/pawn_bridge.cpp");
	CHECK(!inc.empty());
	CHECK(!bridge.empty());
	const std::size_t split = inc.find("#else // MEMGUARD_DISABLE");
	CHECK(split != std::string::npos);
	const std::string enabled = inc.substr(0, split);
	const std::string disabled = inc.substr(split);

	std::map<std::string, int> declared;
	const std::regex nativeRe(R"re(\nnative (MemGuard_\w+)\(([^)]*)\);)re");
	for (std::sregex_iterator it(enabled.begin(), enabled.end(), nativeRe), end; it != end; ++it)
	{
		declared[(*it)[1]] = countParams((*it)[2]);
	}

	std::map<std::string, std::string> registered; // native -> C++ function
	const std::regex regRe(R"re(\{ "(MemGuard_\w+)", &PawnBridgeAccess::(\w+) \})re");
	for (std::sregex_iterator it(bridge.begin(), bridge.end(), regRe), end; it != end; ++it)
	{
		registered[(*it)[1]] = (*it)[2];
	}

	CHECK(declared.size() >= 14);
	CHECK_EQ(declared.size(), registered.size());
	for (const auto& [name, params] : declared)
	{
		const auto r = registered.find(name);
		if (r == registered.end())
		{
			mgtest::fail(__FILE__, __LINE__, "declared but not registered: " + name);
			continue;
		}
		// The C++ implementation must validate exactly the declared parameter count.
		const std::regex implRe("AMX_NATIVE_CALL " + r->second + R"re(\([^)]*\)\s*\{([\s\S]*?)\n\t\})re");
		std::smatch m;
		if (!std::regex_search(bridge, m, implRe))
		{
			mgtest::fail(__FILE__, __LINE__, "implementation not found for " + name);
			continue;
		}
		const std::string body = m[1];
		std::smatch argc;
		if (params > 0)
		{
			const bool found = std::regex_search(body, argc, std::regex(R"re(argCount\(params, (\d+)\))re"));
			CHECK(found);
			if (found && std::stoi(argc[1]) != params)
			{
				mgtest::fail(__FILE__, __LINE__, name + ": include declares " + std::to_string(params) + " params, component checks " + std::string(argc[1]));
			}
		}
		// Every native except the MEMGUARD_FUNC internals needs a MEMGUARD_DISABLE stub.
		if (name != "MemGuard_FuncEnter" && name != "MemGuard_FuncLeave" && disabled.find("stock " + name + "(") == std::string::npos)
		{
			mgtest::fail(__FILE__, __LINE__, "no MEMGUARD_DISABLE stub for " + name);
		}
	}
	for (const auto& [name, fn] : registered)
	{
		if (!declared.count(name))
		{
			mgtest::fail(__FILE__, __LINE__, "registered but not declared in memguard.inc: " + name);
		}
	}
}
