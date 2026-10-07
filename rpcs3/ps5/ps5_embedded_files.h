#pragma once

// PS5: files built into the title (embed_files.py), written out by the
// frontend at its start: RPCS3's overlay images (bin/Icons/ui)

struct ps5_embedded_file
{
	const char* name; // its path below Icons/ui/
	const unsigned char* data;
	unsigned long size;
};

extern const ps5_embedded_file ps5_embedded_files[];
extern const unsigned ps5_embedded_file_count;
