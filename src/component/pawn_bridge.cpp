#include "pawn_bridge.hpp"

#include "version.hpp"

#include <algorithm>
#include <cstring>
#include <string>

namespace memguard {

PawnBridge* PawnBridge::instance_ = nullptr;

namespace {

	constexpr std::size_t kMaxName = 128;

	// Return codes shared with pawn/memguard.inc.
	constexpr cell MG_ALLOC_TRACKED = 1;
	constexpr cell MG_ALLOC_DUPLICATE = 2;
	constexpr cell MG_FREE_RELEASED = 1;
	constexpr cell MG_FREE_UNKNOWN = -1;
	constexpr cell MG_FREE_DOUBLE = -2;
	constexpr cell MG_IGNORED = 0;

	cell clampToCell(std::uint64_t v)
	{
		const std::uint64_t maxCell = static_cast<std::uint64_t>(0x7FFFFFFF);
		return static_cast<cell>(v > maxCell ? maxCell : v);
	}

} // namespace

struct PawnBridgeAccess
{
	static PawnBridge* bridge() { return PawnBridge::instance_; }

	static bool argCount(const cell* params, int needed)
	{
		return params[0] >= static_cast<cell>(needed * sizeof(cell));
	}

	static cell* refAddr(AMX* amx, cell amxAddr)
	{
		PawnBridge* b = bridge();
		if (!b || !b->pawn_)
		{
			return nullptr;
		}
		IPawnScript* script = b->pawn_->getScript(amx);
		cell* phys = nullptr;
		if (!script || script->GetAddr(amxAddr, &phys) != AMX_ERR_NONE)
		{
			return nullptr;
		}
		return phys;
	}

	static cell usClamp(Nanos ns)
	{
		return clampToCell(ns / 1000ull);
	}

	// native MemGuard_FuncEnter(&cache, const name[]);
	// Used by the MEMGUARD_FUNC macro. `cache` is a per-function static that holds the zone id
	// after the first call, so the name string is only read once per script load.
	static cell AMX_NATIVE_CALL FuncEnter(AMX* amx, const cell* params)
	{
		PawnBridge* b = bridge();
		if (!b || !argCount(params, 2))
		{
			return 0;
		}
		cell* cache = refAddr(amx, params[1]);
		if (!cache)
		{
			return 0;
		}
		Profiler& p = b->engine_.profiler();
		ZoneId id;
		if (*cache >= 0 && static_cast<std::size_t>(*cache) < p.zoneCount())
		{
			id = static_cast<ZoneId>(*cache);
		}
		else
		{
			char name[kMaxName];
			if (!readString(amx, params[2], name, sizeof(name)))
			{
				return 0;
			}
			id = p.zone(name, ZoneKind::Function);
			if (id == Profiler::kInvalidZone)
			{
				return 0;
			}
			*cache = static_cast<cell>(id);
		}
		return static_cast<cell>(p.beginSerial(id));
	}

	// native MemGuard_FuncLeave(frame);
	static cell AMX_NATIVE_CALL FuncLeave(AMX*, const cell* params)
	{
		PawnBridge* b = bridge();
		if (!b || !argCount(params, 1) || params[1] <= 0)
		{
			return 0;
		}
		return b->engine_.profiler().endSerial(static_cast<std::uint32_t>(params[1])) ? 1 : 0;
	}

	// native MemGuard_GetZoneStats(const name[], &calls, &total_us, &self_us, &max_us);
	static cell AMX_NATIVE_CALL GetZoneStats(AMX* amx, const cell* params)
	{
		PawnBridge* b = bridge();
		char name[kMaxName];
		if (!b || !argCount(params, 5) || !readString(amx, params[1], name, sizeof(name)))
		{
			return 0;
		}
		Profiler& p = b->engine_.profiler();
		const auto id = p.findZone(name);
		const auto z = id ? p.zoneStats(*id) : std::nullopt;
		const cell values[4] = {
			z ? clampToCell(z->calls) : 0,
			z ? usClamp(z->totalNs) : 0,
			z ? usClamp(z->selfNs) : 0,
			z ? usClamp(z->maxNs) : 0,
		};
		for (int i = 0; i < 4; ++i)
		{
			if (cell* out = refAddr(amx, params[2 + i]))
			{
				*out = values[i];
			}
		}
		return z ? 1 : 0;
	}

