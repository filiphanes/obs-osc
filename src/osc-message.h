#pragma once

/*
 * Minimal OSC 1.0 wire codec (Open Sound Control).
 *
 * Handles address-pattern messages and #bundle packets (bundles are
 * flattened and executed immediately, timetag scheduling is ignored).
 *
 * Inbound typetags: i f s b T F, plus safe skipping of d h t m r c.
 * Outbound typetags: i f s b.
 *
 * https://opensoundcontrol.stanford.edu/spec-1_0.html
 */

#include <cstdint>
#include <initializer_list>
#include <string>
#include <vector>

struct osc_argument {
	char type = '\0'; /* 'i', 'f', 's', 'b', 'T' or 'F' */
	int32_t i = 0;
	float f = 0.0f;
	std::string s;
	std::vector<uint8_t> b;
};

struct osc_message {
	std::string address;
	std::vector<osc_argument> args;

	bool arg_bool(size_t index, bool def) const;
	int32_t arg_int(size_t index, int32_t def) const;
	float arg_float(size_t index, float def) const;
	std::string arg_string(size_t index, const char *def) const;
};

inline osc_argument osc_int(int32_t v)
{
	osc_argument a;
	a.type = 'i';
	a.i = v;
	return a;
}

inline osc_argument osc_flt(float v)
{
	osc_argument a;
	a.type = 'f';
	a.f = v;
	return a;
}

inline osc_argument osc_str(std::string v)
{
	osc_argument a;
	a.type = 's';
	a.s = std::move(v);
	return a;
}

inline osc_argument osc_blob(std::vector<uint8_t> v)
{
	osc_argument a;
	a.type = 'b';
	a.b = std::move(v);
	return a;
}

/* Parses one packet (message or bundle). Returns false on structurally
 * broken input. Well-formed bundles are flattened in order. */
bool osc_parse_packet(const uint8_t *data, size_t size, std::vector<osc_message> *out);

/* Encodes one message and appends it to buf. */
void osc_build_message(std::vector<uint8_t> *buf, const char *address,
		       std::initializer_list<osc_argument> args = {});
