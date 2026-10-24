#include "stdafx.h"
#include "nv406e.h"
#include "nv47_sync.hpp"

#include "Emu/RSX/RSXThread.h"
#include "Emu/system_config.h"

#include "context_accessors.define.h"

namespace rsx
{
#ifdef __PROSPERO__
	// PS5: the label a wait last timed out on, and the RSX's own writes to it
	// (nv47_sync.hpp): a write from the RSX tells its release from the game's
	// PPU or SPU (Army of Two: The 40th Day's label 254, build 174)
	atomic_t<u32> g_ps5_watched_label = 0;
	atomic_t<u32> g_ps5_watched_label_rsx_writes = 0;
	u32 g_ps5_watched_label_values[8]{};

	// The values the RSX last wrote to the watched label, oldest first
	static std::string ps5_watched_label_history()
	{
		const u32 count = g_ps5_watched_label_rsx_writes;
		std::string out;
		for (u32 i = count > 8 ? count - 8 : 0; i < count; i++)
		{
			fmt::append(out, "%s0x%x", out.empty() ? "" : " ", g_ps5_watched_label_values[i % 8]);
		}
		return out.empty() ? std::string("none") : out;
	}
#endif

	namespace nv406e
	{
		void set_reference(context* ctx, u32 /*reg*/, u32 arg)
		{
			RSX(ctx)->sync();

			// Write ref+get (get will be written again with the same value at command end)
			auto& dma = *vm::_ptr<RsxDmaControl>(RSX(ctx)->dma_address);
			//dma.get.store(RSX(ctx)->fifo_ctrl->get_pos() + 4);
			dma.ref.store(arg);
		}

		void semaphore_acquire(context* ctx, u32 /*reg*/, u32 arg)
		{
			RSX(ctx)->sync_point_request.release(true);
			const u32 addr = get_address(REGS(ctx)->semaphore_offset_406e(), REGS(ctx)->semaphore_context_dma_406e());

			// Syncronization point, may be associated with memory changes without actually changing addresses
			RSX(ctx)->m_graphics_state |= rsx::pipeline_state::fragment_program_needs_rehash;

			// Ensure atomic seq-cst memory ordering for FIFO GET update
			atomic_fence_seq_cst();

			const auto& sema = vm::_ref<RsxSemaphore>(addr);
			const auto& atomic_sema = vm::_ref<atomic_t<RsxSemaphore>>(addr);

			RSX(ctx)->last_sema_cmd = RSX(ctx)->fifo_ctrl->get_pos() - 4;
			RSX(ctx)->last_sema_addr = addr;

			if (sema == arg)
			{
				// Flip semaphore doesnt need wake-up delay
				if (addr != RSX(ctx)->label_addr + 0x10)
				{
					//RSX(ctx)->flush_fifo();
					RSX(ctx)->fifo_wake_delay(2);
				}

				return;
			}
			else
			{
				//RSX(ctx)->flush_fifo();
			}

			u64 start = get_system_time();
			u64 last_check_val = start;
#ifdef __PROSPERO__
			const u32 sema_at_start = sema;
			u32 sema_changes = 0;
			u32 sema_last = sema_at_start;
#endif

			while (sema != arg)
			{
				if (RSX(ctx)->test_stopped())
				{
					RSX(ctx)->state += cpu_flag::again;
					return;
				}

				if (const auto tdr = static_cast<u64>(g_cfg.video.driver_recovery_timeout))
				{
					const u64 current = get_system_time();

					if (current - last_check_val > 20'000)
					{
						// Suspicious amnount of time has passed
						// External pause such as debuggers' pause or operating system sleep may have taken place
						// Ignore it
						start += current - last_check_val;
					}

					last_check_val = current;

					if ((current - start) > tdr)
					{
						// If longer than driver timeout force exit
#ifdef __PROSPERO__
						rsx_log.error("nv406e::semaphore_acquire has timed out. semaphore_address=0x%X (label %u; waited for 0x%x, held 0x%x at the start, 0x%x now, changed %u times; the RSX wrote it %u times since the last: %s)",
							addr, (addr - RSX(ctx)->label_addr) / 0x10, arg, sema_at_start, u32{sema}, sema_changes, u32{g_ps5_watched_label_rsx_writes}, ps5_watched_label_history());
						g_ps5_watched_label_rsx_writes = 0;
						g_ps5_watched_label = addr;
#else
						rsx_log.error("nv406e::semaphore_acquire has timed out. semaphore_address=0x%X", addr);
#endif
						break;
					}
				}

				if (RSX(ctx)->external_interrupt_lock ||
					(RSX(ctx)->state & (cpu_flag::dbg_global_pause + cpu_flag::exit)) == cpu_flag::dbg_global_pause)
				{
					RSX(ctx)->cpu_wait({});
					continue;
				}

				RSX(ctx)->on_semaphore_acquire_wait();
#ifdef __PROSPERO__
				if (const u32 now = sema; now != sema_last)
				{
					sema_changes++;
					sema_last = now;
				}
#endif

				// Wait until the value changes or until 100us pass.
				utils::spin_on_cacheline_once(atomic_sema, sema, 100);
			}

			RSX(ctx)->fifo_wake_delay();
#ifdef __PROSPERO__
			// Waits long enough to be seen as a stutter (longer than a frame at 10 fps:
			// a game's own 30 fps pacing waits 33 ms each frame), and what released them
			if (const u64 waited = get_system_time() - start; waited > 100'000 && sema == arg)
			{
				rsx_log.warning("PS5: semaphore_acquire at 0x%X (label %u) waited %u ms for 0x%x, from 0x%x; the RSX wrote it %u times since the last: %s",
					addr, (addr - RSX(ctx)->label_addr) / 0x10, waited / 1000, arg, sema_at_start, u32{g_ps5_watched_label_rsx_writes}, ps5_watched_label_history());
				g_ps5_watched_label_rsx_writes = 0;
				g_ps5_watched_label = addr;
			}
#endif
			RSX(ctx)->performance_counters.idle_time += (get_system_time() - start);
		}

		void semaphore_release(context* ctx, u32 reg, u32 arg)
		{
			const u32 offset = REGS(ctx)->semaphore_offset_406e();

			if (offset % 4)
			{
				rsx_log.warning("NV406E semaphore release is using unaligned semaphore, ignoring. (offset=0x%x)", offset);
				return;
			}

			const u32 ctxt = REGS(ctx)->semaphore_context_dma_406e();

			// By avoiding doing this on flip's semaphore release
			// We allow last gcm's registers reset to occur in case of a crash
			if (const bool is_flip_sema = (offset == 0x10 && ctxt == CELL_GCM_CONTEXT_DMA_SEMAPHORE_R);
				!is_flip_sema)
			{
				RSX(ctx)->sync_point_request.release(true);
			}

			const u32 addr = get_address(offset, ctxt);

			// TODO: Check if possible to write on reservations
			if (RSX(ctx)->label_addr >> 28 != addr >> 28)
			{
				rsx_log.error("NV406E semaphore unexpected address. Please report to the developers. (offset=0x%x, addr=0x%x)", offset, addr);
				RSX(ctx)->recover_fifo();
				return;
			}

			if (addr == RSX(ctx)->device_addr + 0x30 && !arg)
			{
				// HW flip synchronization related, 1 is not written without display queue command (TODO: make it behave as real hw)
				arg = 1;
			}

			util::write_gcm_label<false, true>(ctx, reg, addr, arg);
		}
	}
}
