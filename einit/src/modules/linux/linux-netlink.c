/*
 *  linux-netlink.c
 *  einit
 *
 *  Created by Magnus Deininger on 07/08/2007.
 *  Copyright 2006, 2007 Magnus Deininger. All rights reserved.
 *
 */

/*
Copyright (c) 2006, 2007, Magnus Deininger
All rights reserved.

Redistribution and use in source and binary forms, with or without modification,
are permitted provided that the following conditions are met:

    * Redistributions of source code must retain the above copyright notice,
	  this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright notice,
	  this list of conditions and the following disclaimer in the documentation
	  and/or other materials provided with the distribution.
    * Neither the name of the project nor the names of its contributors may be
	  used to endorse or promote products derived from this software without
	  specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR
ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <einit/module.h>
#include <einit/config.h>
#include <einit/bitch.h>
#include <string.h>
#include <time.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <errno.h>
#include <dirent.h>
#include <ctype.h>

#include <netlink/handlers.h>
#include <netlink/netlink.h>
#include <pthread.h>

#define EXPECTED_EIV 1

#if EXPECTED_EIV != EINIT_VERSION
#warning "This module was developed for a different version of eINIT, you might experience problems"
#endif

int linux_netlink_configure (struct lmodule *);

#if defined(EINIT_MODULE) || defined(EINIT_MODULE_HEADER)
const struct smodule module_linux_netlink_self = {
 .eiversion = EINIT_VERSION,
 .eibuild   = BUILDNUMBER,
 .version   = 1,
 .mode      = 0,
 .name      = "eINIT <-> NetLink Connector",
 .rid       = "linux-netlink",
 .si        = {
  .provides = NULL,
  .requires = NULL,
  .after    = NULL,
  .before   = NULL
 },
 .configure = linux_netlink_configure
};

module_register(module_linux_netlink_self);

#endif

struct nl_handle *linux_netlink_handle;

int linux_netlink_cleanup (struct lmodule *this) {
 nl_close (linux_netlink_handle);
 nl_handle_destroy(linux_netlink_handle);

 return 0;
}

void *linux_netlink_read_thread (void *irrelevant) {
/* while (1) {
 }*/
 nl_close (linux_netlink_handle);
 nl_handle_destroy(linux_netlink_handle);

 return NULL;
}

int linux_netlink_connect() {
 linux_netlink_handle = nl_handle_alloc();
 nl_handle_set_pid(linux_netlink_handle, getpid());
 nl_disable_sequence_check(linux_netlink_handle);
 return nl_connect(linux_netlink_handle, NETLINK_GENERIC);
}

int linux_netlink_configure (struct lmodule *irr) {
 pthread_t thread;

 module_init (irr);

 thismodule->cleanup = linux_netlink_cleanup;

 if (linux_netlink_connect()) {
  notice (2, "eINIT <-> NetLink: could not connect");
  perror ("netlink NOT connected (%s)");
 } else {
  notice (2, "eINIT <-> NetLink: connected");
  ethread_create (&thread, &thread_attribute_detached, linux_netlink_read_thread, NULL);
 }

 return 0;
}

/* passive module... */