	// native MemGuard_GetVersion();
	static cell AMX_NATIVE_CALL GetVersion(AMX*, const cell*)
	{
		return (MEMGUARDX_VERSION_MAJOR << 16) | (MEMGUARDX_VERSION_MINOR << 8) | MEMGUARDX_VERSION_PATCH;
	}

	static bool readString(AMX* amx, cell amxAddr, char* out, std::size_t size)
	{
		PawnBridge* b = bridge();
		if (!b || !b->pawn_)
		{
			return false;
		}
		IPawnScript* script = b->pawn_->getScript(amx);
		if (!script)
		{
			return false;
		}
		cell* phys = nullptr;
		if (script->GetAddr(amxAddr, &phys) != AMX_ERR_NONE || !phys)
		{
			return false;
		}
		out[0] = '\0';
		if (script->GetString(out, phys, false, size) != AMX_ERR_NONE)
		{
			return false;
		}
		out[size - 1] = '\0';
		return out[0] != '\0';
	}

	// native MemGuard_ProfileBegin(const name[]);
	static cell AMX_NATIVE_CALL ProfileBegin(AMX* amx, const cell* params)
	{
		PawnBridge* b = bridge();
		char name[kMaxName];
		if (!b || !argCount(params, 1) || !readString(amx, params[1], name, sizeof(name)))
		{
			return -1;
		}
		Profiler& p = b->engine_.profiler();
		const ZoneId id = p.zone(name, ZoneKind::Script);
		if (id == Profiler::kInvalidZone)
		{
			return -1;
		}
		p.begin(id);
		return static_cast<cell>(id);
	}

	// native MemGuard_ZoneId(const name[]);
	static cell AMX_NATIVE_CALL ZoneIdNative(AMX* amx, const cell* params)
	{
		PawnBridge* b = bridge();
		char name[kMaxName];
		if (!b || !argCount(params, 1) || !readString(amx, params[1], name, sizeof(name)))
		{
			return -1;
		}
		const ZoneId id = b->engine_.profiler().zone(name, ZoneKind::Script);
		return id == Profiler::kInvalidZone ? -1 : static_cast<cell>(id);
	}

	// native MemGuard_ZoneBegin(zone);
	static cell AMX_NATIVE_CALL ZoneBegin(AMX*, const cell* params)
	{
		PawnBridge* b = bridge();
		if (!b || !argCount(params, 1) || params[1] < 0)
		{
			return 0;
		}
		return b->engine_.profiler().begin(static_cast<ZoneId>(params[1])) != Profiler::kNoFrame ? 1 : 0;
	}

	// native MemGuard_ProfileEnd(zone);
	static cell AMX_NATIVE_CALL ProfileEnd(AMX*, const cell* params)
	{
		PawnBridge* b = bridge();
		if (!b || !argCount(params, 1) || params[1] < 0)
		{
			return 0;
		}
		return b->engine_.profiler().endZone(static_cast<ZoneId>(params[1])) ? 1 : 0;
	}

	static bool categoryFromArg(AMX* amx, cell addr, CategoryId& out)
	{
		char name[kMaxName];
		if (!readString(amx, addr, name, sizeof(name)))
		{
			return false;
		}
		out = bridge()->engine_.tracker().category(std::string("script.") + name, true);
		return true;
	}

