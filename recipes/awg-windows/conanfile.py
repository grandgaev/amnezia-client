from conan import ConanFile
from conan.tools.layout import basic_layout
from conan.errors import ConanInvalidConfiguration
from conan.tools.files import get, copy, chdir, load, save
from conan.tools.gnu import AutotoolsToolchain
from conan.tools.env import Environment

import json
import os

class AwgWindows(ConanFile):
    name = "awg-windows"
    version = "3.1.20260814"
    settings = "os", "arch"

    @property
    def _goarm(self):
        return {
            "armv5el": "5",
            "armv5hf": "5",
            "armv6": "6",
            "armv7": "7",
            "armv7hf": "7",
            "armv7s": "7",
            "armv7k": "7",
        }.get(str(self.settings.arch))
    
    @property
    def _goarch(self):
        return {
            "x86": "386",
            "x86_64": "amd64",
            "armv5el": "arm",
            "armv5hf": "arm",
            "armv6": "arm",
            "armv7": "arm",
            "armv7hf": "arm",
            "armv7s": "arm",
            "armv7k": "arm",
            "armv8": "arm64",
            "armv8_32": "arm64",
            "armv8.3": "arm64",
            "arm64ec": "arm64"
        }.get(str(self.settings.arch))

    def layout(self):
        basic_layout(self)

    def validate(self):
        if not str(self.settings.os).startswith("Windows"):
            raise ConanInvalidConfiguration(
                f"{self.name} v{self.version} is to be used on Windows only!"
            )
        if not self._goarch:
            raise ConanInvalidConfiguration(
                f"{self.name} v{self.version} does not support {self.settings.arch} architecture"
            )

    _awg_go_module = "github.com/amnezia-vpn/amneziawg-go/v3"
    _awg_go_src_version = "3.1.20260828"

    def build_requirements(self):
        self.tool_requires("mingw-builds/15.1.0")
        self.tool_requires("go/1.26.0")
        # Patched amneziawg-go sources (routing profiles router), see
        # recipes/awg-go-src. A new patch revision must produce a new binary.
        self.tool_requires(f"awg-go-src/{self._awg_go_src_version}", package_id_mode="revision_mode")

    def requirements(self):
        self.requires("wintun/[*]")

    def source(self):
        get(self, f"https://github.com/amnezia-vpn/amneziawg-windows/archive/refs/tags/v{self.version}.zip",
            sha256="d941861e3c0fada70b6b66b08aad4c77098d612aa11dd41b8ad70dd8afa6c61b", strip_root=True)
        
    def generate(self):
        tc = AutotoolsToolchain(self)
        tc.extra_cflags = [
            "-Wall",
            "-Wno-unused-function",
            "-Wno-switch",
            "-DWINVER=0x0601"
        ]
        tc.extra_ldflags = [ 
            "-Wl,--dynamicbase",
            "-Wl,--nxcompat",
            "-Wl,--export-all-symbols",
            "-Wl,--high-entropy-va"
        ]
        env = tc.environment()
        env.define("GOOS", "windows")
        if self._goarm:
            env.define("GOARM", self._goarm)
        env.define("GOARCH", self._goarch)
        env.define("CGO_ENABLED", "1")
        env.define("CGO_LDFLAGS", tc.ldflags)
        env.define("CGO_CFLAGS", tc.cflags)
        tc.generate(env)

    def _use_patched_awg_go(self):
        # Build against the patched amneziawg-go instead of the upstream module.
        src = os.path.join(self.dependencies.build["awg-go-src"].package_folder, "src").replace("\\", "/")
        go_mod = os.path.join(self.source_folder, "go.mod")
        lines = [line for line in load(self, go_mod).splitlines()
                 if not line.startswith(f"replace {self._awg_go_module} ")]
        lines.append(f"replace {self._awg_go_module} => {json.dumps(src)}")
        save(self, go_mod, "\n".join(lines) + "\n")

    def build(self):
        self._use_patched_awg_go()
        env = Environment()
        # Lets go add the go.sum entries of the patched module's dependencies.
        env.define("GOFLAGS", "-mod=mod")
        with chdir(self, self.source_folder), env.vars(self).apply():
            self.run(f'go build -buildmode c-shared -ldflags="-w -s" -trimpath -v -o "{os.path.join(self.build_folder, "tunnel.dll")}"')

    def package(self):
        copy(self, "tunnel.dll", src=self.build_folder, dst=os.path.join(self.package_folder, "bin"))

    def package_info(self):
        self.cpp_info.exe = True
        self.cpp_info.location = os.path.join(self.package_folder, "bin", "tunnel.dll")
        self.cpp_info.set_property("cmake_target_name", "amnezia::awg-windows")
