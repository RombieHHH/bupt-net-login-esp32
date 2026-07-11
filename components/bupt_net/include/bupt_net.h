#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* Optional IPv6 support. Set either macro to 0 to exclude that behavior. */
#ifndef BUPT_NET_ENABLE_IPV6
#define BUPT_NET_ENABLE_IPV6 1
#endif

#ifndef BUPT_NET_ENABLE_IPV6_PING
#define BUPT_NET_ENABLE_IPV6_PING BUPT_NET_ENABLE_IPV6
#endif

#ifndef BUPT_NET_IPV6_PING_TARGET
#define BUPT_NET_IPV6_PING_TARGET "2400:3200::1"
#endif

void bupt_net_init(void);
void bupt_net_run(const char *user, const char *pass, int keepalive_sec);

#ifdef __cplusplus
}
#endif
