// Loads the built MemGuard X shared library the same way the open.mp server does
// (LoadLibrary/dlopen + "ComponentEntryPoint") and checks the component through the SDK ABI:
// name, UID, SDK version, semantic version, default configuration generation, and free().
// It does not need a running server; onLoad (which requires ICore) is covered by the
// runtime test in an actual server (see docs/VERIFICATION.md).
#include <sdk.hpp>

#include "version.hpp"

#include <cstdio>
#include <map>
#include <string>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace {

int g_failures = 0;

void check(bool cond, const char* what)
{
	std::printf("  [%s] %s\n", cond ? " OK " : "FAIL", what);
	if (!cond)
	{
		++g_failures;
	}
}

struct FakeConfig final : IEarlyConfig
{
	struct Value
	{
		ConfigOptionType type = ConfigOptionType_None;
		std::string s;
		int i = 0;
		float f = 0.f;
		bool b = false;
	};
	std::map<std::string, Value> values;
	BanEntry dummyBan { "" };

	static std::string k(StringView key) { return std::string(key.data(), key.size()); }

	const StringView getString(StringView key) const override
	{
		auto it = values.find(k(key));
		return it == values.end() ? StringView() : StringView(it->second.s.data(), it->second.s.size());
	}
	int* getInt(StringView key) override
	{
		auto it = values.find(k(key));
		return it == values.end() ? nullptr : &it->second.i;
	}
	float* getFloat(StringView key) override
	{
		auto it = values.find(k(key));
		return it == values.end() ? nullptr : &it->second.f;
	}
	size_t getStrings(StringView, Span<StringView>) const override { return 0; }
	size_t getStringsCount(StringView) const override { return 0; }
	ConfigOptionType getType(StringView key) const override
	{
		auto it = values.find(k(key));
		return it == values.end() ? ConfigOptionType_None : it->second.type;
	}
	size_t getBansCount() const override { return 0; }
	const BanEntry& getBan(size_t) const override { return dummyBan; }
	void addBan(const BanEntry&) override { }
	void removeBan(size_t) override { }
	void removeBan(const BanEntry&) override { }
	void writeBans() override { }
	void reloadBans() override { }
	void clearBans() override { }
	bool isBanned(const BanEntry&) const override { return false; }
	Pair<bool, StringView> getNameFromAlias(StringView) const override { return { false, StringView() }; }
	void enumOptions(OptionEnumeratorCallback&) const override { }
	bool* getBool(StringView key) override
	{
		auto it = values.find(k(key));
		return it == values.end() ? nullptr : &it->second.b;
	}
	void setString(StringView key, StringView value) override
	{
		Value& v = values[k(key)];
		v.type = ConfigOptionType_String;
		v.s = k(value);
	}
	void setInt(StringView key, int value) override
	{
		Value& v = values[k(key)];
		v.type = ConfigOptionType_Int;
		v.i = value;
	}
	void setFloat(StringView key, float value) override
	{
		Value& v = values[k(key)];
		v.type = ConfigOptionType_Float;
		v.f = value;
	}
	void setStrings(StringView, Span<const StringView>) override { }
	void addAlias(StringView, StringView, bool) override { }
	void setBool(StringView key, bool value) override
	{
		Value& v = values[k(key)];
		v.type = ConfigOptionType_Bool;
		v.b = value;
	}
};

struct NullLogger final : ILogger
{
	void printLn(const char*, ...) override { }
	void vprintLn(const char*, va_list) override { }
	void logLn(LogLevel, const char*, ...) override { }
	void vlogLn(LogLevel, const char*, va_list) override { }
	void printLnU8(const char*, ...) override { }
	void vprintLnU8(const char*, va_list) override { }
	void logLnU8(LogLevel, const char*, ...) override { }
	void vlogLnU8(LogLevel, const char*, va_list) override { }
};

} // namespace

int main(int argc, char** argv)
{
	if (argc < 2)
	{
		std::fprintf(stderr, "usage: %s <path to MemGuardX.dll/.so>\n", argv[0]);
		return 2;
	}
	std::printf("Loading %s\n", argv[1]);
#if defined(_WIN32)
	HMODULE lib = LoadLibraryA(argv[1]);
	check(lib != nullptr, "LoadLibrary succeeded");
	if (!lib)
	{
		std::printf("  GetLastError() = %lu\n", GetLastError());
		return 1;
	}
	auto entry = reinterpret_cast<ComponentEntryPoint_t>(reinterpret_cast<void*>(GetProcAddress(lib, "ComponentEntryPoint")));
#else
	void* lib = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
	check(lib != nullptr, "dlopen succeeded");
	if (!lib)
	{
		std::printf("  dlerror: %s\n", dlerror());
		return 1;
	}
	auto entry = reinterpret_cast<ComponentEntryPoint_t>(dlsym(lib, "ComponentEntryPoint"));
#endif
	check(entry != nullptr, "exported symbol ComponentEntryPoint found");
	if (!entry)
	{
		return 1;
	}

	IComponent* component = entry();
	check(component != nullptr, "ComponentEntryPoint returned a component");
	if (!component)
	{
		return 1;
	}
	const StringView name = component->componentName();
	check(std::string(name.data(), name.size()) == "MemGuard X", "componentName() == \"MemGuard X\"");
	check(component->getUID() == static_cast<UID>(MEMGUARDX_COMPONENT_UID), "getUID() matches MEMGUARDX_COMPONENT_UID");
	check(component->supportedVersion() == OMP_VERSION_SUPPORTED, "supportedVersion() == OMP_VERSION_SUPPORTED");
	const SemanticVersion v = component->componentVersion();
	check(v.major == MEMGUARDX_VERSION_MAJOR && v.minor == MEMGUARDX_VERSION_MINOR && v.patch == MEMGUARDX_VERSION_PATCH, "componentVersion() matches build version");
	check(component->componentType() == ComponentType::Other, "componentType() == Other");

	FakeConfig config;
	NullLogger logger;
	component->provideConfiguration(logger, config, true);
	check(config.getType("memguard.enable") == ConfigOptionType_Bool, "defaults: memguard.enable is a bool");
	check(config.getType("memguard.report_interval") == ConfigOptionType_Int && *config.getInt("memguard.report_interval") == 300, "defaults: memguard.report_interval = 300");
	check(config.getType("memguard.log_level") == ConfigOptionType_String, "defaults: memguard.log_level is a string");
	check(config.values.size() >= 19, "defaults: all memguard.* keys generated");

	FakeConfig existing;
	existing.setInt("memguard.report_interval", 42);
	component->provideConfiguration(logger, existing, false);
	check(*existing.getInt("memguard.report_interval") == 42, "non-default pass keeps administrator values");
	check(existing.getType("memguard.top_n") == ConfigOptionType_Int, "non-default pass fills missing keys");

	component->free(); // must not touch ICore (never loaded) and must delete itself cleanly
	check(true, "free() returned");

#if defined(_WIN32)
	FreeLibrary(lib);
#else
	dlclose(lib);
#endif
	std::printf("%s (%d failure(s))\n", g_failures == 0 ? "PASSED" : "FAILED", g_failures);
	return g_failures == 0 ? 0 : 1;
}
