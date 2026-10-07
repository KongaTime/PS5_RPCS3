// PS5: what std::terminate reports on the console.
//
// RPCS3 is built without exceptions, but libraries it calls throw (libc++'s
// std::thread and std::system_error, the shader compiler's): one that reaches
// RPCS3's code ends in std::terminate, and RPCS3's own handler reports only
// "RPCS3 has abnormally terminated." (the Ratchet & Clank Collection's first
// boot, on my console, as its first shaders compiled). This one names the
// exception, its message, the thread and the frame pointers' backtrace. It is
// built with exceptions, the only unit here that is, to rethrow and catch
// the exception for its message.

#include "stdafx.h"

#include "Utilities/Thread.h"

#include <cxxabi.h>
#include <exception>
#include <typeinfo>

[[noreturn]] void report_fatal_error(std::string_view text, bool is_html = false, bool include_help_text = true);

namespace
{
	// The exception's message, where it is a std::exception
	std::string current_exception_what()
	{
		try
		{
			if (const std::exception_ptr exception = std::current_exception())
			{
				std::rethrow_exception(exception);
			}
		}
		catch (const std::exception& e)
		{
			return e.what();
		}
		catch (...)
		{
		}
		return {};
	}

	[[noreturn]] void ps5_terminate()
	{
		std::string msg = "RPCS3 has abnormally terminated (std::terminate)";

		if (const std::type_info* type = abi::__cxa_current_exception_type())
		{
			int status = 0;
			char* const name = abi::__cxa_demangle(type->name(), nullptr, nullptr, &status);
			fmt::append(msg, ": exception %s", status == 0 && name ? name : type->name());
			std::free(name);

			if (const std::string what = current_exception_what(); !what.empty())
			{
				fmt::append(msg, ", \"%s\"", what);
			}
		}
		else
		{
			msg += ": no exception (a joinable std::thread destroyed, or a noexcept function left by one)";
		}

		fmt::append(msg, "\nThread: '%s'\nBacktrace:", thread_ctrl::get_name());

		// The frame pointers' chain (the title is built with them), each frame a
		// little above the last on the stack; symbolised against llvm-pie.elf
		// as the fault handler's are (Utilities/Thread.cpp)
		u64 frame = reinterpret_cast<u64>(__builtin_frame_address(0));
		const u64 low = frame;
		for (u32 i = 0; i < 40 && frame >= low && frame - low < 0x1000000 && frame % 8 == 0; i++)
		{
			const u64* const words = reinterpret_cast<const u64*>(frame);
			fmt::append(msg, " %p", reinterpret_cast<void*>(words[1]));
			if (words[0] <= frame)
			{
				break;
			}
			frame = words[0];
		}

		report_fatal_error(msg);
	}
}

// Set by the frontend at its start, over RPCS3's own (Utilities/Thread.cpp)
void ps5_set_terminate_handler()
{
	std::set_terminate(ps5_terminate);
}
