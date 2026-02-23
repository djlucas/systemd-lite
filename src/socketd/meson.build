# SPDX-License-Identifier: LGPL-2.1-or-later

systemd_socketd_sources = files('systemd-socketd.c')

executables += [
        executable_template + {
                'name' : 'systemd-socketd',
                'sources' : systemd_socketd_sources,
                'install_dir' : libexecdir,
        },
]
