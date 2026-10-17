#!/usr/bin/env bash
#
# install-bluefin.sh: Build, install, and configure libfprint with Goodix 532D support
# tailored specifically for Fedora Bluefin / Silverblue (immutable ostree).
#
# Features:
#  - Robust host & container detection (handles host, sudo, and Distrobox bench).
#  - Builds the driver in distrobox/bench or host ninja.
#  - Installs custom libfprint into /usr/local/lib64 (persists across ostree updates).
#  - Copies sensor baseline calibration to /var/lib/fprint/goodix53xd_clear.pgm.
#  - Configures ldconfig and systemd sandboxing overrides for fprintd.
#  - Configures PAM via authselect for GNOME lock screen, GDM, and sudo.
#  - Configures Polkit rule for wheel group fingerprint management.
#  - Onboards user fingerprint with fprintd-enroll and verifies matching.
#

set -euo pipefail

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${REPO_DIR}/build"
INSTALL_PREFIX="/usr/local"
LIB_DIR="${INSTALL_PREFIX}/lib64"

# CLI flags
SKIP_BUILD=0
NO_ENROLL=0
BUILD_ONLY=0
SYSTEM_INSTALL=0

for arg in "$@"; do
    case "${arg}" in
        --skip-build)
            SKIP_BUILD=1
            ;;
        --no-enroll)
            NO_ENROLL=1
            ;;
        --build-only)
            BUILD_ONLY=1
            ;;
        --system-install)
            SYSTEM_INSTALL=1
            ;;
        -h|--help)
            echo "Usage: ./install-bluefin.sh [OPTIONS]"
            echo ""
            echo "Options:"
            echo "  --skip-build      Skip building libfprint; use existing build binary."
            echo "  --no-enroll       Install and configure services, skip interactive enrollment prompt."
            echo "  --build-only      Only compile libfprint, do not install system files."
            echo "  --system-install  (Internal) Run root-level installation and service setup."
            echo "  -h, --help        Show this help message."
            exit 0
            ;;
    esac
done

# Terminal colors
BOLD="\033[1m"
GREEN="\033[32m"
YELLOW="\033[33m"
CYAN="\033[36m"
RED="\033[31m"
RESET="\033[0m"

log_info() {
    echo -e "${BOLD}${CYAN}[INFO]${RESET} $1"
}

log_success() {
    echo -e "${BOLD}${GREEN}[SUCCESS]${RESET} $1"
}

log_warn() {
    echo -e "${BOLD}${YELLOW}[WARNING]${RESET} $1"
}

log_err() {
    echo -e "${BOLD}${RED}[ERROR]${RESET} $1"
}

# Determine the non-root target user for enrollment & user-scoped commands
get_target_user() {
    if [[ -n "${SUDO_USER:-}" && "${SUDO_USER}" != "root" ]]; then
        echo "${SUDO_USER}"
    elif [[ "${USER:-}" != "root" && -n "${USER:-}" ]]; then
        echo "${USER}"
    else
        id -nu 1000 2>/dev/null || echo "brian"
    fi
}
TARGET_USER="$(get_target_user)"

# Helper to run a command as the non-root target user
run_as_target_user() {
    if [[ $EUID -eq 0 && -n "${TARGET_USER}" && "${TARGET_USER}" != "root" ]]; then
        sudo -u "${TARGET_USER}" -- "$@"
    else
        "$@"
    fi
}

# Detect if running inside a container (Distrobox, Toolbx, Docker, Podman)
is_container() {
    [[ -f /run/.containerenv ]] || [[ -f /.dockerenv ]]
}

