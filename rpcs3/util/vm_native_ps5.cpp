// PS5: utils' virtual memory on the console (util/vm.hpp), in place of
// vm_native.cpp's POSIX backend.
//
// The console's measurements (PS5_PayloadSDK, platform/docs/PROBE.md): pages
// are 16 KiB; plain anonymous mappings are charged to the title's small
// flexible budget; ranges the size of RPCS3's guest layout (8, 12, 32 and
// 4 GiB) are granted from 0x10_0000_0000 up, exactly at the hint or not at all.
// The first RPCS3 title on the console stopped before main: memory_commit's
// mprotect on a range plain mmap had reserved failed (EINVAL).
//
// So everything goes through the platform layer (ps5platform/shm.h):
// reservations are its virtual ranges, committed memory its direct-memory
// units, and shared memory (the guest's RAM and its mirrors) its direct-memory
// objects, mapped into the ranges, charged to the direct pool only.

#include "stdafx.h"
#include "util/vm.hpp"
#include "util/asm.hpp"

#include <ps5platform/shm.h>

#include <sys/mman.h>
#include <errno.h>
#include <unistd.h>

#include <algorithm>
#include <mutex>
#include <vector>

LOG_CHANNEL(vm_log, "VM");

namespace utils
{
	namespace
	{
		// Measured: the console's page (PROBE.md, "Memory")
		constexpr long ps5_page_size = 0x4000;

		int ps5_protection(protection prot)
		{
			switch (prot)
			{
			case protection::rw: return PS5_SHM_READ | PS5_SHM_WRITE;
			case protection::ro: return PS5_SHM_READ;
			case protection::no: return 0;
			case protection::wx: return PS5_SHM_READ | PS5_SHM_WRITE | PS5_SHM_EXEC;
			case protection::rx: return PS5_SHM_READ | PS5_SHM_EXEC;
			}
			return 0;
		}

		int posix_protection(protection prot)
		{
			const int p = ps5_protection(prot);
			return (p & PS5_SHM_READ ? PROT_READ : 0) | (p & PS5_SHM_WRITE ? PROT_WRITE : 0) | (p & PS5_SHM_EXEC ? PROT_EXEC : 0);
		}

		// The ranges memory_reserve gave out, so memory_protect knows reserved space
		// from mapped memory
		struct reserved_range
		{
			u64 base;
			u64 size;
		};

		std::mutex g_ranges_lock;
		std::vector<reserved_range> g_ranges;

		bool is_reserved(u64 addr)
		{
			std::lock_guard lock(g_ranges_lock);
			return std::any_of(g_ranges.begin(), g_ranges.end(), [&](const reserved_range& r)
			{
				return addr >= r.base && addr - r.base < r.size;
			});
		}

		// The range rounded out to whole pages
		std::pair<void*, usz> page_span(void* pointer, usz size)
		{
			const u64 begin = reinterpret_cast<u64>(pointer) & -ps5_page_size;
			const u64 end = utils::align<u64>(reinterpret_cast<u64>(pointer) + size, ps5_page_size);
			return {reinterpret_cast<void*>(begin), end - begin};
		}
	}

	long get_page_size()
	{
		return ps5_page_size;
	}

	void* memory_reserve(usz size, void* use_addr, [[maybe_unused]] bool is_memory_mapping, [[maybe_unused]] bool can_be_jit)
	{
		void* base = nullptr;

		if (use_addr)
		{
			// Exactly at the address, or refused: callers search upward themselves
			if (reinterpret_cast<uptr>(use_addr) % 0x10000 || ps5_vrange_reserve_at(use_addr, size) != 0)
			{
				return nullptr;
			}
			base = use_addr;
		}
		else if (ps5_vrange_reserve(size, nullptr, 0x10000, &base) != 0)
		{
			return nullptr;
		}

		std::lock_guard lock(g_ranges_lock);
		g_ranges.push_back({reinterpret_cast<u64>(base), size});
		return base;
	}

	void memory_commit(void* pointer, usz size, protection prot)
	{
		if (!size)
		{
			return;
		}

		const int result = ps5_vrange_commit(pointer, size, ps5_protection(prot));
		if (result != 0)
		{
			fmt::throw_exception("memory_commit(%p, 0x%x, %d) failed: 0x%x", pointer, size, static_cast<int>(prot), static_cast<u32>(result));
		}
	}

	void memory_decommit(void* pointer, usz size, bool /*can_be_jit*/)
	{
		if (!size)
		{
			return;
		}

		const int result = ps5_vrange_decommit(pointer, size);
		if (result != 0)
		{
			fmt::throw_exception("memory_decommit(%p, 0x%x) failed: 0x%x", pointer, size, static_cast<u32>(result));
		}
	}

	void memory_reset(void* pointer, usz size, protection prot, bool can_be_jit)
	{
		// Decommitted units come back zeroed at their next commit
		memory_decommit(pointer, size, can_be_jit);
		memory_commit(pointer, size, prot);
	}

