#pragma once

/*
 * Bonjour/mDNS advertisement of the "_oscjson._tcp" service so OSCQuery
 * controllers can discover the plugin. Only implemented on Apple
 * platforms; everywhere else registration is a no-op.
 */

void osc_bonjour_register(int http_port);
void osc_bonjour_unregister(void);
