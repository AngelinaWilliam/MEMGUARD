// MemGuard X - open.mp pool entity tracking.
// Each watcher listens to one pool's creation/destruction events and mirrors them into the
// MemoryTracker under the category "pool.<name>". Entity sizes are not observable through
// the SDK, so these categories report counts only.
#pragma once

#include <sdk.hpp>

#include "../core/engine.hpp"

namespace memguard {

template <typename T>
class PoolWatcher final : public PoolEventHandler<T>
{
public:
	PoolWatcher(Engine& engine, const char* name)
		: engine_(engine)
		, category_(engine.tracker().category(std::string("pool.") + name, false))
	{
	}

	~PoolWatcher() { detach(); }

	void attach(IEventDispatcher<PoolEventHandler<T>>& dispatcher, IComponent* owner)
	{
		detach();
		closed_ = false;
		if (dispatcher.addEventHandler(this))
		{
			dispatcher_ = &dispatcher;
			owner_ = owner;
		}
	}

	void detach()
	{
		if (dispatcher_)
		{
			dispatcher_->removeEventHandler(this);
			dispatcher_ = nullptr;
		}
		owner_ = nullptr;
	}

	/// The owning component is being freed: its dispatcher becomes invalid.
	void onComponentFree(IComponent* component)
	{
		if (owner_ && component == owner_)
		{
			// The pool is going away. Entities it destroys from now on may or may not raise
			// events; either way they are no evidence of a tracking error, so stop recording.
			dispatcher_ = nullptr;
			owner_ = nullptr;
			closed_ = true;
			engine_.tracker().forgetActive(category_);
		}
	}

	bool attached() const { return dispatcher_ != nullptr; }

	void onPoolEntryCreated(T& entry) override
	{
		if (closed_)
		{
			return;
		}
		engine_.tracker().onAlloc(category_, static_cast<std::uint64_t>(static_cast<std::uint32_t>(entry.getID())), 0);
	}

	void onPoolEntryDestroyed(T& entry) override
	{
		if (closed_)
		{
			return;
		}
		const FreeResult r = engine_.tracker().onFree(category_, static_cast<std::uint64_t>(static_cast<std::uint32_t>(entry.getID())));
		if (r == FreeResult::UnknownFree || r == FreeResult::DoubleFree)
		{
			// Entities that existed before MemGuard X attached are expected to show up here once.
			engine_.logger().logLimited(LogLevel::Debug, "pool-untracked:" + engine_.tracker().categoryName(category_),
				"[MemGuardX] Destroyed " + engine_.tracker().categoryName(category_) + " id " + std::to_string(entry.getID()) + " that was not tracked as active (" + freeResultName(r) + ")");
		}
	}

private:
	Engine& engine_;
	CategoryId category_;
	IEventDispatcher<PoolEventHandler<T>>* dispatcher_ = nullptr;
	IComponent* owner_ = nullptr;
	bool closed_ = false;
};

} // namespace memguard
