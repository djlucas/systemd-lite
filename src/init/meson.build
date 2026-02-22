# SPDX-License-Identifier: LGPL-2.1-or-later

systemd_init_sources = files('systemd-init.c')

executables += [
        libexec_template + {
                'name' : 'systemd-init',
                'sources' : systemd_init_sources,
                'link_with' : [],
                'c_args' : ['-fno-pie'],
                'link_args' : ['-static', '-no-pie'],
                'build_by_default' : have_standalone_binaries,
                'install' : have_standalone_binaries,
        },
]
