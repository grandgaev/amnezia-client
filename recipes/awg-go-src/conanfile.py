from conan import ConanFile
from conan.tools.files import copy
from conan.tools.scm import Git

import os


class AwgGoSrc(ConanFile):
    """Patched amneziawg-go sources shared by the AmneziaWG backends.

    Adds the rule based split tunnelling router (routing profiles) and transport
    fixes on top of the upstream release. The backends (awg-go, awg-windows,
    awg-apple, awg-android) build against these sources through a go.mod replace
    directive until the changes are merged upstream.
    """

    name = "awg-go-src"
    version = "3.1.20260828"
    package_type = "build-scripts"
    exports_sources = "patches/*"
    no_copy_source = False

    module_path = "github.com/amnezia-vpn/amneziawg-go/v3"

    def source(self):
        git = Git(self)
        git.clone(
            url="https://github.com/amnezia-vpn/amneziawg-go.git",
            target="src",
            args=["--depth", "1", "--branch", f"v{self.version}"],
        )
        src = os.path.join(self.source_folder, "src")
        for patch in sorted(os.listdir(os.path.join(self.source_folder, "patches"))):
            self.run(f'git -C "{src}" apply --whitespace=nowarn "{os.path.join(self.source_folder, "patches", patch)}"')

    def package(self):
        copy(self, "*", src=os.path.join(self.source_folder, "src"), dst=os.path.join(self.package_folder, "src"),
             excludes=[".git/*", ".git"])

    def package_info(self):
        self.cpp_info.includedirs = []
        self.cpp_info.libdirs = []
        self.cpp_info.bindirs = []
        self.conf_info.define("user.awg-go-src:path", os.path.join(self.package_folder, "src").replace("\\", "/"))
        self.conf_info.define("user.awg-go-src:module", self.module_path)