	void memory_release(void* pointer, usz size)
	{
		if (!size)
		{
			return;
		}

		ensure(ps5_vrange_release(pointer, size) == 0);

		std::lock_guard lock(g_ranges_lock);
		std::erase_if(g_ranges, [&](const reserved_range& r) { return r.base == reinterpret_cast<u64>(pointer); });
	}

	void memory_protect(void* pointer, usz size, protection prot)
	{
		if (!size)
		{
			return;
		}

		// Mapped memory (committed units, shared-memory views) changes protection
		// in place; reserved space that nothing backs yet is committed with it
		const auto [page, bytes] = page_span(pointer, size);
		if (::mprotect(page, bytes, posix_protection(prot)) == 0)
		{
			return;
		}

		const int error = errno;
		if (is_reserved(reinterpret_cast<u64>(pointer)))
		{
			memory_commit(pointer, size, prot);
			return;
		}

		fmt::throw_exception("memory_protect(%p, 0x%x, %d) failed (errno=%d)", pointer, size, static_cast<int>(prot), error);
	}

	bool memory_lock(void* pointer, usz size)
	{
		return !size || !::mlock(pointer, size);
	}

	void* memory_map_fd(native_handle fd, usz size, protection prot)
	{
		const auto result = ::mmap(nullptr, size, posix_protection(prot), MAP_SHARED, fd, 0);
		return result == MAP_FAILED ? nullptr : result;
	}

	shm::shm(u64 size, u32 flags)
		: m_flags(flags)
		, m_size(utils::align(size, 0x10000))
	{
		ps5_shm object{};
		const int result = ps5_shm_create(m_size, &object);
		if (result != 0)
		{
			fmt::throw_exception("ps5_shm_create(0x%x) failed: 0x%x", m_size, static_cast<u32>(result));
		}
		m_direct_start = object.direct_start;
		m_direct_bytes = object.bytes;
	}

	shm::shm(u64 size, const std::string& storage)
		: shm(size, 0)
	{
		// No sparse file on the console: the storage is direct memory as well
		m_storage = storage;
	}

	shm::~shm()
	{
		this->unmap_self();

		ps5_shm object{m_direct_start, static_cast<size_t>(m_direct_bytes)};
		ps5_shm_destroy(&object);
	}

	u8* shm::map(void* ptr, protection prot, bool cow) const
	{
		if (cow)
		{
			// One direct-memory object has no private copies of its pages
			vm_log.error("shm::map: copy-on-write mappings are not supported on the PS5");
			return nullptr;
		}

		const ps5_shm object{m_direct_start, static_cast<size_t>(m_direct_bytes)};
		void* const target = reinterpret_cast<void*>(reinterpret_cast<u64>(ptr) & -0x10000);
		void* view = nullptr;

		// At an address: there, over whatever reservation is there; else anywhere
		const unsigned flags = target ? PS5_SHM_FIXED : 0;
		if (ps5_shm_map(&object, 0, m_size, target, ps5_protection(prot), flags, &view) != 0)
		{
			return nullptr;
		}

		return static_cast<u8*>(view);
	}

	u8* shm::try_map(void* ptr, protection prot, bool cow) const
	{
		return this->map(ensure(ptr), prot, cow);
	}

	std::pair<u8*, std::string> shm::map_critical(void* ptr, protection prot, bool cow)
	{
		if (const auto mapped = this->map(ptr, prot, cow))
		{
			return {mapped, {}};
		}

		return {nullptr, "ps5_shm_map failed"};
	}

	u8* shm::map_self(protection prot)
	{
		void* ptr = m_ptr;

		for (void* mapped = nullptr; !ptr;)
		{
			if (!mapped)
			{
				mapped = this->map(nullptr, prot);

				if (!mapped)
				{
					if ((ptr = m_ptr))
					{
						break;
					}

					return nullptr;
				}
			}

			// Install mapped memory
			if (m_ptr.compare_exchange(ptr, mapped))
			{
				ptr = mapped;
			}
			else if (ptr)
			{
				// Mapped already, nothing to do.
				ensure(ptr != mapped);
				this->unmap(mapped);
			}
		}

		return static_cast<u8*>(ptr);
	}

	void shm::unmap(void* ptr) const
	{
		ps5_shm_unmap(ptr, m_size, 0);
	}

	void shm::unmap_critical(void* ptr)
	{
		// The view goes; its place stays reserved, a hole in the guest's layout
		void* const target = reinterpret_cast<void*>(reinterpret_cast<u64>(ptr) & -0x10000);
		ps5_shm_unmap(target, m_size, PS5_SHM_KEEP_RESERVED);
	}

	void shm::unmap_self()
	{
		if (auto ptr = m_ptr.exchange(nullptr))
		{
			this->unmap(ptr);
		}
	}
}
