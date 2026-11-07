#include "common/cartridge.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <fstream>

namespace fs = std::filesystem;

namespace cartridge
{

namespace
{

// Files larger than this are not read: the cartridge is 2 MiB and so is a zip holding it.
constexpr size_t MAX_FILE_SIZE = 64u << 20;

struct CrcTable
{
	std::array<uint32_t, 256> value{};
	constexpr CrcTable()
	{
		for (uint32_t i = 0; i < 256; ++i)
		{
			uint32_t c = i;
			for (int bit = 0; bit < 8; ++bit)
				c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
			value[i] = c;
		}
	}
};
constexpr CrcTable CRC_TABLE;

std::string hex32(uint32_t value)
{
	char text[16];
	std::snprintf(text, sizeof text, "%08x", static_cast<unsigned>(value));
	return text;
}

// The expected file, as words for a message.
std::string expectation()
{
	return std::to_string(SIZE) + " bytes with CRC-32 " + hex32(CRC32);
}

// Raw DEFLATE (RFC 1951), after Mark Adler's puff: the whole stream is decoded into a
// buffer of exactly the size the archive promises, and anything that does not fit is
// an error. Every read of the input and every write of the output is checked.
class Inflater
{
public:
	Inflater(const uint8_t *in, size_t in_size, uint8_t *out, size_t out_size) :
		m_in(in), m_in_size(in_size), m_out(out), m_out_size(out_size)
	{
	}

	// True if the stream decodes and fills the output exactly.
	bool run()
	{
		try
		{
			bool last;
			do
			{
				last = bits(1) != 0;
				switch (bits(2))
				{
					case 0: stored(); break;
					case 1: fixed(); break;
					case 2: dynamic(); break;
					default: throw Error();
				}
			} while (!last);
		}
		catch (const Error &)
		{
			return false;
		}
		return m_out_pos == m_out_size;
	}

private:
	struct Error {};

	static constexpr int MAX_BITS = 15;
	static constexpr int MAX_LENGTH_CODES = 286;
	static constexpr int MAX_DISTANCE_CODES = 30;
	static constexpr int FIXED_LENGTH_CODES = 288;

	struct Huffman
	{
		uint16_t count[MAX_BITS + 1];   // codes of each length
		uint16_t symbol[FIXED_LENGTH_CODES];   // symbols, by code
	};

	uint32_t bits(int need)
	{
		uint32_t value = m_bit_buffer;
		while (m_bit_count < need)
		{
			if (m_in_pos >= m_in_size)
				throw Error();
			value |= static_cast<uint32_t>(m_in[m_in_pos++]) << m_bit_count;
			m_bit_count += 8;
		}
		m_bit_buffer = value >> need;
		m_bit_count -= need;
		return value & ((1u << need) - 1);
	}

	void put(uint8_t byte)
	{
		if (m_out_pos >= m_out_size)
			throw Error();
		m_out[m_out_pos++] = byte;
	}

	// Builds the decoding tables for codes of the given lengths. Returns 0 for a complete
	// code, positive for an incomplete one and negative for one with too many codes.
	static int construct(Huffman &h, const uint16_t *length, int n)
	{
		for (int len = 0; len <= MAX_BITS; ++len)
			h.count[len] = 0;
		for (int symbol = 0; symbol < n; ++symbol)
			++h.count[length[symbol]];
		if (h.count[0] == n)
			return 0;   // no codes: complete, though it decodes nothing
		int left = 1;
		for (int len = 1; len <= MAX_BITS; ++len)
		{
			left <<= 1;
			left -= h.count[len];
			if (left < 0)
				return left;
		}
		uint16_t offset[MAX_BITS + 1];
		offset[1] = 0;
		for (int len = 1; len < MAX_BITS; ++len)
			offset[len + 1] = static_cast<uint16_t>(offset[len] + h.count[len]);
		for (int symbol = 0; symbol < n; ++symbol)
			if (length[symbol] != 0)
				h.symbol[offset[length[symbol]]++] = static_cast<uint16_t>(symbol);
		return left;
	}