	// native MemGuard_TrackAlloc(const category[], id, size = 0);
	static cell AMX_NATIVE_CALL TrackAlloc(AMX* amx, const cell* params)
	{
		CategoryId cat;
		if (!bridge() || !argCount(params, 3) || !categoryFromArg(amx, params[1], cat) || params[3] < 0)
		{
			return MG_IGNORED;
		}
		const AllocResult r = bridge()->engine_.tracker().onAlloc(cat, static_cast<std::uint32_t>(params[2]), static_cast<std::uint64_t>(params[3]));
		if (r == AllocResult::DuplicateActive)
		{
			bridge()->engine_.logger().logLimited(LogLevel::Warning, "dup-alloc",
				"[MemGuardX] Tracking error: MemGuard_TrackAlloc for an id that is still active (category '" + bridge()->engine_.tracker().categoryName(cat) + "', id " + std::to_string(params[2]) + ")");
			return MG_ALLOC_DUPLICATE;
		}
		return r == AllocResult::Tracked ? MG_ALLOC_TRACKED : MG_IGNORED;
	}

	// native MemGuard_TrackFree(const category[], id);
	static cell AMX_NATIVE_CALL TrackFree(AMX* amx, const cell* params)
	{
		CategoryId cat;
		if (!bridge() || !argCount(params, 2) || !categoryFromArg(amx, params[1], cat))
		{
			return MG_IGNORED;
		}
		const FreeResult r = bridge()->engine_.tracker().onFree(cat, static_cast<std::uint32_t>(params[2]));
		switch (r)
		{
		case FreeResult::Released:
			return MG_FREE_RELEASED;
		case FreeResult::UnknownFree:
			bridge()->engine_.logger().logLimited(LogLevel::Warning, "unknown-free",
				"[MemGuardX] Tracking error: MemGuard_TrackFree of an id that was never tracked (category '" + bridge()->engine_.tracker().categoryName(cat) + "', id " + std::to_string(params[2]) + ")");
			return MG_FREE_UNKNOWN;
		case FreeResult::DoubleFree:
			bridge()->engine_.logger().logLimited(LogLevel::Warning, "double-free",
				"[MemGuardX] Tracking error: double free (category '" + bridge()->engine_.tracker().categoryName(cat) + "', id " + std::to_string(params[2]) + ")");
			return MG_FREE_DOUBLE;
		default:
			return MG_IGNORED;
		}
	}

	// native MemGuard_GetActiveCount(const category[]);
	static cell AMX_NATIVE_CALL GetActiveCount(AMX* amx, const cell* params)
	{
		char name[kMaxName];
		if (!bridge() || !argCount(params, 1) || !readString(amx, params[1], name, sizeof(name)))
		{
			return -1;
		}
		MemoryTracker& t = bridge()->engine_.tracker();
		// Accept both the bare script category name and a full name such as "pool.vehicle".
		auto cat = t.findCategory(std::string("script.") + name);
		if (!cat)
		{
			cat = t.findCategory(name);
		}
		if (!cat)
		{
			return 0;
		}
		const auto stats = t.categoryStats(*cat);
		return stats ? clampToCell(stats->active) : 0;
	}

	// native MemGuard_GetZoneCalls(const name[]);
	static cell AMX_NATIVE_CALL GetZoneCalls(AMX* amx, const cell* params)
	{
		char name[kMaxName];
		if (!bridge() || !argCount(params, 1) || !readString(amx, params[1], name, sizeof(name)))
		{
			return -1;
		}
		Profiler& p = bridge()->engine_.profiler();
		const auto id = p.findZone(name);
		if (!id)
		{
			return 0;
		}
		const auto z = p.zoneStats(*id);
		return z ? clampToCell(z->calls) : 0;
	}

	// native MemGuard_Snapshot();
	static cell AMX_NATIVE_CALL SnapshotNative(AMX*, const cell*)
	{
		if (!bridge())
		{
			return 0;
		}
		return clampToCell(bridge()->engine_.takeSnapshot().sequence);
	}

