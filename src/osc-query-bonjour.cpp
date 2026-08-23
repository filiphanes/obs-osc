#include "osc-query-bonjour.h"

#include <util/base.h>

#ifdef __APPLE__
#include "osc-net.h"

#include <cstdio>
#include <string>

#include <dns_sd.h>

static DNSServiceRef bonjour_ref = nullptr;

static void bonjour_reply(DNSServiceRef, DNSServiceFlags, DNSServiceErrorType, const char *,
			  const char *, const char *, void *)
{
	/* Registration results are not tracked. */
}

void osc_bonjour_register(int http_port)
{
	char host[128] = {};
	if (gethostname(host, sizeof(host) - 1) != 0)
		snprintf(host, sizeof(host), "OBS");

	const std::string name = std::string("OBS Studio @ ") + host;
	const DNSServiceErrorType err = DNSServiceRegister(
		&bonjour_ref, 0, 0, name.c_str(), "_oscjson._tcp.", nullptr, nullptr,
		htons((uint16_t)http_port), 0, nullptr, bonjour_reply, nullptr);

	if (err != kDNSServiceErr_NoError) {
		bonjour_ref = nullptr;
		blog(LOG_WARNING, "[obs-osc] Bonjour registration failed (%d)", (int)err);
		return;
	}

	blog(LOG_INFO, "[obs-osc] advertised as '%s' via _oscjson._tcp", name.c_str());
}

void osc_bonjour_unregister(void)
{
	if (bonjour_ref) {
		DNSServiceRefDeallocate(bonjour_ref);
		bonjour_ref = nullptr;
	}
}

#else

void osc_bonjour_register(int http_port)
{
	UNUSED_PARAMETER(http_port);
}

void osc_bonjour_unregister(void)
{
}

#endif
