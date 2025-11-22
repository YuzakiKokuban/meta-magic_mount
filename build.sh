#!/bin/bash

# Color definitions
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

# Configuration
BASE_URL="https://github.com/YuzakiKokuban/meta-magic_mount/releases/download"
UPDATE_JSON_URL="https://raw.githubusercontent.com/YuzakiKokuban/meta-magic_mount/public/update.json"
CHANGELOG_URL="https://raw.githubusercontent.com/YuzakiKokuban/meta-magic_mount/public/changelog.md"

# Build state
BUILD_TYPES=()
VERSION=""
VERSION_FULL=""
GIT_COMMIT=""
TEMP_DIRS=()

# Logging functions
log_info() { echo -e "${GREEN}$*${NC}" >&2; }
log_warn() { echo -e "${YELLOW}$*${NC}" >&2; }
log_error() { echo -e "${RED}$*${NC}" >&2; }
log_step() { echo -e "\n${YELLOW}[$1/$2] $3${NC}" >&2; }

# Cleanup function
cleanup() {
    local exit_code=$?
    log_warn "Cleaning up temporary directories..."
    for dir in "${TEMP_DIRS[@]}"; do
        if [ -d "$dir" ]; then
            rm -rf "$dir"
            log_info "  Removed: $dir"
        fi
    done
    if [ $exit_code -ne 0 ]; then
        log_error "Build failed with exit code $exit_code"
    fi
    exit $exit_code
}
trap cleanup EXIT INT TERM

usage() {
    cat << EOF
Usage: $0 [OPTIONS]
OPTIONS:
    --release       Build release version only
    --debug         Build debug version only
    -h, --help      Show this help message
    (no option)     Build both versions
EOF
    exit 1
}

parse_args() {
    while [[ $# -gt 0 ]]; do
        case $1 in
            --release) BUILD_TYPES+=("release"); shift ;;
            --debug) BUILD_TYPES+=("debug"); shift ;;
            -h|--help) usage ;;
            *) log_error "Unknown parameter: $1"; usage ;;
        esac
    done
    [ ${#BUILD_TYPES[@]} -eq 0 ] && BUILD_TYPES=("release" "debug")
}

check_prerequisites() {
    if ! git rev-parse --git-dir > /dev/null 2>&1; then log_error "Not in a git repository"; exit 1; fi
    if ! command -v zig &> /dev/null; then log_error "zig is not installed"; exit 1; fi
    # Check for pnpm/npm for webui
    if ! command -v pnpm &> /dev/null && ! command -v npm &> /dev/null; then
        log_warn "Node.js package manager not found (pnpm/npm). WebUI might not build."
    fi
    for dir in template src webui; do
        if [ ! -d "$dir" ]; then log_error "Required directory '$dir' not found"; exit 1; fi
    done
}

get_version_info() {
    local describe=$(git describe --tags --long --always --dirty 2>/dev/null)
    if [ -z "$describe" ]; then log_error "Failed to get git information"; exit 1; fi
    
    if [[ "$describe" =~ ^(.+)-([0-9]+)-g([0-9a-f]+)(-dirty)?$ ]]; then
        VERSION="${BASH_REMATCH[1]}"
        local commits="${BASH_REMATCH[2]}"
        GIT_COMMIT="${BASH_REMATCH[3]}"
        local dirty="${BASH_REMATCH[4]}"
        
        if [ "$commits" = "0" ]; then
            VERSION_FULL="$VERSION"
        else
            VERSION_FULL="${VERSION}-${commits}-g${GIT_COMMIT}"
        fi
        [ -n "$dirty" ] && VERSION_FULL="${VERSION_FULL}-dirty"
    else
        VERSION="0.0.0"
        GIT_COMMIT="$describe"
        VERSION_FULL="$describe"
    fi
    log_info "Version: $VERSION_FULL"
}

build_webui() {
    log_step 2 7 "Building WebUI"
    
    cd webui || return 1
    
    # Sync version to package.json
    if [ -f "package.json" ]; then
        if [[ "$OSTYPE" == "darwin"* ]]; then
            sed -i '' "s/\"version\": \".*\"/\"version\": \"$VERSION\"/" package.json
        else
            sed -i "s/\"version\": \".*\"/\"version\": \"$VERSION\"/" package.json
        fi
    fi

    # Install & Build
    if command -v pnpm &> /dev/null; then
        pnpm install && pnpm build
    else
        npm install && npm run build
    fi
    
    if [ ! -d "dist" ]; then
        log_error "WebUI build failed (dist folder missing)"
        cd ..
        return 1
    fi
    
    cd ..
    log_info "WebUI built successfully"
}

build_binaries() {
    local build_type=$1
    log_step 3 7 "Building binaries ($build_type)"
    
    cd src || return 1
    make clean > /dev/null 2>&1
    if ! make "$build_type" VERSION="$VERSION_FULL" >&2; then
        cd ..
        log_error "Failed to build binaries"
        return 1
    fi
    cd ..
}

configure_module() {
    local build_dir=$1
    local build_type=$2
    local version_code=$3
    
    log_step 4 7 "Configuring module files"
    
    local module_prop="$build_dir/module.prop"
    
    sed -i "s|^version=.*|version=$VERSION_FULL|" "$module_prop"
    sed -i "s|^versionCode=.*|versionCode=$version_code|" "$module_prop"
    sed -i "s|^updateJson=.*|updateJson=$UPDATE_JSON_URL|" "$module_prop"
}

build_single_type() {
    local build_type=$1
    local build_dir="build/${build_type}_temp"
    local version_code=$(git rev-list --count HEAD)
    local output_name="meta-hybrid-${VERSION_FULL}-${build_type}.zip"
    
    TEMP_DIRS+=("$build_dir")
    TEMP_DIRS+=("src/bin")
    
    rm -rf "$build_dir"
    mkdir -p "$build_dir"
    
    # 1. Copy template
    log_step 1 7 "Copying template"
    cp -r template/* "$build_dir/"
    
    # 2. Build WebUI (Only need to run once, but running here for simplicity)
    
    # 3. Build Binaries
    build_binaries "$build_type" || return 1
    
    # 4. Configure
    configure_module "$build_dir" "$build_type" "$version_code" || return 1
    
    # 5. Copy Binaries
    log_step 5 7 "Copying binaries"
    mkdir -p "$build_dir/bin"
    cp src/bin/* "$build_dir/bin/"
    
    # 6. Copy WebUI
    log_step 6 7 "Copying WebUI"
    mkdir -p "$build_dir/webroot"
    cp -r webui/dist/* "$build_dir/webroot/"
    
    # 7. Package
    log_step 7 7 "Creating package"
    if ! (cd "$build_dir" && zip -qr "../../build/$output_name" ./*); then
        log_error "Failed to create package"
        return 1
    fi
    
    local size=$(du -h "build/$output_name" | cut -f1)
    log_info "Build complete: $output_name ($size)"
}

main() {
    parse_args "$@"
    check_prerequisites
    get_version_info
    
    mkdir -p build
    
    # Build WebUI once for all types
    build_webui || exit 1
    
    for build_type in "${BUILD_TYPES[@]}"; do
        build_single_type "$build_type"
    done
}

main "$@"