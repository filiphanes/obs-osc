#include "osc-message.h"

#include <cstring>

static inline uint32_t read_be32(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static inline void write_be32(std::vector<uint8_t> *buf, uint32_t v)
{
	buf->push_back((uint8_t)(v >> 24));
	buf->push_back((uint8_t)(v >> 16));
	buf->push_back((uint8_t)(v >> 8));
	buf->push_back((uint8_t)v);
}

/* OSC strings are NUL-terminated and padded to a 4 byte boundary. */
static bool read_string(const uint8_t *data, size_t size, size_t *pos, std::string *out)
{
	if (*pos >= size)
		return false;

	const void *nul = memchr(data + *pos, 0, size - *pos);
	if (!nul)
		return false;

	out->assign(reinterpret_cast<const char *>(data + *pos));
	*pos += (out->size() + 4) & ~3u;
	return true;
}

static void write_padded_string(std::vector<uint8_t> *buf, const char *str)
{
	size_t len = strlen(str) + 1;

	buf->insert(buf->end(), str, str + len);
	while (len++ & 3)
		buf->push_back(0);
}

static bool parse_message(const uint8_t *data, size_t size, std::vector<osc_message> *out)
{
	osc_message msg;
	std::string tags;
	size_t pos = 0;

	if (!read_string(data, size, &pos, &msg.address))
		return false;
	if (msg.address.empty() || msg.address[0] != '/')
		return false;

	if (!read_string(data, size, &pos, &tags))
		return false;
	if (tags.empty() || tags[0] != ',')
		return false;

	for (size_t k = 1; k < tags.size(); k++) {
		osc_argument arg;
		arg.type = tags[k];

		switch (arg.type) {
		case 'i':
			if (pos + 4 > size)
				return false;
			arg.i = (int32_t)read_be32(data + pos);
			pos += 4;
			break;
		case 'f': {
			if (pos + 4 > size)
				return false;
			const uint32_t raw = read_be32(data + pos);
			memcpy(&arg.f, &raw, sizeof(raw));
			pos += 4;
			break;
		}
		case 's':
			if (!read_string(data, size, &pos, &arg.s))
				return false;
			break;
		case 'b': {
			if (pos + 4 > size)
				return false;
			const uint32_t blob = read_be32(data + pos);
			pos += 4;
			if ((size_t)blob > size - pos)
				return false;
			arg.b.assign(data + pos, data + pos + blob);
			pos += (blob + 3u) & ~3u;
			break;
		}
		case 'T':
			arg.i = 1;
			break;
		case 'F':
			arg.i = 0;
			break;
		case 'd':
		case 'h':
		case 't':
			if (pos + 8 > size)
				return false;
			pos += 8;
			break;
		case 'm':
		case 'r':
		case 'c':
			if (pos + 4 > size)
				return false;
			pos += 4;
			break;
		default:
			/* Unknown typetag: cannot safely continue. */
			return false;
		}

		msg.args.push_back(std::move(arg));
	}

	out->push_back(std::move(msg));
	return true;
}

bool osc_parse_packet(const uint8_t *data, size_t size, std::vector<osc_message> *out)
{
	if (!data || !out || size < 4 || (size & 3) != 0)
		return false;

	if (size >= 16 && memcmp(data, "#bundle", 8) == 0) {
		size_t pos = 16; /* skip timetag: elements are executed immediately */

		while (pos + 4 <= size) {
			const uint32_t elem = read_be32(data + pos);
			pos += 4;
			if (elem == 0 || elem > size - pos)
				break;
			osc_parse_packet(data + pos, elem, out);
			pos += elem;
		}
		return true;
	}

	return parse_message(data, size, out);
}

void osc_build_message(std::vector<uint8_t> *buf, const char *address,
		       std::initializer_list<osc_argument> args)
{
	std::string tags = ",";

	for (const osc_argument &a : args)
		tags += a.type ? a.type : 's';

	write_padded_string(buf, address);
	write_padded_string(buf, tags.c_str());

	for (const osc_argument &a : args) {
		switch (a.type) {
		case 'i':
			write_be32(buf, (uint32_t)a.i);
			break;
		case 'f': {
			uint32_t raw;
			memcpy(&raw, &a.f, sizeof(raw));
			write_be32(buf, raw);
			break;
		}
		case 'b': {
			const uint32_t blob = (uint32_t)a.b.size();
			write_be32(buf, blob);
			buf->insert(buf->end(), a.b.begin(), a.b.end());
			for (uint32_t pad = (4u - (blob & 3u)) & 3u; pad; pad--)
				buf->push_back(0);
			break;
		}
		default:
			write_padded_string(buf, a.s.c_str());
			break;
		}
	}
}

bool osc_message::arg_bool(size_t index, bool def) const
{
	if (index >= args.size())
		return def;

	const osc_argument &a = args[index];
	if (a.type == 'T')
		return true;
	if (a.type == 'F')
		return false;
	if (a.type == 'i')
		return a.i != 0;
	if (a.type == 'f')
		return a.f != 0.0f;
	return def;
}

int32_t osc_message::arg_int(size_t index, int32_t def) const
{
	if (index >= args.size())
		return def;

	const osc_argument &a = args[index];
	if (a.type == 'i' || a.type == 'T' || a.type == 'F')
		return a.i;
	if (a.type == 'f')
		return (int32_t)a.f;
	return def;
}

float osc_message::arg_float(size_t index, float def) const
{
	if (index >= args.size())
		return def;

	const osc_argument &a = args[index];
	if (a.type == 'f')
		return a.f;
	if (a.type == 'i')
		return (float)a.i;
	return def;
}