	int decode(const Huffman &h)
	{
		int code = 0, first = 0, index = 0;
		for (int len = 1; len <= MAX_BITS; ++len)
		{
			code |= static_cast<int>(bits(1));
			const int count = h.count[len];
			if (code - count < first)
				return h.symbol[index + (code - first)];
			index += count;
			first += count;
			first <<= 1;
			code <<= 1;
		}
		throw Error();
	}

	void codes(const Huffman &lengths_code, const Huffman &distances_code)
	{
		static const uint16_t length_base[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
				35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
		static const uint8_t length_extra[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
				3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
		static const uint16_t distance_base[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
				257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577 };
		static const uint8_t distance_extra[30] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
				7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };
		for (;;)
		{
			int symbol = decode(lengths_code);
			if (symbol < 256)
			{
				put(static_cast<uint8_t>(symbol));
				continue;
			}
			if (symbol == 256)
				return;
			symbol -= 257;
			if (symbol >= 29)
				throw Error();
			const size_t length = length_base[symbol] + bits(length_extra[symbol]);
			symbol = decode(distances_code);
			if (symbol >= 30)
				throw Error();
			const size_t distance = distance_base[symbol] + bits(distance_extra[symbol]);
			if (distance > m_out_pos || length > m_out_size - m_out_pos)
				throw Error();
			for (size_t i = 0; i < length; ++i, ++m_out_pos)
				m_out[m_out_pos] = m_out[m_out_pos - distance];
		}
	}

	void stored()
	{
		m_bit_buffer = 0;   // the rest of the byte is skipped
		m_bit_count = 0;
		if (m_in_size - m_in_pos < 4)
			throw Error();
		const uint32_t length = m_in[m_in_pos] | (m_in[m_in_pos + 1] << 8);
		const uint32_t inverse = m_in[m_in_pos + 2] | (m_in[m_in_pos + 3] << 8);
		m_in_pos += 4;
		if (length != (~inverse & 0xFFFF) || length > m_in_size - m_in_pos || length > m_out_size - m_out_pos)
			throw Error();
		for (uint32_t i = 0; i < length; ++i)
			m_out[m_out_pos++] = m_in[m_in_pos++];
	}

	void fixed()
	{
		Huffman lengths_code, distances_code;
		uint16_t lengths[FIXED_LENGTH_CODES];
		int symbol = 0;
		for (; symbol < 144; ++symbol) lengths[symbol] = 8;
		for (; symbol < 256; ++symbol) lengths[symbol] = 9;
		for (; symbol < 280; ++symbol) lengths[symbol] = 7;
		for (; symbol < FIXED_LENGTH_CODES; ++symbol) lengths[symbol] = 8;
		construct(lengths_code, lengths, FIXED_LENGTH_CODES);
		for (symbol = 0; symbol < MAX_DISTANCE_CODES; ++symbol) lengths[symbol] = 5;
		construct(distances_code, lengths, MAX_DISTANCE_CODES);
		codes(lengths_code, distances_code);
	}

	void dynamic()
	{
		static const uint8_t order[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };
		const int nlen = static_cast<int>(bits(5)) + 257;
		const int ndist = static_cast<int>(bits(5)) + 1;
		const int ncode = static_cast<int>(bits(4)) + 4;
		if (nlen > MAX_LENGTH_CODES || ndist > MAX_DISTANCE_CODES)
			throw Error();

		uint16_t lengths[MAX_LENGTH_CODES + MAX_DISTANCE_CODES];
		int index = 0;
		for (; index < ncode; ++index)
			lengths[order[index]] = static_cast<uint16_t>(bits(3));
		for (; index < 19; ++index)
			lengths[order[index]] = 0;
		Huffman lengths_code, distances_code;
		if (construct(lengths_code, lengths, 19) != 0)
			throw Error();

		index = 0;
		while (index < nlen + ndist)
		{
			int symbol = decode(lengths_code);
			if (symbol < 16)
			{
				lengths[index++] = static_cast<uint16_t>(symbol);
				continue;
			}
			uint16_t repeated = 0;
			if (symbol == 16)
			{
				if (index == 0)
					throw Error();
				repeated = lengths[index - 1];
				symbol = 3 + static_cast<int>(bits(2));
			}
			else if (symbol == 17)
				symbol = 3 + static_cast<int>(bits(3));
			else
				symbol = 11 + static_cast<int>(bits(7));
			if (index + symbol > nlen + ndist)
				throw Error();
			while (symbol--)
				lengths[index++] = repeated;
		}
		if (lengths[256] == 0)
			throw Error();   // no end-of-block code

		// An incomplete code is allowed only for a single code (one symbol of one bit).
		int result = construct(lengths_code, lengths, nlen);
		if (result < 0 || (result > 0 && nlen != lengths_code.count[0] + lengths_code.count[1]))
			throw Error();
		result = construct(distances_code, lengths + nlen, ndist);
		if (result < 0 || (result > 0 && ndist != distances_code.count[0] + distances_code.count[1]))
			throw Error();
		codes(lengths_code, distances_code);
	}

	const uint8_t *m_in;
	size_t m_in_size, m_in_pos = 0;
	uint8_t *m_out;
	size_t m_out_size, m_out_pos = 0;
	uint32_t m_bit_buffer = 0;
	int m_bit_count = 0;
};

// Little-endian fields of a zip archive, which are read only where the archive has bytes.
class Archive
{
public:
	explicit Archive(const std::vector<uint8_t> &bytes) : m_bytes(bytes) {}

