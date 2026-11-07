// Finding the cartridge image and checking that it is the release this program
// is built for (the one in config/rom.json): the file itself or a zip archive that
// holds it, identified by its size and CRC-32.
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace cartridge
{

constexpr size_t SIZE = 0x200000;
constexpr uint32_t CRC32 = 0x8bd22145;

// What the cartridge file is called, and the zip archive that holds it.
constexpr const char *FILE_NAME = "Sonic The Hedgehog - Pocket Adventure (World) (En,Ja).ngc";
constexpr const char *ZIP_NAME = "Sonic The Hedgehog - Pocket Adventure (World) (En,Ja).zip";

uint32_t crc32(const uint8_t *data, size_t size, uint32_t crc = 0);

// Whether `image` is the expected cartridge: its size and its CRC-32.
bool is_cartridge(const std::vector<uint8_t> &image);

enum class Status
{
	Ok,        // `image` is the cartridge
	Missing,   // there is no such file (or it cannot be read)
	Wrong,     // it is a file, but not the cartridge
};

// Reads `path`, which is the cartridge image or a zip archive holding it (whatever
// the archive's entry is called), into `image`. `problem` says what is wrong
// when the result is not Ok.
Status read(const std::filesystem::path &path, std::vector<uint8_t> &image, std::string &problem);

// Looks in each folder, in order, for FILE_NAME and then for ZIP_NAME (or FILE_NAME with
// ".zip" after it) and reads the first that is the cartridge: returns its path, or an empty
// path if there is none. `problem` collects what is wrong with files of those names that
// are there but are not the cartridge.
std::filesystem::path find(const std::vector<std::filesystem::path> &folders, std::vector<uint8_t> &image, std::string &problem);

// Paths as UTF-8 text (for files and messages) and back.
std::string to_utf8(const std::filesystem::path &path);
std::filesystem::path from_utf8(const std::string &text);

// Decodes a raw DEFLATE stream (as a zip archive holds it) that must fill exactly `out_size`
// bytes of `out`. False if the stream is damaged, or ends short or long. Exposed for testing.
bool inflate(const uint8_t *in, size_t in_size, uint8_t *out, size_t out_size);

// Reads the entry of a zip archive in memory that is the cartridge (by size and CRC-32).
// The archive's own checks are made too. Exposed for testing; `read` is what to use.
bool extract_from_zip(const std::vector<uint8_t> &archive, std::vector<uint8_t> &image, std::string &problem);

} // namespace cartridge
