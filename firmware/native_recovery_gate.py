"""Recovery requires a validated terminal-stop packet, not missing networking.

Input is the same trial's CRC/quality-validated power_trace decoder result.
The existing finisher still checks fresh trace identity and remote availability.
"""


def known_terminal_stop(decoded):
    if not isinstance(decoded, dict):
        return False
    packet = decoded.get('packet')
    if not isinstance(packet, dict) or packet.get('decoded') is not True:
        return False
    code = packet.get('code')
    if type(code) is not int:
        return False
    category, detail = code >> 8, code & 255
    # Other bootstrap aborts attempt a GameOS reboot after reporting. Their
    # later state can include startup writes and is not a proven static halt.
    return (category == 8 or code == 0x300 or
            category == 4 and (detail <= 31 or detail == 255))