	size_t size() const { return m_bytes.size(); }
	bool has(size_t at, size_t length) const { return at <= m_bytes.size() && length <= m_bytes.size() - at; }
	uint32_t u16(size_t at) const { return m_bytes[at] | (m_bytes[at + 1] << 8); }
	uint32_t u32(size_t at) const { return u16(at) | (u16(at + 2) << 16); }
	const uint8_t *data(size_t at) const { return m_bytes.data() + at; }

private:
	const std::vector<uint8_t> &m_bytes;
};

constexpr uint32_t END_OF_DIRECTORY = 0x06054b50;
constexpr uint32_t DIRECTORY_ENTRY = 0x02014b50;
constexpr uint32_t LOCAL_HEADER = 0x04034b50;

} // namespace

uint32_t crc32(const uint8_t *data, size_t size, uint32_t crc)
{
	crc = ~crc;
	for (size_t i = 0; i < size; ++i)
		crc = CRC_TABLE.value[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
	return ~crc;
}

bool is_cartridge(const std::vector<uint8_t> &image)
{
	return image.size() == SIZE && crc32(image.data(), image.size()) == CRC32;
}

std::string to_utf8(const fs::path &path)
{
	const std::u8string text = path.u8string();
	return std::string(text.begin(), text.end());
}

fs::path from_utf8(const std::string &text)
{
	std::u8string converted;
	converted.reserve(text.size());
	for (char c : text)
		converted.push_back(static_cast<char8_t>(static_cast<unsigned char>(c)));
	return fs::path(converted);
}

bool inflate(const uint8_t *in, size_t in_size, uint8_t *out, size_t out_size)
{
	return Inflater(in, in_size, out, out_size).run();
}

bool extract_from_zip(const std::vector<uint8_t> &bytes, std::vector<uint8_t> &image, std::string &problem)
{
	const Archive archive(bytes);
	constexpr size_t END_SIZE = 22;
	if (archive.size() < END_SIZE)
	{
		problem = "it is not a zip archive, or it is cut short";
		return false;
	}

	// The end-of-directory record is the last thing in the file, before a comment of up to 64 KiB.
	const size_t lowest = archive.size() - END_SIZE > 0xFFFF ? archive.size() - END_SIZE - 0xFFFF : 0;
	size_t end = archive.size();
	for (size_t at = archive.size() - END_SIZE;; --at)
	{
		if (archive.u32(at) == END_OF_DIRECTORY)
		{
			end = at;
			break;
		}
		if (at == lowest)
			break;
	}
	if (end == archive.size())
	{
		problem = "it is not a zip archive, or it is cut short";
		return false;
	}
	const size_t entries = archive.u16(end + 10);
	const size_t directory_size = archive.u32(end + 12);
	const size_t directory = archive.u32(end + 16);
	if (!archive.has(directory, directory_size))
	{
		problem = "the zip archive is damaged";
		return false;
	}

	bool damaged = false;
	size_t at = directory;
	for (size_t entry = 0; entry < entries; ++entry)
	{
		if (!archive.has(at, 46) || archive.u32(at) != DIRECTORY_ENTRY)
		{
			damaged = true;
			break;
		}
		const unsigned flags = archive.u16(at + 8), method = archive.u16(at + 10);
		const uint32_t crc = archive.u32(at + 16);
		const size_t packed = archive.u32(at + 20), unpacked = archive.u32(at + 24);
		const size_t local = archive.u32(at + 42);
		at += 46 + archive.u16(at + 28) + archive.u16(at + 30) + archive.u16(at + 32);

		// Only an entry that claims to be the cartridge is read, and is checked again when it is.
		if (unpacked != SIZE || crc != CRC32 || (flags & 1) || (method != 0 && method != 8))
			continue;
		if (!archive.has(local, 30) || archive.u32(local) != LOCAL_HEADER)
		{
			damaged = true;
			continue;
		}
		const size_t start = local + 30 + archive.u16(local + 26) + archive.u16(local + 28);
		if (!archive.has(start, packed))
		{
			damaged = true;
			continue;
		}
		std::vector<uint8_t> candidate(SIZE);
		bool unpacked_ok;
		if (method == 0)
		{
			unpacked_ok = packed == SIZE;
			if (unpacked_ok)
				std::copy(archive.data(start), archive.data(start) + packed, candidate.begin());
		}
		else
			unpacked_ok = inflate(archive.data(start), packed, candidate.data(), candidate.size());
		if (unpacked_ok && is_cartridge(candidate))
		{
			image = std::move(candidate);
			return true;
		}
		damaged = true;
	}
	problem = damaged ? "the zip archive is damaged"
			: "none of the files in the archive is the cartridge, which is " + expectation();
	return false;
}

Status read(const fs::path &path, std::vector<uint8_t> &image, std::string &problem)
{
	const std::string name = to_utf8(path);
	std::error_code error;
	if (!fs::is_regular_file(path, error))
	{
		problem = name + " is not a file";
		return Status::Missing;
	}
	const uintmax_t size = fs::file_size(path, error);
	std::ifstream file(path, std::ios::binary);
	if (error || !file)
	{
		problem = "cannot read " + name;
		return Status::Missing;
	}
	if (size > MAX_FILE_SIZE)
	{
		problem = name + " is not the cartridge: it is " + std::to_string(size) + " bytes, and the cartridge is " + expectation();
		return Status::Wrong;
	}
	std::vector<uint8_t> bytes(static_cast<size_t>(size));
	file.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
	if (static_cast<size_t>(file.gcount()) != bytes.size())
	{
		problem = "cannot read " + name;
		return Status::Missing;
	}

	if (is_cartridge(bytes))
	{
		image = std::move(bytes);
		return Status::Ok;
	}
	if (bytes.size() >= 4 && bytes[0] == 'P' && bytes[1] == 'K')
	{
		std::string why;
		if (extract_from_zip(bytes, image, why))
			return Status::Ok;
		problem = name + ": " + why;
		return Status::Wrong;
	}
	problem = name + " is not the cartridge: it is " + std::to_string(bytes.size()) + " bytes with CRC-32 " +
			hex32(crc32(bytes.data(), bytes.size())) + ", and the cartridge is " + expectation();
	return Status::Wrong;
}

fs::path find(const std::vector<fs::path> &folders, std::vector<uint8_t> &image, std::string &problem)
{
	const std::string names[] = { FILE_NAME, ZIP_NAME, std::string(FILE_NAME) + ".zip" };
	for (const fs::path &folder : folders)
		for (const std::string &name : names)
		{
			const fs::path path = (folder / from_utf8(name)).lexically_normal();
			std::string why;
			switch (read(path, image, why))
			{
				case Status::Ok:
					return path;
				case Status::Wrong:
					problem += (problem.empty() ? "" : "\n") + why;
					break;
				case Status::Missing:
					break;
			}
		}
	return {};
}

} // namespace cartridge
