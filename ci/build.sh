#!/bin/bash
# Building generator and processor for ProcessBus — HOST SIDE.

set -e          # Exit on error
set -o pipefail # Don't let a successful tail/grep mask an upstream failure

# realpath on the parent, not "$CI_DIR/.." — REPO_DIR becomes a podman -v
# source, and a bind mount wants a clean absolute path.
CI_DIR="$(dirname "$(realpath "$0")")"
REPO_DIR="$(realpath "$CI_DIR/..")"
source "$CI_DIR/platforms.sh"

# Where the repository is mounted inside the container. The internal script
# derives everything from its own location, so this only has to be consistent.
CONTAINER_REPO="/ProcessBus"

PLATFORM=atom
ACTION=""
PASSTHRU=()

function usage()
{
    echo "Usage: $0 [--platform=atom/qemu/orangepi3b/all] [--update=0/1] [--dpdk=0/1] [--pbus=0/1] [--rebuild] [--clean] [--check] [--setup] [--shell]"
    echo ""
    echo "Runs ci/build_internal.sh inside the builder container for the platform."
    exit 1
}

# --platform is needed here to choose the image, and is also forwarded so the
# internal script agrees with us. --setup/--shell never reach the container.
while [[ "$#" -gt 0 ]]; do
    case $1 in
        --platform=*)
            PLATFORM="${1#*=}"
            PASSTHRU+=("$1")
            ;;
        --platform)
            PLATFORM="$2"
            PASSTHRU+=("$1" "$2")
            shift
            ;;
        --setup|--shell)
            ACTION="${1#--}"
            ;;
        -h|--help)
            usage
            ;;
        *)
            PASSTHRU+=("$1")
            ;;
    esac
    shift
done

# --platform=all iterates x86 platforms only, which share the default builder
# image; resolve the container against atom and let the internal script loop.
IMAGE_PLATFORM="$PLATFORM"
[[ "$IMAGE_PLATFORM" == "all" ]] && IMAGE_PLATFORM=atom

SAVED_PLATFORM="$PLATFORM"
PLATFORM="$IMAGE_PLATFORM"
configure_platform
PLATFORM="$SAVED_PLATFORM"

# Building the image is an explicit action, never a side effect of a build.
if [[ "$ACTION" == "setup" ]]; then
    echo "Building image $BUILDER_IMAGE from $(basename "$DOCKERFILE")"
    podman build -f "$DOCKERFILE" --tag "$BUILDER_IMAGE" "$REPO_DIR"
    exit 0
fi

if ! podman image exists "$BUILDER_IMAGE"; then
    echo "Builder image '$BUILDER_IMAGE' not found." >&2
    echo "Run: $0 --platform=$PLATFORM --setup" >&2
    exit 1
fi

# --userns=keep-id maps the caller to the same uid inside, so build artifacts
# stay owned by the caller instead of by root.
PODMAN_COMMON=(
    --rm
    --cap-add=NET_RAW
    -v "$REPO_DIR:$CONTAINER_REPO:Z"
    --userns=keep-id
    -w "$CONTAINER_REPO"
)

if [[ "$ACTION" == "shell" ]]; then
    podman run -it "${PODMAN_COMMON[@]}" --name "$BUILDER_IMAGE" "$BUILDER_IMAGE" /bin/bash
    exit 0
fi

podman run "${PODMAN_COMMON[@]}" "$BUILDER_IMAGE" \
    "$CONTAINER_REPO/ci/build_internal.sh" "${PASSTHRU[@]}"
