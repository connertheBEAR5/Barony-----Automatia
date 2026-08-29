#!/usr/bin/env python3
"""Add regular-Mimic mouth voxels to the existing Mini Mimic shell slabs.

Barony's ``.vox`` files use the old slab layout: three little-endian int32
dimensions, one palette-index byte per voxel (x-major, then y, then z), and a
256-entry RGB palette.  This generator deliberately does not replace or
rescale the authored Mini Mimic shell.  It preserves every original shell
voxel and fills only empty cells with red/purple mouth colors sampled from the
matching regular Mimic asset.

The generated files remain the existing model entries 1794/1795.  That keeps
the model table stable and lets runtime code offer both appearances without
registering another monster or shifting any asset indices.

The operation is idempotent.  Palette slots 240..254 are reserved for generated
interior colors; rerunning first removes only voxels using those slots and then
verifies that the original shell is still byte-identical before rebuilding it.
"""

from __future__ import annotations

import argparse
from collections import Counter
from dataclasses import dataclass
import hashlib
from pathlib import Path
import struct
from typing import Iterable


EMPTY_VOXEL = 255
GENERATED_PALETTE_FIRST = 240
GENERATED_PALETTE_LAST = 254


@dataclass
class Slab:
	size_x: int
	size_y: int
	size_z: int
	voxels: bytearray
	palette: bytearray

	def index(self, x: int, y: int, z: int) -> int:
		return z + y * self.size_z + x * self.size_y * self.size_z

	def to_bytes(self) -> bytes:
		return (
			struct.pack("<iii", self.size_x, self.size_y, self.size_z)
			+ bytes(self.voxels)
			+ bytes(self.palette)
		)


@dataclass(frozen=True)
class AssetPair:
	name: str
	mini_relative: Path
	regular_relative: Path
	original_mini_sha256: str
	regular_sha256: str


ASSETS = (
	AssetPair(
		name="trunk",
		mini_relative=Path("models/creatures/minimimic/MinimimicTrunk.vox"),
		regular_relative=Path("models/creatures/mimic/MimicTrunk.vox"),
		original_mini_sha256=(
			"751886c6e8f491a28ee1f6dfaf70a45e6c8316a83ee39138570303ac09965b0e"
		),
		regular_sha256=(
			"1e00b81c88b8e6974a0979c14e3b0235f1cfd9b0110bd52e4963a97ba117667a"
		),
	),
	AssetPair(
		name="lid",
		mini_relative=Path("models/creatures/minimimic/MinimimicLid.vox"),
		regular_relative=Path("models/creatures/mimic/MimicLid.vox"),
		original_mini_sha256=(
			"473046fd33f7b3422ba580707d9cdeb33b2d18ba016d798d13ff9d2d0bfd0c3c"
		),
		regular_sha256=(
			"dd39c35945c97bedbaeceb095a66aaadc917884d43d714bad237217e1a876b1f"
		),
	),
)


def sha256(data: bytes) -> str:
	return hashlib.sha256(data).hexdigest()


def read_slab(path: Path) -> Slab:
	data = path.read_bytes()
	if len(data) < 12:
		raise ValueError(f"{path}: too short to be a Barony slab")
	size_x, size_y, size_z = struct.unpack("<iii", data[:12])
	voxel_count = size_x * size_y * size_z
	expected_size = 12 + voxel_count + 256 * 3
	if min(size_x, size_y, size_z) <= 0 or len(data) != expected_size:
		raise ValueError(
			f"{path}: expected {expected_size} bytes for "
			f"{size_x}x{size_y}x{size_z}, found {len(data)}"
		)
	return Slab(
		size_x,
		size_y,
		size_z,
		bytearray(data[12 : 12 + voxel_count]),
		bytearray(data[12 + voxel_count :]),
	)


def generated_palette_index(value: int) -> bool:
	return GENERATED_PALETTE_FIRST <= value <= GENERATED_PALETTE_LAST


def reconstruct_original_shell(generated_or_original: Slab) -> Slab:
	"""Remove only this generator's reserved colors and restore their palette."""
	clean = Slab(
		generated_or_original.size_x,
		generated_or_original.size_y,
		generated_or_original.size_z,
		bytearray(generated_or_original.voxels),
		bytearray(generated_or_original.palette),
	)
	for index, value in enumerate(clean.voxels):
		if generated_palette_index(value):
			clean.voxels[index] = EMPTY_VOXEL
	for palette_index in range(
		GENERATED_PALETTE_FIRST, GENERATED_PALETTE_LAST + 1
	):
		start = palette_index * 3
		clean.palette[start : start + 3] = b"\0\0\0"
	return clean


def palette_rgb(slab: Slab, palette_index: int) -> tuple[int, int, int]:
	start = palette_index * 3
	return tuple(slab.palette[start : start + 3])  # type: ignore[return-value]


