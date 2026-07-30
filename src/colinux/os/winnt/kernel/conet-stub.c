/*
 * This source code is a part of coLinux source package.
 *
 * The code is licensed under the GPL. See the COPYING file at
 * the root directory.
 */

/*
 * Stand-in for conet.c, the in-driver NDIS protocol binding behind the
 * "ndis-bridge" network type.
 *
 * mingw-w64's ddk/ndis.h does not currently compile: the declaration of
 * NdisMWanIndicateReceiveComplete() is missing the comma between its two
 * parameters, and it sits in the body of the header rather than behind any
 * optional guard, so the header cannot be included at all. Until that is fixed
 * upstream (or a corrected copy is vendored here), the driver is built with
 * this file in place of conet.c and reports the ndis-bridge path as
 * unavailable.
 *
 * Guest networking over slirp or TAP is unaffected: both are served by
 * userspace daemons and never enter this code.
 *
 * Build conet.c instead by setting COLINUX_ENABLE_NDIS=yes.
 */

#include <colinux/common/debug.h>
#include <colinux/kernel/monitor.h>

co_rc_t co_conet_register_protocol(co_monitor_t *monitor)
{
	/* Called unconditionally when a monitor starts; nothing to register. */
	return CO_RC(OK);
}

co_rc_t co_conet_unregister_protocol(co_monitor_t *monitor)
{
	return CO_RC(OK);
}

co_rc_t co_conet_bind_adapter(co_monitor_t *monitor,
			      int	     conet_unit,
			      char*	     netcfg_id,
			      int	     promise,
			      char	     mac[6])
{
	co_debug("conet%d: this driver was built without NDIS support, "
		 "use slirp or TAP networking instead", conet_unit);
	return CO_RC(ERROR);
}

co_rc_t co_conet_unbind_adapter(co_monitor_t *monitor, int conet_unit)
{
	return CO_RC(ERROR);
}

co_rc_t co_conet_inject_packet_to_adapter(co_monitor_t *monitor,
					  int		 conet_unit,
					  void*		 packet_data,
					  int		 length)
{
	return CO_RC(ERROR);
}
