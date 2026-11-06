#include "stdafx.h"
#include "ps5_config_database.h"

#include <algorithm>
#include <iterator>

namespace
{
	struct database_entry
	{
		std::string_view serial;
		std::string_view config;
	};

	// Sorted by serial (config_database.py)
	constexpr database_entry c_database[]
	{
#include "ps5_config_database.inc"
	};
}

std::string_view ps5_config_database_entry(std::string_view serial)
{
	const auto it = std::lower_bound(std::begin(c_database), std::end(c_database), serial, [](const database_entry& entry, std::string_view key)
	{
		return entry.serial < key;
	});
	return it != std::end(c_database) && it->serial == serial ? it->config : std::string_view{};
}