def is_mimic_interior_color(rgb: tuple[int, int, int]) -> bool:
	"""Select flesh/gum reds and purple, excluding brown wood and gray metal."""
	r, g, b = rgb
	strong_red = r >= 8 and r >= 2 * g + 10 and r >= 2 * b + 10
	purple_gum = r >= 8 and b >= 8 and g <= 2
	return strong_red or purple_gum


def sample_range(destination: int, destination_size: int, source_size: int) -> range:
	start = destination * source_size // destination_size
	end = ((destination + 1) * source_size + destination_size - 1) // destination_size
	return range(start, max(start + 1, end))


def choose_interior_color(
	regular: Slab,
	x_values: Iterable[int],
	y_values: Iterable[int],
	z_values: Iterable[int],
) -> tuple[int, int, int] | None:
	colors: Counter[tuple[int, int, int]] = Counter()
	for x in x_values:
		for y in y_values:
			for z in z_values:
				value = regular.voxels[regular.index(x, y, z)]
				if value == EMPTY_VOXEL:
					continue
				rgb = palette_rgb(regular, value)
				if is_mimic_interior_color(rgb):
					colors[rgb] += 1
	if not colors:
		return None
	# Stable tie breaking keeps generated assets deterministic across Python versions.
	return sorted(colors.items(), key=lambda item: (-item[1], item[0]))[0][0]


def add_interior(shell: Slab, regular: Slab) -> tuple[Slab, int]:
	result = reconstruct_original_shell(shell)
	occupied_z = [
		z
		for z in range(result.size_z)
		if any(
			result.voxels[result.index(x, y, z)] != EMPTY_VOXEL
			for x in range(result.size_x)
			for y in range(result.size_y)
		)
	]
	if not occupied_z:
		raise ValueError("Mini Mimic shell contains no authored voxels")
	first_z = min(occupied_z)
	target_depth = result.size_z - first_z

	chosen: dict[tuple[int, int, int], int] = {}
	additions: list[tuple[int, tuple[int, int, int]]] = []
	for x in range(result.size_x):
		for y in range(result.size_y):
			for z in range(first_z, result.size_z):
				index = result.index(x, y, z)
				if result.voxels[index] != EMPTY_VOXEL:
					continue
				rgb = choose_interior_color(
					regular,
					sample_range(x, result.size_x, regular.size_x),
					sample_range(y, result.size_y, regular.size_y),
					sample_range(z - first_z, target_depth, regular.size_z),
				)
				if rgb is not None:
					additions.append((index, rgb))
					chosen.setdefault(rgb, 0)

	if len(chosen) > GENERATED_PALETTE_LAST - GENERATED_PALETTE_FIRST + 1:
		raise ValueError(f"interior needs {len(chosen)} reserved palette slots")
	for offset, rgb in enumerate(sorted(chosen)):
		palette_index = GENERATED_PALETTE_FIRST + offset
		chosen[rgb] = palette_index
		start = palette_index * 3
		result.palette[start : start + 3] = bytes(rgb)
	for index, rgb in additions:
		result.voxels[index] = chosen[rgb]
	return result, len(additions)


def generate(data_root: Path, output_root: Path) -> None:
	for asset in ASSETS:
		mini_path = data_root / asset.mini_relative
		regular_path = data_root / asset.regular_relative
		mini = read_slab(mini_path)
		regular_bytes = regular_path.read_bytes()
		if sha256(regular_bytes) != asset.regular_sha256:
			raise ValueError(
				f"{regular_path}: regular Mimic source changed; audit the resampling "
				"before regenerating Mini Mimic artwork"
			)
		regular = read_slab(regular_path)

		original_shell = reconstruct_original_shell(mini)
		if sha256(original_shell.to_bytes()) != asset.original_mini_sha256:
			raise ValueError(
				f"{mini_path}: does not match the original shell or a prior "
				"idempotent generator result"
			)

		generated, added = add_interior(original_shell, regular)
		output_path = output_root / asset.mini_relative
		output_path.parent.mkdir(parents=True, exist_ok=True)
		output = generated.to_bytes()
		output_path.write_bytes(output)
		print(
			f"{asset.name}: preserved original shell, added {added} interior "
			f"voxels, sha256={sha256(output)}"
		)


def main() -> None:
	parser = argparse.ArgumentParser(description=__doc__)
	repository = Path(__file__).resolve().parents[1]
	parser.add_argument(
		"--data-root",
		type=Path,
		default=repository / "build",
		help="directory containing models/ (default: repository build directory)",
	)
	parser.add_argument(
		"--output-root",
		type=Path,
		help="alternate output root; defaults to updating --data-root in place",
	)
	arguments = parser.parse_args()
	output_root = arguments.output_root or arguments.data_root
	generate(arguments.data_root.resolve(), output_root.resolve())


if __name__ == "__main__":
	main()
