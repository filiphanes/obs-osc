#pragma once

/*
 * OSCQuery schema: renders the plugin's address namespace (see
 * osc-addresses.h) as the JSON node tree that OSCQuery controllers
 * fetch. The tree must be built on the UI thread, so lookups block
 * briefly while the UI thread produces the document.
 */

#include <string>

/* Returns the OSCQuery JSON description for an address path, or ""
 * when the path does not exist. */
std::string osc_query_schema_lookup(const std::string &path);
