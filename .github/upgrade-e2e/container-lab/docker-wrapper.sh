#!/bin/bash
set -euo pipefail

real_docker=/usr/bin/docker
marker=/run/amnezia-upgrade-e2e-lab
owner=/opt/amnezia/.upgrade-e2e-owner
sentinel=/opt/amnezia-e2e/inject-docker-name-conflict

if [ "$EUID" -ne 0 ]; then
  echo "Upgrade e2e Docker wrapper must run as root" >&2
  exit 1
fi
nonce=$(cat "$marker" 2>/dev/null) || exit 1
test "$nonce" = "$(cat "$owner" 2>/dev/null)" || exit 1
test -f /.dockerenv

command=${1:-}
is_awg_container=false
previous=
for argument in "$@"; do
  if { [ "$previous" = --name ] && [ "$argument" = amnezia-awg2 ]; } \
      || [ "$argument" = --name=amnezia-awg2 ]; then
    is_awg_container=true
  fi
  previous=$argument
done
if [ "$command" = run ] && [ -f "$sentinel" ]; then
  case " $* " in
    *" --name amnezia-awg2 "*)
      rm -f "$sentinel"
      "$real_docker" create \
        --label "com.amnezia.upgrade-e2e.run=$nonce" \
        --name amnezia-awg2 alpine:3.20 sleep infinity >/dev/null
      ;;
  esac
fi

case "$command" in
  run|create)
    shift
    if $is_awg_container; then
      sanitized=()
      while [ "$#" -gt 0 ]; do
        case "$1" in
          --privileged|--privileged=true|--cap-add=SYS_MODULE)
            shift
            ;;
          --cap-add=NET_ADMIN)
            shift
            ;;
          --cap-add=*)
            echo "Unexpected capability in disposable upgrade lab: $1" >&2
            exit 1
            ;;
          --cap-add)
            test "$#" -ge 2
            case "$2" in
              NET_ADMIN|SYS_MODULE) shift 2 ;;
              *)
                echo "Unexpected capability in disposable upgrade lab: $2" >&2
                exit 1
                ;;
            esac
            ;;
          -v|--volume)
            test "$#" -ge 2
            if [ "$2" = /lib/modules:/lib/modules ]; then
              shift 2
            else
              echo "Unexpected volume in disposable upgrade lab: $2" >&2
              exit 1
            fi
            ;;
          -v/lib/modules:/lib/modules|--volume=/lib/modules:/lib/modules)
            shift
            ;;
          -v*|--volume=*|--mount|--mount=*)
            echo "Unexpected mount in disposable upgrade lab: $1" >&2
            exit 1
            ;;
          --device|--device=*)
            echo "Unexpected device in disposable upgrade lab: $1" >&2
            exit 1
            ;;
          *)
            sanitized+=("$1")
            shift
            ;;
        esac
      done
      exec "$real_docker" "$command" \
        --label "com.amnezia.upgrade-e2e.run=$nonce" \
        --cap-drop=ALL \
        --cap-add=NET_ADMIN \
        --device=/dev/net/tun \
        "${sanitized[@]}"
    fi
    exec "$real_docker" "$command" --label "com.amnezia.upgrade-e2e.run=$nonce" "$@"
    ;;
  build)
    shift
    tag=
    previous=
    for argument in "$@"; do
      if [ "$previous" = -t ] || [ "$previous" = --tag ]; then
        tag=$argument
      fi
      case "$argument" in
        --tag=*) tag=${argument#--tag=} ;;
      esac
      previous=$argument
    done
    context=${!#}
    test "$tag" = amnezia-awg2
    case "$context" in
      /opt/amnezia/awg|/workspace/client/server_scripts/awg) ;;
      *)
        echo "Unexpected AWG build context in disposable upgrade lab: $context" >&2
        exit 1
        ;;
    esac
    base_digest=$(cat /run/amnezia-upgrade-e2e-awg-base)
    case "$base_digest" in
      amneziavpn/amneziawg-go@sha256:????????????????????????????????????????????????????????????????) ;;
      *) exit 1 ;;
    esac
    source_dockerfile=$context/Dockerfile
    pinned_dockerfile=/run/amnezia-upgrade-e2e-awg.Dockerfile
    test "$(sed -n '1p' "$source_dockerfile")" = 'FROM amneziavpn/amneziawg-go:latest'
    {
      printf 'FROM %s\n' "$base_digest"
      sed '1d' "$source_dockerfile"
    } > "$pinned_dockerfile"
    cat >> "$pinned_dockerfile" <<'DOCKERFILE'
ENV WG_QUICK_USERSPACE_IMPLEMENTATION=amneziawg-go \
    PATH=/usr/local/bin:/usr/local/sbin:/usr/sbin:/usr/bin:/sbin:/bin
RUN real_ip="$(command -v ip)" && \
    mv "$real_ip" "${real_ip}.amnezia-real" && \
    printf '%s\n' \
      '#!/bin/sh' \
      'if [ "${1:-}" = link ] && [ "${2:-}" = add ]; then' \
      '  case " $* " in *" type amneziawg "*) exit 1 ;; esac' \
      'fi' \
      "exec ${real_ip}.amnezia-real \"\$@\"" \
      > /usr/local/bin/ip && \
    chmod 0755 /usr/local/bin/ip
DOCKERFILE
    chmod 0600 "$pinned_dockerfile"
    exec "$real_docker" build \
      --label "com.amnezia.upgrade-e2e.run=$nonce" \
      --label "com.amnezia.upgrade-e2e.base=$base_digest" \
      --label "com.amnezia.upgrade-e2e.userspace=forced" \
      -f "$pinned_dockerfile" "$@"
    ;;
  network)
    if [ "${2:-}" = create ]; then
      shift 2
      exec "$real_docker" network create --label "com.amnezia.upgrade-e2e.run=$nonce" "$@"
    fi
    ;;
esac
exec "$real_docker" "$@"