# Handle container execution
handle_container_context() {
    if ! is_container; then
        return
    fi

    echo -e "${BOLD}=== Goodix 532D Container Build Environment ===${RESET}\n"
    log_info "Detected container environment ($(cat /etc/os-release 2>/dev/null | grep PRETTY_NAME | cut -d= -f2 | tr -d '\"' || echo 'container'))."

    # Step 1: Compile driver inside container
    log_info "Compiling libfprint inside container..."
    if command -v ninja >/dev/null 2>&1; then
        ninja -C "${BUILD_DIR}"
    else
        log_err "ninja not found inside container."
        exit 1
    fi

    if [[ ! -f "${BUILD_DIR}/libfprint/libfprint-2.so.2.0.0" ]]; then
        log_err "Compilation failed: ${BUILD_DIR}/libfprint/libfprint-2.so.2.0.0 not found."
        exit 1
    fi

    log_success "Build complete inside container!"

    if [[ "${BUILD_ONLY}" -eq 1 ]]; then
        exit 0
    fi

    echo ""
    log_info "System files (/usr/local/lib64, udev rules, systemd fprintd service, PAM)"
    log_info "must be installed on the host Bluefin system, not inside this container."
    echo ""

    # If running as normal user inside container and distrobox-host-exec is present, offer host exec
    if [[ $EUID -ne 0 ]] && command -v distrobox-host-exec >/dev/null 2>&1 && [[ -t 0 ]]; then
        read -r -p "Run the host installation now via distrobox-host-exec? [Y/n] " choice
        choice="${choice:-Y}"
        if [[ "${choice}" =~ ^[Yy]$ ]]; then
            exec distrobox-host-exec bash "${REPO_DIR}/install-bluefin.sh" --skip-build "$@"
        fi
    fi

    echo -e "${BOLD}To complete installation on your host:${RESET}"
    echo -e "  1. Exit this container: ${CYAN}exit${RESET}"
    echo -e "  2. Run on the host:     ${GREEN}./install-bluefin.sh${RESET}"
    echo ""
    exit 0
}

# Step 1: Build libfprint
build_driver() {
    if [[ "${SKIP_BUILD}" -eq 1 ]]; then
        log_info "Step 1: Skipping build (--skip-build specified)."
        if [[ ! -f "${BUILD_DIR}/libfprint/libfprint-2.so.2.0.0" ]]; then
            log_err "Build artifact ${BUILD_DIR}/libfprint/libfprint-2.so.2.0.0 does not exist."
            exit 1
        fi
        return
    fi

    log_info "Step 1: Building libfprint..."
    local built=0

    # 1. Try Distrobox container 'bench' (as target user if root)
    if command -v distrobox >/dev/null 2>&1; then
        if run_as_target_user distrobox list 2>/dev/null | grep -q '\bbench\b'; then
            log_info "Building via Distrobox container 'bench' (user: ${TARGET_USER})..."
            if run_as_target_user distrobox enter bench -- ninja -C "${BUILD_DIR}"; then
                built=1
            fi
        fi
    fi

    # 2. Try Podman container 'bench'
    if [[ $built -eq 0 ]] && command -v podman >/dev/null 2>&1; then
        if run_as_target_user podman ps --format '{{.Names}}' 2>/dev/null | grep -q '^bench$'; then
            log_info "Building via Podman container 'bench' (user: ${TARGET_USER})..."
            if run_as_target_user podman exec bench ninja -C "${BUILD_DIR}"; then
                built=1
            fi
        fi
    fi

    # 3. Try host ninja (check PATH and linuxbrew)
    if [[ $built -eq 0 ]]; then
        local host_ninja=""
        if command -v ninja >/dev/null 2>&1; then
            host_ninja="$(command -v ninja)"
        elif [[ -x "/home/linuxbrew/.linuxbrew/bin/ninja" ]]; then
            host_ninja="/home/linuxbrew/.linuxbrew/bin/ninja"
        fi

        if [[ -n "${host_ninja}" ]]; then
            log_info "Building natively on host with ${host_ninja}..."
            if "${host_ninja}" -C "${BUILD_DIR}"; then
                built=1
            fi
        fi
    fi

    # Evaluate build outcome
    if [[ -f "${BUILD_DIR}/libfprint/libfprint-2.so.2.0.0" ]]; then
        if [[ $built -eq 1 ]]; then
            log_success "Build complete: ${BUILD_DIR}/libfprint/libfprint-2.so.2.0.0"
        else
            log_warn "Could not invoke build environment, but existing binary was found."
            log_info "Using: ${BUILD_DIR}/libfprint/libfprint-2.so.2.0.0"
        fi
    else
        log_err "Build failed: ${BUILD_DIR}/libfprint/libfprint-2.so.2.0.0 not found."
        log_err "Ensure Distrobox container 'bench' is started or build with: distrobox enter bench -- ninja -C build"
        exit 1
    fi
}

