from conan import ConanFile
from conan.errors import ConanInvalidConfiguration
from conan.tools.layout import basic_layout
from conan.tools.files import copy, collect_libs, load, save
from conan.tools.apple import is_apple_os
from conan.tools.gnu import AutotoolsToolchain, Autotools
from conan.tools.scm import Git

import json
import os

class AwgApple(ConanFile):
    name = "awg-apple"
    version = "3.1.4"
    settings = "os", "arch", "compiler"
    exports_sources = "patches/*"

    _awg_go_module = "github.com/amnezia-vpn/amneziawg-go/v3"

    @property
    def _goarch(self):
        arch_map = {
            "armv8": "arm64",
            "x86_64": "x86_64",
        }
        archs = str(self.settings.arch).split("|")
        return " ".join(arch_map.get(arch, arch) for arch in archs)

    def configure(self):
        self.settings.rm_safe("compiler.libcxx")
        self.settings.rm_safe("compiler.cppstd")

    def layout(self):
        basic_layout(self, build_folder=os.path.join(self.folders.source, "Sources/WireGuardKitGo"))

    def build_requirements(self):
        self.tool_requires("go/1.26.0")
        # Patched amneziawg-go sources (routing profiles router), see
        # recipes/awg-go-src. A new patch revision must produce a new binary.
        self.tool_requires("awg-go-src/3.1.20260828", package_id_mode="revision_mode")

    def validate(self):
        if not is_apple_os(self):
            raise ConanInvalidConfiguration(
                f"{self.name} v{self.version} does not support {self.settings.os}"
            )

    def source(self):
        # The exported patches are already in the source folder, which "git clone"
        # refuses to clone into, so the tag is fetched into a new repository instead.
        git = Git(self)
        git.run("init -q")
        git.run(f"fetch -q --depth 1 https://github.com/amnezia-vpn/amneziawg-apple.git refs/tags/v{self.version}")
        git.run("checkout -q FETCH_HEAD")
        patches = os.path.join(self.source_folder, "patches")
        for patch in sorted(os.listdir(patches)):
            git.run(f'apply --whitespace=nowarn "{os.path.join(patches, patch)}"')

    def generate(self):
        tc = AutotoolsToolchain(self)
        sdk = self.settings.get_safe("os.sdk", "macosx")
        tc.make_args = [
            f"ARCHS={self._goarch}",
            f"PLATFORM_NAME={sdk}"
        ]
        env = tc.environment()
        # -mod=mod: go.sum lacks the entries of the dependencies of the patched amneziawg-go.
        # -buildvcs=false: the sources are a git checkout now, keep the build as with the release archive.
        env.define("GOFLAGS", "-mod=mod -buildvcs=false")
        tc.generate(env)

    def _use_awg_go_src(self):
        awg_go_src = os.path.join(self.dependencies.build["awg-go-src"].package_folder, "src").replace("\\", "/")
        go_mod = os.path.join(self.build_folder, "go.mod")
        content = load(self, go_mod)
        directive = f"replace {self._awg_go_module} =>"
        if directive not in content:
            save(self, go_mod, f"{content.rstrip()}\n\n{directive} {json.dumps(awg_go_src)}\n")

    def build(self):
        self._use_awg_go_src()
        autotools = Autotools(self)
        autotools.make()
        autotools.make("version-header")

    def package(self):
        copy(self, "wireguard.h", src=self.build_folder, dst=os.path.join(self.package_folder, "include"))
        copy(self, "*.h", src=os.path.join(self.build_folder, "out"), dst=os.path.join(self.package_folder, "include"))
        copy(self, "*.a", src=os.path.join(self.build_folder, "out"), dst=os.path.join(self.package_folder, "lib"))

    def package_info(self):
        self.cpp_info.set_property("cmake_target_name", "amnezia::awg-apple")
        self.cpp_info.libs = collect_libs(self)
