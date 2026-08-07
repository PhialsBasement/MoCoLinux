/*
 * This source code is a part of coLinux source package.
 *
 * Dan Aloni <da-x@colinux.org>, 2004 (c)
 *
 * The code is licensed under the GPL. See the COPYING file at
 * the root directory.
 *
 */

#ifndef __CO_OS_USER_WINNT_DAEMON_DRIVER_H__
#define __CO_OS_USER_WINNT_DAEMON_DRIVER_H__

#include <colinux/common/common.h>

extern co_rc_t co_winnt_status_driver(int verbose);
extern co_rc_t co_winnt_probe_va(const char* arg);
extern co_rc_t co_winnt_probe_passage(void);
extern co_rc_t co_winnt_probe_sweep(void);
extern co_rc_t co_winnt_save_state(bool_t restore);
extern co_rc_t co_winnt_test_switch(int mode);
extern co_rc_t co_winnt_test_space(void);
extern co_rc_t co_winnt_test_smp(const char* iterations_arg);

extern co_rc_t co_winnt_install_driver(void);
extern co_rc_t co_winnt_initialize_driver(void);
extern co_rc_t co_winnt_remove_driver(void);

#endif
