FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && \
    apt-get install --no-install-recommends -y \
      ca-certificates docker.io iproute2 iptables openssh-server psmisc sudo && \
    rm -rf /var/lib/apt/lists/* && \
    install -d -m 0755 /opt/amnezia-e2e/bin /etc/docker && \
    printf '%s\n' \
      '{' \
      '  "features": {"containerd-snapshotter": false},' \
      '  "storage-driver": "vfs"' \
      '}' \
      > /etc/docker/daemon.json

COPY .github/upgrade-e2e/container-lab/entrypoint.sh /usr/local/bin/upgrade-e2e-entrypoint
COPY .github/upgrade-e2e/container-lab/attest.sh /usr/local/bin/upgrade-e2e-attest
COPY .github/upgrade-e2e/container-lab/docker-wrapper.sh /opt/amnezia-e2e/bin/docker
COPY .github/upgrade-e2e/container-lab/sysctl-wrapper.sh /opt/amnezia-e2e/bin/sysctl
COPY .github/upgrade-e2e/container-lab/pin-awg-base.sh /usr/local/bin/upgrade-e2e-pin-awg-base
COPY .github/upgrade-e2e/container-lab/seed.sh /usr/local/bin/upgrade-e2e-seed
COPY .github/upgrade-e2e/container-lab/enable-conflict.sh /usr/local/bin/upgrade-e2e-enable-conflict
COPY .github/upgrade-e2e/container-lab/prepare-recovery.sh /usr/local/bin/upgrade-e2e-prepare-recovery
COPY .github/upgrade-e2e/container-lab/probe-peers.sh /usr/local/bin/upgrade-e2e-probe-peers
COPY client/server_scripts/awg /workspace/client/server_scripts/awg

RUN chmod 0755 \
      /usr/local/bin/upgrade-e2e-entrypoint \
      /usr/local/bin/upgrade-e2e-attest \
      /opt/amnezia-e2e/bin/docker \
      /opt/amnezia-e2e/bin/sysctl \
      /usr/local/bin/upgrade-e2e-pin-awg-base \
      /usr/local/bin/upgrade-e2e-seed \
      /usr/local/bin/upgrade-e2e-enable-conflict \
      /usr/local/bin/upgrade-e2e-prepare-recovery \
      /usr/local/bin/upgrade-e2e-probe-peers

HEALTHCHECK --interval=2s --timeout=2s --start-period=5m --retries=60 \
  CMD /usr/bin/docker info >/dev/null 2>&1 && \
      test -s /run/amnezia-upgrade-e2e-lab && \
      test -s /run/amnezia-upgrade-e2e-awg-base && \
      test -s /run/amnezia-upgrade-e2e-awg-base.manifest && \
      ss -H -ltn 'sport = :22' | grep -q .

ENTRYPOINT ["/usr/local/bin/upgrade-e2e-entrypoint"]