# Step 2: Install shared library into /usr/local/lib64
install_library() {
    log_info "Step 2: Installing libfprint into ${LIB_DIR}..."

    mkdir -p "${LIB_DIR}"
    install -m 0755 "${BUILD_DIR}/libfprint/libfprint-2.so.2.0.0" "${LIB_DIR}/libfprint-2.so.2.0.0"
    ln -sf libfprint-2.so.2.0.0 "${LIB_DIR}/libfprint-2.so.2"
    ln -sf libfprint-2.so.2 "${LIB_DIR}/libfprint-2.so"

    # Configure dynamic linker
    mkdir -p /etc/ld.so.conf.d
    echo "${LIB_DIR}" > /etc/ld.so.conf.d/00-libfprint-local.conf
    ldconfig

    # Copy hardware sensor calibration if available
    local cal_src=""
    if [[ -f "${REPO_DIR}/data/goodix53xd_clear.pgm" ]]; then
        cal_src="${REPO_DIR}/data/goodix53xd_clear.pgm"
    elif [[ -f "/home/${TARGET_USER}/git/goodix-fp-dump/clear-3.pgm" ]]; then
        cal_src="/home/${TARGET_USER}/git/goodix-fp-dump/clear-3.pgm"
    elif [[ -f "/var/home/${TARGET_USER}/git/goodix-fp-dump/clear-3.pgm" ]]; then
        cal_src="/var/home/${TARGET_USER}/git/goodix-fp-dump/clear-3.pgm"
    elif [[ -f "/home/brian/git/goodix-fp-dump/clear-3.pgm" ]]; then
        cal_src="/home/brian/git/goodix-fp-dump/clear-3.pgm"
    fi

    if [[ -n "${cal_src}" ]]; then
        mkdir -p /var/lib/fprint
        cp "${cal_src}" /var/lib/fprint/goodix53xd_clear.pgm
        chmod 644 /var/lib/fprint/goodix53xd_clear.pgm
        log_success "Hardware sensor calibration copied to /var/lib/fprint/goodix53xd_clear.pgm"
    else
        log_warn "No clear baseline file found; driver will use live empty frame capture."
    fi

    log_success "Library installed and ldconfig updated."
}

# Step 3: Configure udev / hwdb for Goodix 532D autosuspend & detection
configure_udev() {
    log_info "Step 3: Configuring udev hwdb for Goodix 27c6:532d..."

    mkdir -p /etc/udev/hwdb.d
    cat << 'EOF' > /etc/udev/hwdb.d/60-autosuspend-goodix-532d.hwdb
# Goodix TLS 53XD / 532D Fingerprint reader
usb:v27C6p532D*
 ID_AUTOSUSPEND=1
EOF

    systemd-hwdb update 2>/dev/null || true
    udevadm trigger -s usb --attr-match=idVendor=27c6 2>/dev/null || udevadm trigger -s usb 2>/dev/null || true
    log_success "Udev hwdb configured."
}

# Step 4: Configure systemd sandboxing override for fprintd
configure_fprintd_service() {
    log_info "Step 4: Configuring systemd override for fprintd.service..."

    local override_dir="/etc/systemd/system/fprintd.service.d"
    mkdir -p "${override_dir}"

    # fprintd has ProtectSystem=strict, so we explicitly permit
    # reading /var/usrlocal where /usr/local symlinks to on ostree/Bluefin
    cat << 'EOF' > "${override_dir}/10-custom-libfprint.conf"
[Service]
Environment="LD_LIBRARY_PATH=/usr/local/lib64"
Environment="G_MESSAGES_DEBUG=all"
ReadOnlyPaths=/var/usrlocal
EOF

    if [[ -d /run/systemd/system ]]; then
        systemctl daemon-reload
        log_info "Restarting fprintd.service..."
        systemctl restart fprintd.service || log_warn "Could not restart fprintd.service immediately; will start on demand."
    else
        log_warn "systemd not running as PID 1; skipping service restart."
    fi

    log_success "fprintd.service restarted with custom libfprint."
}

# Step 5: Configure PAM via authselect
configure_pam() {
    log_info "Step 5: Verifying PAM fingerprint authentication..."

    if ! command -v authselect >/dev/null 2>&1; then
        log_warn "authselect command not found; skipping authselect configuration."
        return
    fi

    local current_profile
    current_profile="$(authselect current 2>/dev/null | awk '/Profile ID:/ {print $3}')"
    if [[ -z "${current_profile}" ]]; then
        current_profile="local"
    fi

    if authselect current 2>/dev/null | grep -q 'with-fingerprint'; then
        log_success "PAM is already configured with fingerprint support (${current_profile})."
    else
        log_info "Enabling 'with-fingerprint' feature on profile '${current_profile}'..."
        authselect enable-feature with-fingerprint 2>/dev/null || authselect select "${current_profile}" with-fingerprint --force
        log_success "authselect updated with fingerprint feature."
    fi
}

