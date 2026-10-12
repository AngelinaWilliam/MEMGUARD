// MemGuard X - Pawn integration.
//
// Native-call profiling: every Pawn native call goes through AMX::callback (SYSREQ.C/N
// opcodes call amx->callback). On script load we save the existing callback and install
// our own, which looks up (and caches) the native's name, times the call with the
// profiler and forwards to the saved callback. Natives whose callback was bypassed by the
// runtime (e.g. SYSREQ.D direct calls, if a runtime patched them in) would not be seen;
// the runtime verification in docs/VERIFICATION.md checks call counts against a known loop.
//
// The bridge also registers the MemGuard_* natives declared in pawn/memguard.inc.
#pragma once

#include <sdk.hpp>
#include <Server/Components/Pawn/pawn.hpp>

#include "../core/engine.hpp"

#include <memory>
#include <vector>

namespace memguard {

class PawnBridge
{
public:
	explicit PawnBridge(Engine& engine);
	~PawnBridge();
	PawnBridge(const PawnBridge&) = delete;
	PawnBridge& operator=(const PawnBridge&) = delete;

	void setPawnComponent(IPawnComponent* pawn) { pawn_ = pawn; }

	/// Called from PawnEventHandler::onAmxLoad / onAmxUnload.
	void attach(IPawnScript& script);
	void detach(IPawnScript& script);

	/// Restores the original callbacks of every attached script (component shutdown).
	void detachAll();

	/// Re-installs the hook if something replaced AMX::callback after we attached.
	/// Returns the number of scripts that had to be re-hooked.
	std::size_t verifyHooks();

	void setNativeProfiling(bool enabled) { profileNatives_ = enabled; }

	/// Adds per-script AMX heap/stack usage to a snapshot.
	void collectMetrics(std::vector<Metric>& out) const;

	std::size_t attachedScripts() const { return scripts_.size(); }
	std::uint64_t rehookCount() const { return rehooks_; }

private:
	friend struct PawnBridgeAccess;

	static constexpr ZoneId kUnresolved = static_cast<ZoneId>(-2);
	static constexpr ZoneId kPassthrough = static_cast<ZoneId>(-3);

	struct ScriptState
	{
		AMX* amx = nullptr;
		IPawnScript* script = nullptr;
		AMX_CALLBACK original = nullptr;
		int id = 0;
		std::vector<ZoneId> nativeZones;
	};

	static int AMXAPI callbackHook(AMX* amx, cell index, cell* result, const cell* params);
	ScriptState* find(AMX* amx);
	ZoneId resolveNative(ScriptState& state, cell index);
	void registerNatives(IPawnScript& script);

	Engine& engine_;
	IPawnComponent* pawn_ = nullptr;
	bool profileNatives_ = true;
	std::vector<std::unique_ptr<ScriptState>> scripts_;
	ScriptState* cached_ = nullptr;
	AMX_CALLBACK fallbackOriginal_ = nullptr;
	std::uint64_t rehooks_ = 0;

	static PawnBridge* instance_;
};

} // namespace memguard
