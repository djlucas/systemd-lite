# SPDX-License-Identifier: LGPL-2.1-or-later

systemd_timerd_sources = files('systemd-timerd.c')

executables += [
        executable_template + {
                'name' : 'systemd-timerd',
                'sources' : systemd_timerd_sources,
                'install_dir' : libexecdir,
        },
]
