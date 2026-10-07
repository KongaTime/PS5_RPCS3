// PS5: which files are open, by name, across the whole title.
//
// A title holds about 249 files open by path at once (PS5_PayloadSDK's
// platform/docs/PROBE.md, "Open files"). GTA IV's boot ran out of them with
// about 200 open before the game itself had opened any (b302525), and only 3
// of those through RPCS3's fs: the rest are opened by something else linked
// in (RADV, LLVM, libc++, the platform layer). The title is linked with
// --wrap=open, --wrap=openat and --wrap=fopen (PS5_RPCS3Title's
// rpcs3/rpcs3.cmake), so every open by path made from the program passes
// here and is recorded with the file's device and inode; a record counts as
// open while its descriptor still names that file, so nothing need see the
// closes.

#include <algorithm>
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <map>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <vector>

extern "C"
{
	int __real_open(const char* path, int flags, ...);
	int __real_openat(int dir, const char* path, int flags, ...);
	FILE* __real_fopen(const char* path, const char* mode);
}

namespace
{
	constexpr int c_slots = 16384;

	struct record
	{
		dev_t dev = 0;
		ino_t ino = 0;
		bool used = false;
		char path[200]{};
	};

	std::mutex s_lock;
	record s_records[c_slots];

	void remember(int fd, const char* path)
	{
		if (fd < 0 || fd >= c_slots || !path)
		{
			return;
		}
		struct ::stat info;
		if (::fstat(fd, &info) != 0)
		{
			return;
		}
		std::lock_guard lock(s_lock);
		record& r = s_records[fd];
		r.dev = info.st_dev;
		r.ino = info.st_ino;
		r.used = true;
		std::strncpy(r.path, path, sizeof(r.path) - 1);
		r.path[sizeof(r.path) - 1] = 0;
	}
}

extern "C" int __wrap_open(const char* path, int flags, ...)
{
	int mode = 0;
	if (flags & O_CREAT)
	{
		va_list args;
		va_start(args, flags);
		mode = va_arg(args, int);
		va_end(args);
	}
	const int fd = __real_open(path, flags, mode);
	const int saved = errno;
	remember(fd, path);
	errno = saved;
	return fd;
}

extern "C" int __wrap_openat(int dir, const char* path, int flags, ...)
{
	int mode = 0;
	if (flags & O_CREAT)
	{
		va_list args;
		va_start(args, flags);
		mode = va_arg(args, int);
		va_end(args);
	}
	const int fd = __real_openat(dir, path, flags, mode);
	const int saved = errno;
	remember(fd, path);
	errno = saved;
	return fd;
}

extern "C" FILE* __wrap_fopen(const char* path, const char* mode)
{
	FILE* const file = __real_fopen(path, mode);
	const int saved = errno;
	if (file)
	{
		remember(::fileno(file), path);
	}
	errno = saved;
	return file;
}

// The files open now that were opened by name, by folder, the most first
std::string ps5_open_files_report()
{
	std::map<std::string, unsigned> by_folder;
	unsigned total = 0;
	{
		std::lock_guard lock(s_lock);
		for (int fd = 0; fd < c_slots; fd++)
		{
			record& r = s_records[fd];
			if (!r.used)
			{
				continue;
			}
			struct ::stat info;
			if (::fstat(fd, &info) != 0 || info.st_dev != r.dev || info.st_ino != r.ino)
			{
				r.used = false; // closed since, or the number reused by an unnamed open
				continue;
			}
			std::string folder = r.path;
			const auto slash = folder.find_last_of('/');
			folder = slash == std::string::npos ? std::string(".") : folder.substr(0, slash);
			by_folder[folder]++;
			total++;
		}
	}

	std::vector<std::pair<unsigned, std::string>> sorted;
	for (const auto& [folder, count] : by_folder)
	{
		sorted.emplace_back(count, folder);
	}
	std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.first > b.first; });

	std::string result = std::to_string(total) + " opened by name:";
	for (std::size_t i = 0; i < sorted.size() && i < 12; i++)
	{
		result += " [" + std::to_string(sorted[i].first) + " in " + sorted[i].second + "]";
	}
	return result;
}