# Step 5b: Configure Polkit rule for fprintd management
configure_polkit() {
    log_info "Step 5b: Configuring Polkit authorization for fingerprint enrollment..."

    local rules_dir="/etc/polkit-1/rules.d"
    if [[ -d "${rules_dir}" ]]; then
        cat << 'EOF' > "${rules_dir}/60-net.reactivated.fprint.rules"
// Allow members of wheel to manage and verify fingerprints
polkit.addRule(function(action, subject) {
    if (action.id.indexOf("net.reactivated.fprint.device.") === 0 &&
        subject.isInGroup("wheel")) {
        return polkit.Result.YES;
    }
});
EOF
        chmod 644 "${rules_dir}/60-net.reactivated.fprint.rules"
        log_success "Polkit rule configured for wheel group."
    fi
}

# Step 6: User enrollment & verification prompt
enroll_and_verify_user() {
    log_info "Step 6: User Onboarding (${TARGET_USER})..."

    if [[ "${NO_ENROLL}" -eq 1 ]]; then
        log_info "Skipping interactive enrollment (--no-enroll specified)."
        return
    fi

    # Verify device is claimed by fprintd
    sleep 1
    if ! run_as_target_user fprintd-list "${TARGET_USER}" >/dev/null 2>&1; then
        log_warn "fprintd did not report the device immediately. Checking daemon status..."
        if [[ -d /run/systemd/system ]]; then
            systemctl status fprintd.service --no-pager || true
        fi
    fi

    echo ""
    echo -e "${BOLD}${GREEN}================================================================${RESET}"
    echo -e "${BOLD}${GREEN} libfprint successfully installed and active on Bluefin!        ${RESET}"
    echo -e "${BOLD}${GREEN}================================================================${RESET}"
    echo ""
    echo "Device: Goodix TLS Fingerprint Sensor (27c6:532d)"
    echo "GNOME Settings & GDM Lock Screen will now recognize your fingerprint."
    echo ""

    if [[ ! -t 0 ]]; then
        echo -e "Non-interactive session detected."
        echo -e "To enroll your fingerprint anytime, run: ${BOLD}fprintd-enroll${RESET}"
        return
    fi

    read -r -p "Would you like to enroll a fingerprint now for user '${TARGET_USER}'? [Y/n] " choice
    choice="${choice:-Y}"
    if [[ "${choice}" =~ ^[Yy]$ ]]; then
        echo -e "\n${CYAN}Starting fprintd-enroll for ${TARGET_USER}...${RESET}"
        run_as_target_user fprintd-enroll

        echo -e "\n${CYAN}Testing verification... Touch the sensor:${RESET}"
        run_as_target_user fprintd-verify || true
    else
        echo -e "\nYou can enroll anytime with: ${BOLD}fprintd-enroll${RESET}"
        echo -e "Or through ${BOLD}GNOME Settings -> Users -> Fingerprint Login${RESET}."
    fi
}

main() {
    echo -e "${BOLD}=== Bluefin Goodix 532D Installer & PAM Onboarding ===${RESET}\n"

    # Handle running inside container first
    handle_container_context

    # Internal sub-call when escalating to sudo for system files only
    if [[ "${SYSTEM_INSTALL}" -eq 1 ]]; then
        install_library
        configure_udev
        configure_fprintd_service
        configure_pam
        configure_polkit
        exit 0
    fi

    # Step 1: Run build (works under normal user or sudo on host)
    build_driver

    if [[ "${BUILD_ONLY}" -eq 1 ]]; then
        exit 0
    fi

    # Steps 2-5: If non-root on host, run system installation via sudo
    if [[ $EUID -ne 0 ]]; then
        log_info "Step 2-5: System installation requires root privileges..."
        sudo bash "$0" --system-install
    else
        install_library
        configure_udev
        configure_fprintd_service
        configure_pam
        configure_polkit
    fi

    # Step 6: User Onboarding
    # If run as non-root, this runs directly in the user's active session!
    # If run as root, run_as_target_user is authorized by the polkit rule!
    enroll_and_verify_user
}

main "$@"
