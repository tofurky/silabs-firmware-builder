#!/usr/bin/env python3
"""Tool to create a JSON manifest file for a collection of firmwares."""

from __future__ import annotations

import argparse
import hashlib
import json
import logging
import pathlib
import re
import sys
from datetime import UTC, datetime

from pygbl import (
    FirmwareImage,
    GBL3Header,
    GBL3Image,
    GBL3Type,
    GBLError,
    parse_firmware_image,
    read_encryption_key,
)

_LOGGER = logging.getLogger(__name__)

# A firmware's `fw_type` names its source directory, except where the two have drifted
SOURCE_DIRS = {
    "gecko-bootloader": "bootloader",
    "zwave_ncp": "zwa2_controller",
}


def parse_markdown_changelog(text: str) -> list[dict[str, str | None]]:
    """Parse a changelog into an ordered list of entries, newest first."""
    entries = []
    chunks = re.split(r"^# (.*?)\n", text, flags=re.MULTILINE)[1:]

    for version, raw_text in zip(chunks[::2], chunks[1::2]):
        first_line, rest = raw_text.split("\n", 1)

        if len(first_line) > 255:
            raise ValueError(
                "First line of every changelog must be less than 255 characters"
            )

        entries.append(
            {
                "version": version,
                "summary": first_line,
                "notes": rest.strip() or None,
            }
        )

    return entries


def get_firmware_version(metadata: dict) -> str | None:
    """Extract the firmware version from its metadata, if it is versioned at all."""
    version_keys = {k for k in metadata if k.endswith("_version")} - {
        "sdk_version",
        "metadata_version",
    }

    # Some firmwares, such as the Zigbee router, carry no version of their own
    if not version_keys:
        return None

    (version_key,) = version_keys

    return metadata[version_key]


def maybe_decrypt_firmware(firmware: FirmwareImage, keys: list[bytes]) -> FirmwareImage:
    """Decrypt an encrypted image to access metadata."""
    if not isinstance(firmware, GBL3Image):
        return firmware

    if GBL3Type.ENCRYPTION_AESCCM not in firmware.get_first_tag(GBL3Header).type:
        return firmware

    # The image does not identify its key, so every key is tried until one decrypts
    for key in keys:
        try:
            return firmware.decrypt(key)
        except GBLError:
            continue

    raise ValueError("Image is encrypted, pass a matching --encryption-key")


def main():
    parser = argparse.ArgumentParser(
        formatter_class=argparse.ArgumentDefaultsHelpFormatter
    )
    parser.add_argument(
        "firmware_dir",
        type=pathlib.Path,
        help="Directory containing firmware images",
    )
    parser.add_argument(
        "source_dir",
        type=pathlib.Path,
        help="Directory containing the source tree to identify changelogs",
    )
    parser.add_argument(
        "--encryption-key",
        type=pathlib.Path,
        action="append",
        default=[],
        help="Path to an image encryption key token file, for encrypted GBL files. Can be passed more than once",
    )

    args = parser.parse_args()

    keys = [read_encryption_key(p.read_text()) for p in args.encryption_key]

    manifest = {
        "metadata": {
            "created_at": datetime.now(UTC).isoformat(),
        },
        "changelogs": {},
        "firmwares": [],
    }

    for firmware_file in sorted(args.firmware_dir.glob("*.gbl")):
        data = firmware_file.read_bytes()

        try:
            firmware = parse_firmware_image(data)
        except GBLError:
            _LOGGER.warning("Ignoring invalid firmware file: %s", firmware_file)
            continue

        raw_metadata = maybe_decrypt_firmware(firmware, keys).get_metadata()
        metadata = json.loads(raw_metadata) if raw_metadata is not None else None

        manifest["firmwares"].append(
            {
                "filename": firmware_file.name,
                "version": get_firmware_version(metadata) if metadata else None,
                "checksum": f"sha3-256:{hashlib.sha3_256(data).hexdigest()}",
                "size": len(data),
                "metadata": metadata,
                "release_notes": None,
                "release_summary": None,
            }
        )

    missing_changelogs = False
    changelogs: dict[str, list[dict[str, str | None]]] = {}

    for fw in manifest["firmwares"]:
        if fw["metadata"] is None:
            continue

        fw_type = fw["metadata"]["fw_type"]

        changelog_md = (
            args.source_dir / SOURCE_DIRS.get(fw_type, fw_type) / "CHANGELOG.md"
        )

        if fw_type not in changelogs:
            if changelog_md.exists():
                changelogs[fw_type] = parse_markdown_changelog(changelog_md.read_text())
            else:
                changelogs[fw_type] = []

        if not changelogs[fw_type]:
            continue

        entry = next(
            (e for e in changelogs[fw_type] if e["version"] == fw["version"]), None
        )

        if entry is None:
            _LOGGER.error(
                "Firmware %s version %s has no changelog entry in %s",
                fw["filename"],
                fw["version"],
                changelog_md,
            )
            missing_changelogs = True
            continue

        # These two fields are kept for backwards compatibility with older clients that
        # predate `changelogs`. Their names are inverted: `release_notes` holds the
        # one-line summary and `release_summary` holds the detailed body.
        fw["release_notes"] = entry["summary"]
        fw["release_summary"] = entry["notes"]

    manifest["changelogs"] = {t: e for t, e in changelogs.items() if e}

    if missing_changelogs:
        sys.exit(1)

    print(json.dumps(manifest, indent=2))


if __name__ == "__main__":
    main()
