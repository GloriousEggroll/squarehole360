"""Read-only diagnostic decoder, never imported by the game.

Decode only the versioned 120-byte gpu_metrics_v1_3 ABI from
https://github.com/torvalds/linux/blob/v6.19/drivers/gpu/drm/amd/include/kgd_pp_interface.h
Other versions retain their raw bytes and header without speculative decoding.
SMU14 reports its average clock in both average_gfxclk and current_gfxclk;
those two values are not independent instantaneous clock measurements.
"""
import struct
import sys


def decode_metrics(raw):
    result = {"raw_hex": raw.hex(), "bytes": len(raw), "status": "unsupported"}
    if len(raw) < 4:
        result["status"] = "truncated_header"
        return result
    size, major, minor = struct.unpack_from("<HBB", raw)
    result.update(structure_size=size, format_revision=major, content_revision=minor)
    if sys.byteorder != "little" or (size, major, minor, len(raw)) != (120, 1, 3, 120):
        return result

    def value(offset, fmt):
        number = struct.unpack_from("<" + fmt, raw, offset)[0]
        # Kernel initialization uses all bits set for unavailable fields.
        return None if number == (1 << (8 * struct.calcsize(fmt))) - 1 else number

    fields = {}
    for offset, name in enumerate((
        "temperature_edge_c", "temperature_hotspot_c", "temperature_mem_c",
        "temperature_vrgfx_c", "temperature_vrsoc_c", "temperature_vrmem_c",
        "average_gfx_activity_percent", "average_umc_activity_percent",
        "average_mm_activity_percent", "average_socket_power_w",
    )):
        fields[name] = value(4 + offset * 2, "H")
    for offset, name in enumerate((
        "average_gfxclk_mhz", "average_socclk_mhz", "average_uclk_mhz",
        "average_vclk0_mhz", "average_dclk0_mhz", "average_vclk1_mhz",
        "average_dclk1_mhz", "current_gfxclk_mhz", "current_socclk_mhz",
        "current_uclk_mhz", "current_vclk0_mhz", "current_dclk0_mhz",
        "current_vclk1_mhz", "current_dclk1_mhz",
    )):
        fields[name] = value(40 + offset * 2, "H")
    for name, offset, fmt in (
        ("energy_accumulator", 24, "Q"), ("system_clock_counter_ns", 32, "Q"),
        ("throttle_status", 68, "I"), ("current_fan_rpm", 72, "H"),
        ("pcie_link_width", 74, "H"), ("pcie_link_speed_0_1_gt_s", 76, "H"),
        ("gfx_activity_acc", 80, "I"), ("mem_activity_acc", 84, "I"),
        ("firmware_timestamp_10ns", 96, "Q"), ("voltage_soc_mv", 104, "H"),
        ("voltage_gfx_mv", 106, "H"), ("voltage_mem_mv", 108, "H"),
        ("indep_throttle_status", 112, "Q"),
    ):
        fields[name] = value(offset, fmt)
    result.update(status="decoded_v1_3", fields=fields)
    return result


def read_metrics(path):
    try:
        # Bound even malformed or future sysfs tables. No writes or ioctls.
        with path.open("rb") as stream:
            raw = stream.read(4097)
        if len(raw) > 4096:
            return {"status": "table_too_large"}
        return decode_metrics(raw)
    except OSError as error:
        return {"status": "unavailable", "errno": error.errno}
