#!/bin/bash

TOP_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" &> /dev/null && pwd)
cd ${TOP_DIR}

MILKV_ACTION="all"

function show_info()
{
  printf "\e[1;32m%s\e[0m\n" "$1"
}

function show_err()
{
  printf "\e[1;31mError: %s\e[0m\n" "$1"
}

function milkv_clean_stale_images()
{
  if [ ! -d "${OUTPUT_DIR}" ]; then
    return 0
  fi

  pushd "${OUTPUT_DIR}" > /dev/null || return 0
  rm -f *.img* upgrade.zip upgrade_ota.zip 2>/dev/null
  popd > /dev/null
}

function milkv_pack_and_report()
{
  milkv_pack
  if [ $? -eq 0 ]; then
    show_info "Build board ${MILKV_BOARD} success!"
    milkv_print_firmware_version
  else
    show_err "Build board ${MILKV_BOARD} failed!"
    exit 1
  fi
}

function milkv_build()
{
  milkv_clean_stale_images
  build_all
  if [ $? -ne 0 ]; then
    show_err "Build board ${MILKV_BOARD} failed!"
    exit 1
  fi
}

function milkv_pack_sd()
{
  pack_sd_image

  [ ! -d out ] && mkdir out

  img_in="${OUTPUT_DIR}/${MILKV_BOARD}.img"
  img_out="${MILKV_BOARD}_`date +%Y-%m%d-%H%M`.img"

  if [ -f "${img_in}" ]; then
    mv ${img_in} out/${img_out}
    show_info "Create SD image successful: out/${img_out}"
  else
    show_err "Create SD image failed!"
    exit 1
  fi
}

function milkv_pack_emmc()
{
  [ ! -d out ] && mkdir out

  stamp="`date +%Y-%m%d-%H%M`"
  img_in="${OUTPUT_DIR}/upgrade.zip"
  img_out="${MILKV_BOARD}_${stamp}.zip"
  ota_in="${OUTPUT_DIR}/upgrade_ota.zip"
  ota_out="${MILKV_BOARD}_ota_${stamp}.zip"

  if [ -f "${img_in}" ]; then
    mv ${img_in} out/${img_out}
    show_info "Create eMMC image successful: out/${img_out}"
    if [ -f "${ota_in}" ]; then
      mv ${ota_in} out/${ota_out}
      show_info "Create eMMC OTA image successful: out/${ota_out}"
    fi
  else
    show_err "Create eMMC image failed!"
    exit 1
  fi
}

