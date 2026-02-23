# SPDX-License-Identifier: LGPL-2.1-or-later

systemd_fsmd_sources = files('systemd-fsmd.c')

executables += [
        executable_template + {
                'name' : 'systemd-fsmd',
                'sources' : systemd_fsmd_sources,
                'install_dir' : libexecdir,
        },
]