	// native MemGuard_WriteReport();
	static cell AMX_NATIVE_CALL WriteReport(AMX*, const cell*)
	{
		if (!bridge())
		{
			return 0;
		}
		std::string error;
		if (!bridge()->engine_.writeReport(error))
		{
			bridge()->engine_.logger().log(LogLevel::Error, "[MemGuardX] MemGuard_WriteReport failed: " + error);
			return 0;
		}
		return 1;
	}
};

namespace {

	const AMX_NATIVE_INFO g_natives[] = {
		{ "MemGuard_ProfileBegin", &PawnBridgeAccess::ProfileBegin },
		{ "MemGuard_ProfileEnd", &PawnBridgeAccess::ProfileEnd },
		{ "MemGuard_ZoneId", &PawnBridgeAccess::ZoneIdNative },
		{ "MemGuard_ZoneBegin", &PawnBridgeAccess::ZoneBegin },
		{ "MemGuard_TrackAlloc", &PawnBridgeAccess::TrackAlloc },
		{ "MemGuard_TrackFree", &PawnBridgeAccess::TrackFree },
		{ "MemGuard_GetActiveCount", &PawnBridgeAccess::GetActiveCount },
		{ "MemGuard_GetZoneCalls", &PawnBridgeAccess::GetZoneCalls },
		{ "MemGuard_Snapshot", &PawnBridgeAccess::SnapshotNative },
		{ "MemGuard_WriteReport", &PawnBridgeAccess::WriteReport },
		{ "MemGuard_FuncEnter", &PawnBridgeAccess::FuncEnter },
		{ "MemGuard_FuncLeave", &PawnBridgeAccess::FuncLeave },
		{ "MemGuard_GetZoneStats", &PawnBridgeAccess::GetZoneStats },
		{ "MemGuard_GetVersion", &PawnBridgeAccess::GetVersion },
	};

} // namespace

PawnBridge::PawnBridge(Engine& engine)
	: engine_(engine)
{
	instance_ = this;
}

PawnBridge::~PawnBridge()
{
	detachAll();
	if (instance_ == this)
	{
		instance_ = nullptr;
	}
}

PawnBridge::ScriptState* PawnBridge::find(AMX* amx)
{
	if (cached_ && cached_->amx == amx)
	{
		return cached_;
	}
	for (auto& s : scripts_)
	{
		if (s->amx == amx)
		{
			cached_ = s.get();
			return cached_;
		}
	}
	return nullptr;
}

void PawnBridge::registerNatives(IPawnScript& script)
{
	const int count = static_cast<int>(sizeof(g_natives) / sizeof(g_natives[0]));
	script.Register(g_natives, count);
}

void PawnBridge::attach(IPawnScript& script)
{
	registerNatives(script);

	AMX* amx = script.GetAMX();
	if (!amx)
	{
		return;
	}
	if (find(amx))
	{
		return;
	}
	auto state = std::make_unique<ScriptState>();
	state->amx = amx;
	state->script = &script;
	state->id = script.GetID();
	int numNatives = 0;
	if (script.NumNatives(&numNatives) == AMX_ERR_NONE && numNatives > 0)
	{
		state->nativeZones.assign(static_cast<std::size_t>(numNatives), kUnresolved);
	}
	if (amx->callback != &PawnBridge::callbackHook)
	{
		state->original = amx->callback;
		if (!fallbackOriginal_)
		{
			fallbackOriginal_ = amx->callback;
		}
		amx->callback = &PawnBridge::callbackHook;
	}
	else
	{
		state->original = fallbackOriginal_;
	}
	engine_.logger().logf(LogLevel::Info, "[MemGuardX] Attached to Pawn script id %d (%d natives), native profiling %s",
		state->id, numNatives, profileNatives_ ? "on" : "off");
	scripts_.push_back(std::move(state));
}

void PawnBridge::detach(IPawnScript& script)
{
	AMX* amx = script.GetAMX();
	for (auto it = scripts_.begin(); it != scripts_.end(); ++it)
	{
		if ((*it)->amx == amx)
		{
			if (amx && amx->callback == &PawnBridge::callbackHook && (*it)->original)
			{
				amx->callback = (*it)->original;
			}
			if (cached_ == it->get())
			{
				cached_ = nullptr;
			}
			scripts_.erase(it);
			break;
		}
	}
	// A script that was unloaded in the middle of a profiled section can leave frames open.
	if (scripts_.empty() && engine_.profiler().openFrames() != 0)
	{
		engine_.profiler().discardOpenFrames();
	}
}

void PawnBridge::detachAll()
{
	for (auto& s : scripts_)
	{
		if (s->amx && s->amx->callback == &PawnBridge::callbackHook && s->original)
		{
			s->amx->callback = s->original;
		}
	}
	scripts_.clear();
	cached_ = nullptr;
}

std::size_t PawnBridge::verifyHooks()
{
	std::size_t rehooked = 0;
	for (auto& s : scripts_)
	{
		if (s->amx && s->amx->callback != &PawnBridge::callbackHook)
		{
			s->original = s->amx->callback;
			s->amx->callback = &PawnBridge::callbackHook;
			++rehooked;
		}
	}
	rehooks_ += rehooked;
	return rehooked;
}

ZoneId PawnBridge::resolveNative(ScriptState& state, cell index)
{
	if (index < 0)
	{
		return kPassthrough;
	}
	const std::size_t i = static_cast<std::size_t>(index);
	if (i >= state.nativeZones.size())
	{
		state.nativeZones.resize(i + 1, kUnresolved);
	}
	ZoneId& z = state.nativeZones[i];
	if (z != kUnresolved)
	{
		return z;
	}
	char name[256] = { 0 };
	if (state.script->GetNative(static_cast<int>(index), name) != AMX_ERR_NONE || name[0] == '\0')
	{
		z = kPassthrough;
		return z;
	}
	name[sizeof(name) - 1] = '\0';
	// Our own natives manipulate the profiler stack themselves; timing them would interfere.
	if (std::strncmp(name, "MemGuard_", 9) == 0)
	{
		z = kPassthrough;
		return z;
	}
	const ZoneId id = engine_.profiler().zone(name, ZoneKind::Native);
	z = id == Profiler::kInvalidZone ? kPassthrough : id;
	return z;
}

int AMXAPI PawnBridge::callbackHook(AMX* amx, cell index, cell* result, const cell* params)
{
	PawnBridge* self = instance_;
	ScriptState* state = self ? self->find(amx) : nullptr;
	AMX_CALLBACK original = state ? state->original : (self ? self->fallbackOriginal_ : nullptr);
	if (!original)
	{
		return AMX_ERR_CALLBACK;
	}
	if (!state || !self->profileNatives_)
	{
		return original(amx, index, result, params);
	}
	const ZoneId zone = self->resolveNative(*state, index);
	if (zone == kPassthrough)
	{
		return original(amx, index, result, params);
	}
	Profiler& profiler = self->engine_.profiler();
	const std::size_t token = profiler.begin(zone);
	const int err = original(amx, index, result, params);
	const Nanos took = profiler.end(token);
	const Nanos slow = profiler.slowThreshold();
	if (slow != 0 && took > slow)
	{
		self->engine_.noteSlowCall(zone, took);
	}
	return err;
}

void PawnBridge::collectMetrics(std::vector<Metric>& out) const
{
	for (const auto& s : scripts_)
	{
		if (!s->script || !s->amx)
		{
			continue;
		}
		const std::string prefix = "amx.script" + std::to_string(s->id);
		const cell heap = s->script->GetHEA() - s->script->GetHLW();
		const cell stack = s->script->GetSTP() - s->script->GetSTK();
		out.push_back(Metric { prefix + ".heap_bytes", static_cast<std::int64_t>(heap), MetricKind::Count });
		out.push_back(Metric { prefix + ".stack_bytes", static_cast<std::int64_t>(stack), MetricKind::Count });
	}
}

} // namespace memguard