function milkv_pack_nor_nand()
{
  [ ! -d out ] && mkdir out

  if [ -f "${OUTPUT_DIR}/upgrade.zip" ]; then
    img_out_patch=${MILKV_BOARD}-`date +%Y%m%d-%H%M`
    mkdir -p out/$img_out_patch

    if [ "${STORAGE_TYPE}" == "spinor" ]; then
        cp ${OUTPUT_DIR}/fip.bin out/$img_out_patch
        cp ${OUTPUT_DIR}/*.spinor out/$img_out_patch
    else
        cp ${OUTPUT_DIR}/fip.bin out/$img_out_patch
        cp ${OUTPUT_DIR}/*.spinand out/$img_out_patch
    fi

    echo "Copy all to a blank tf card, power on and automatically download firmware to NOR or NAND in U-boot." >> out/$img_out_patch/how_to_download.txt
    show_info "Create spinor/nand img successful: ${img_out_patch}"
  else
    show_err "Create spinor/nand img failed!"
    exit 1
  fi
}

function milkv_pack()
{
  if [ "${STORAGE_TYPE}" == "sd" ]; then
    milkv_pack_sd
  elif [ "${STORAGE_TYPE}" == "emmc" ]; then
    milkv_pack_emmc
  else
    milkv_pack_nor_nand
  fi
}

function list_boards()
{
  for board in "${MILKV_BOARD_ARRAY[@]}"; do
    show_info "$board"
  done
}

function get_toolchain()
{
  if [ ! -d host-tools ]; then
    show_info "Toolchain does not exist, download it now..."

    toolchain_url="https://github.com/milkv-duo/host-tools.git"
    echo "toolchain_url: ${toolchain_url}"

    git clone ${toolchain_url}
    if [ $? -ne 0 ]; then
      show_err "Failed to download ${toolchain_url} !"
      exit 1
    fi
  fi
}

function milkv_is_valid_action()
{
  case "$1" in
    all|clean|distclean|kernel|uboot|rootfs|middleware|osdrv|pack)
      return 0
      ;;
    *)
      return 1
      ;;
  esac
}

function build_usage()
{
  echo "Usage:"
  echo "${BASH_SOURCE[0]}                          - Show this menu"
  echo "${BASH_SOURCE[0]} lunch                    - Select a board to build"
  echo "${BASH_SOURCE[0]} [board]                  - Incremental full build (default)"
  echo "${BASH_SOURCE[0]} [board] all               - Same as incremental full build"
  echo "${BASH_SOURCE[0]} [board] clean             - Clean build artifacts only"
  echo "${BASH_SOURCE[0]} [board] distclean         - Deep clean build artifacts"
  echo "${BASH_SOURCE[0]} [board] kernel            - Build kernel and pack image"
  echo "${BASH_SOURCE[0]} [board] uboot             - Build u-boot and pack image"
  echo "${BASH_SOURCE[0]} [board] rootfs            - Build rootfs and pack image"
  echo "${BASH_SOURCE[0]} [board] middleware        - Build middleware only"
  echo "${BASH_SOURCE[0]} [board] osdrv             - Build osdrv only"
  echo "${BASH_SOURCE[0]} [board] pack              - Repack image only"
  echo ""
  echo "Notes:"
  echo "  Default build is incremental. Run 'clean' or 'distclean' after toolchain,"
  echo "  defconfig, or partition changes."
  echo ""
  echo "${BASH_SOURCE[0]} zonhor                     - Build zonhor-sg2000-glibc-arm64-emmc"
  echo "${BASH_SOURCE[0]} zonhor-nand                - Build zonhor-sg2000-glibc-arm64-nand"
  echo "Supported boards:"
  list_boards
}

function milkv_run_action()
{
  local action="${1:-all}"

  if ! milkv_is_valid_action "${action}"; then
    show_err "Unknown action: ${action}"
    build_usage
    exit 1
  fi

  case "${action}" in
    all)
      milkv_build
      milkv_pack_and_report
      ;;
    clean)
      clean_all
      show_info "Clean board ${MILKV_BOARD} success!"
      ;;
    distclean)
      distclean_all
      show_info "Distclean board ${MILKV_BOARD} success!"
      ;;
    kernel)
      build_kernel || exit 1
      pack_upgrade || exit 1
      milkv_pack_and_report
      ;;
    uboot)
      build_uboot || exit 1
      pack_upgrade || exit 1
      milkv_pack_and_report
      ;;
    rootfs)
      pack_rootfs || exit 1
      pack_upgrade || exit 1
      milkv_pack_and_report
      ;;
    middleware)
      build_middleware || exit 1
      show_info "Build middleware for ${MILKV_BOARD} success!"
      ;;
    osdrv)
      build_osdrv || exit 1
      show_info "Build osdrv for ${MILKV_BOARD} success!"
      ;;
    pack)
      pack_upgrade || exit 1
      milkv_pack_and_report
      ;;
  esac
}

if [ $# -ge 1 ]; then
  if [ "$1" = "lunch" ]; then
    source ${TOP_DIR}/build/envsetup_milkv.sh lunch || exit 1
  else
    MILKV_BOARD_ARG="$1"
    if [ "$MILKV_BOARD_ARG" = "zonhor" ] || [ "$MILKV_BOARD_ARG" = "zonhor-sg2000" ]; then
      MILKV_BOARD_ARG="zonhor-sg2000-glibc-arm64-emmc"
    elif [ "$MILKV_BOARD_ARG" = "zonhor-nand" ]; then
      MILKV_BOARD_ARG="zonhor-sg2000-glibc-arm64-nand"
    fi
    if [ $# -ge 2 ]; then
      MILKV_ACTION="$2"
    fi
    source ${TOP_DIR}/build/envsetup_milkv.sh "list" || exit 1
    if [[ ${MILKV_BOARD_ARRAY[@]} =~ (^|[[:space:]])"${MILKV_BOARD_ARG}"($|[[:space:]]) ]]; then
      check_board ${MILKV_BOARD_ARG} || exit $?
      build_info || exit $?
    else
      show_err "${MILKV_BOARD_ARG} not supported!"
      echo "Available boards:"
      list_boards
      exit $?
    fi
  fi
else
  source ${TOP_DIR}/build/envsetup_milkv.sh list || exit 1
  build_usage && exit 0
fi

get_toolchain

milkv_run_action "${MILKV_ACTION}"
