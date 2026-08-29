#!/usr/bin/env python3
"""Fit regular-Mimic mouth surfaces into the existing Mini Mimic shell slabs.

Barony's ``.vox`` files use the old slab layout: three little-endian int32
dimensions, one palette-index byte per voxel (x-major, then y, then z), and a
256-entry RGB palette.  This generator deliberately does not replace or
rescale the authored Mini Mimic shell.  It preserves every original shell
voxel's geometry, relines only the inward-facing cells selected by the matching
regular Mimic asset, and fills matching empty cavity cells with flesh/gum/teeth.
The exterior silhouette and all non-mouth cells remain the authored Mini asset.

The generated files remain the existing model entries 1794/1795.  That keeps
the model table stable and lets runtime code offer both appearances without
registering another monster or shifting any asset indices.

The operation is idempotent and reversible.  Palette slots 208..223 encode a
flesh relining plus the original Mini palette index; 224..239 do the same for
teeth; 240..254 hold added cavity voxels.  Rerunning restores relined cells to
their original palette indices, removes only added voxels, and verifies that
the original shell is byte-identical before rebuilding it.
"""

from __future__ import annotations

import argparse
from collections import Counter, deque
from dataclasses import dataclass
import hashlib
from pathlib import Path
import struct
from typing import Iterable


EMPTY_VOXEL = 255
# A Mini shell uses only palette indices 0..15. Relining ranges encode that
# original index in their low nibble so reconstruction remains lossless.
ORIGINAL_SHELL_PALETTE_ENTRIES = 16
RELINED_FLESH_PALETTE_FIRST = 208
RELINED_FLESH_PALETTE_LAST = 223
RELINED_TOOTH_PALETTE_FIRST = 224
RELINED_TOOTH_PALETTE_LAST = 239
# Added geometry has no original palette index and is removed on reconstruction.
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
	expected_added: int
	expected_relined: int
	expected_teeth: int
	interior_z_direction: int
	minimum_interior_facing: int


@dataclass(frozen=True)
class InteriorSample:
	kind: str
	rgb: tuple[int, int, int]
	interior_count: int
	shell_count: int


@dataclass(frozen=True)
class GenerationStats:
	added: int
	relined: int
	teeth: int
	interior_facing: int


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
		expected_added=40,
		expected_relined=70,
		expected_teeth=15,
		# A closed trunk's mouth surface faces toward decreasing model Z.
		interior_z_direction=-1,
		minimum_interior_facing=45,
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
		expected_added=12,
		expected_relined=47,
		expected_teeth=13,
		# The lid interior is its underside and faces increasing model Z.
		interior_z_direction=1,
		minimum_interior_facing=55,
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


def added_palette_index(value: int) -> bool:
	return GENERATED_PALETTE_FIRST <= value <= GENERATED_PALETTE_LAST


def relined_flesh_palette_index(value: int) -> bool:
	return RELINED_FLESH_PALETTE_FIRST <= value <= RELINED_FLESH_PALETTE_LAST


def relined_tooth_palette_index(value: int) -> bool:
	return RELINED_TOOTH_PALETTE_FIRST <= value <= RELINED_TOOTH_PALETTE_LAST


def generated_palette_index(value: int) -> bool:
	return (
		relined_flesh_palette_index(value)
		or relined_tooth_palette_index(value)
		or added_palette_index(value)
	)


def reconstruct_original_shell(generated_or_original: Slab) -> Slab:
	"""Undo added/relined interior cells and restore the original Mini shell."""
	clean = Slab(
		generated_or_original.size_x,
		generated_or_original.size_y,
		generated_or_original.size_z,
		bytearray(generated_or_original.voxels),
		bytearray(generated_or_original.palette),
	)
	for index, value in enumerate(clean.voxels):
		if relined_flesh_palette_index(value):
			clean.voxels[index] = value - RELINED_FLESH_PALETTE_FIRST
		elif relined_tooth_palette_index(value):
			clean.voxels[index] = value - RELINED_TOOTH_PALETTE_FIRST
		elif added_palette_index(value):
			clean.voxels[index] = EMPTY_VOXEL
	for palette_index in range(
		RELINED_FLESH_PALETTE_FIRST, GENERATED_PALETTE_LAST + 1
	):
		start = palette_index * 3
		clean.palette[start : start + 3] = b"\0\0\0"
	return clean


def palette_rgb(slab: Slab, palette_index: int) -> tuple[int, int, int]:
	start = palette_index * 3
	return tuple(slab.palette[start : start + 3])  # type: ignore[return-value]


def is_mimic_flesh_color(rgb: tuple[int, int, int]) -> bool:
	"""Select flesh/gum reds and purple, excluding brown wood and gray metal."""
	r, green, b = rgb
	strong_red = (
		r >= 8
		and green <= 5
		and b <= 5
		and r >= 3 * green + 6
		and r >= 3 * b + 6
	)
	purple_gum = r >= 8 and b >= 8 and green <= 2
	return strong_red or purple_gum


def is_mimic_tooth_color(rgb: tuple[int, int, int]) -> bool:
	"""Select warm ivory teeth, excluding wood highlights and gray metal."""
	r, green, b = rgb
	return r >= 22 and green >= 14 and b >= 8 and r > green > b


def mimic_interior_kind(rgb: tuple[int, int, int]) -> str | None:
	if is_mimic_flesh_color(rgb):
		return "flesh"
	if is_mimic_tooth_color(rgb):
		return "tooth"
	return None


def is_mimic_interior_color(rgb: tuple[int, int, int]) -> bool:
	return mimic_interior_kind(rgb) is not None


def sample_range(destination: int, destination_size: int, source_size: int) -> range:
	start = destination * source_size // destination_size
	end = ((destination + 1) * source_size + destination_size - 1) // destination_size
	return range(start, max(start + 1, end))


def choose_interior_color(
	regular: Slab,
	x_values: Iterable[int],
	y_values: Iterable[int],
	z_values: Iterable[int],
) -> InteriorSample | None:
	colors: Counter[tuple[str, tuple[int, int, int]]] = Counter()
	shell_count = 0
	for x in x_values:
		for y in y_values:
			for z in z_values:
				value = regular.voxels[regular.index(x, y, z)]
				if value == EMPTY_VOXEL:
					continue
				rgb = palette_rgb(regular, value)
				kind = mimic_interior_kind(rgb)
				if kind is None:
					shell_count += 1
				else:
					colors[(kind, rgb)] += 1
	if not colors:
		return None
	interior_count = sum(colors.values())
	# A single mouth-colored source voxel must not bleed across a shell-majority
	# sample. Empty source cells do not vote for or against an interior surface.
	if interior_count < shell_count:
		return None
	# Prefer a tooth in an exact tie so downsampling does not erase small teeth.
	(kind, rgb), _ = sorted(
		colors.items(),
		key=lambda item: (
			-item[1],
			0 if item[0][0] == "tooth" else 1,
			item[0][1],
		),
	)[0]
	return InteriorSample(kind, rgb, interior_count, shell_count)


def horizontal_exterior_air(slab: Slab, z: int) -> set[tuple[int, int]]:
	"""Return empty cells connected to an X/Y boundary without crossing shell."""
	exterior: set[tuple[int, int]] = set()
	queue: deque[tuple[int, int]] = deque()
	for x in range(slab.size_x):
		for y in (0, slab.size_y - 1):
			if slab.voxels[slab.index(x, y, z)] == EMPTY_VOXEL:
				exterior.add((x, y))
				queue.append((x, y))
	for y in range(slab.size_y):
		for x in (0, slab.size_x - 1):
			if slab.voxels[slab.index(x, y, z)] == EMPTY_VOXEL:
				exterior.add((x, y))
				queue.append((x, y))
	while queue:
		x, y = queue.popleft()
		for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
			next_x = x + dx
			next_y = y + dy
			if not (0 <= next_x < slab.size_x and 0 <= next_y < slab.size_y):
				continue
			if (next_x, next_y) in exterior:
				continue
			if slab.voxels[slab.index(next_x, next_y, z)] != EMPTY_VOXEL:
				continue
			exterior.add((next_x, next_y))
			queue.append((next_x, next_y))
	return exterior


def shell_cell_faces_interior(
	slab: Slab,
	x: int,
	y: int,
	z: int,
	exterior_air: set[tuple[int, int]],
	interior_z_direction: int,
) -> bool:
	"""Reline cavity/top faces without turning an exterior wall red."""
	horizontal_neighbors = (
		(x + 1, y),
		(x - 1, y),
		(x, y + 1),
		(x, y - 1),
	)
	has_exterior_face = (
		x in (0, slab.size_x - 1)
		or y in (0, slab.size_y - 1)
		or any(neighbor in exterior_air for neighbor in horizontal_neighbors)
	)
	has_interior_face = any(
		0 <= next_x < slab.size_x
		and 0 <= next_y < slab.size_y
		and slab.voxels[slab.index(next_x, next_y, z)] == EMPTY_VOXEL
		and (next_x, next_y) not in exterior_air
		for next_x, next_y in horizontal_neighbors
	)
	if interior_z_direction not in (-1, 1):
		raise ValueError("interior Z direction must be -1 or 1")
	neighbor_z = z + interior_z_direction
	interior_facing = (
		not 0 <= neighbor_z < slab.size_z
		or slab.voxels[slab.index(x, y, neighbor_z)] == EMPTY_VOXEL
	)
	# The open mouth's horizontal lining is an intentional visible surface even
	# when a gap in that same Z slice connects sideways to exterior air. Keep the
	# outermost X/Y rim authored so one colored voxel cannot tint an outer wall.
	if interior_facing and 0 < x < slab.size_x - 1 and 0 < y < slab.size_y - 1:
		return True
	if has_exterior_face:
		return False
	return has_interior_face


def count_interior_facing_generated_voxels(
	slab: Slab,
	interior_z_direction: int,
) -> int:
	if interior_z_direction not in (-1, 1):
		raise ValueError("interior Z direction must be -1 or 1")
	count = 0
	for x in range(slab.size_x):
		for y in range(slab.size_y):
			for z in range(slab.size_z):
				value = slab.voxels[slab.index(x, y, z)]
				if not generated_palette_index(value):
					continue
				neighbor_z = z + interior_z_direction
				if (
					not 0 <= neighbor_z < slab.size_z
					or slab.voxels[slab.index(x, y, neighbor_z)] == EMPTY_VOXEL
				):
					count += 1
	return count


def add_interior(
	shell: Slab,
	regular: Slab,
	interior_z_direction: int,
) -> tuple[Slab, GenerationStats]:
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
	exterior_air_by_z = {
		z: horizontal_exterior_air(result, z)
		for z in range(first_z, result.size_z)
	}

	added_colors: dict[tuple[int, int, int], int] = {}
	additions: list[tuple[int, tuple[int, int, int]]] = []
	relinings: list[tuple[int, str, int, tuple[int, int, int]]] = []
	for x in range(result.size_x):
		for y in range(result.size_y):
			for z in range(first_z, result.size_z):
				index = result.index(x, y, z)
				sample = choose_interior_color(
					regular,
					sample_range(x, result.size_x, regular.size_x),
					sample_range(y, result.size_y, regular.size_y),
					sample_range(z - first_z, target_depth, regular.size_z),
				)
				if sample is None:
					continue
				original_value = result.voxels[index]
				if original_value == EMPTY_VOXEL:
					# Added mouth geometry remains inside the Mini footprint. Unlike
					# relining, a central tooth may intentionally occupy top-connected
					# cavity air.
					if not (
						0 < x < result.size_x - 1
						and 0 < y < result.size_y - 1
					):
						continue
					additions.append((index, sample.rgb))
					added_colors.setdefault(sample.rgb, 0)
				elif original_value < ORIGINAL_SHELL_PALETTE_ENTRIES:
					if not shell_cell_faces_interior(
						result,
						x,
						y,
						z,
						exterior_air_by_z[z],
						interior_z_direction,
					):
						continue
					relinings.append(
						(index, sample.kind, original_value, sample.rgb)
					)
				else:
					raise ValueError(
						f"Mini shell uses unexpected palette index {original_value}"
					)

	if len(added_colors) > GENERATED_PALETTE_LAST - GENERATED_PALETTE_FIRST + 1:
		raise ValueError(
			f"added interior needs {len(added_colors)} reserved palette slots"
		)
	for offset, rgb in enumerate(sorted(added_colors)):
		palette_index = GENERATED_PALETTE_FIRST + offset
		added_colors[rgb] = palette_index
		start = palette_index * 3
		result.palette[start : start + 3] = bytes(rgb)
	for index, rgb in additions:
		result.voxels[index] = added_colors[rgb]

	# Relining palette slots encode the original Mini palette index. Select one
	# representative source color per kind/index pair while retaining exact
	# reconstruction of the authored shell.
	relining_colors: dict[int, Counter[tuple[int, int, int]]] = {}
	for _, kind, original_value, rgb in relinings:
		palette_index = (
			RELINED_TOOTH_PALETTE_FIRST
			if kind == "tooth"
			else RELINED_FLESH_PALETTE_FIRST
		) + original_value
		relining_colors.setdefault(palette_index, Counter())[rgb] += 1
	for palette_index, colors in relining_colors.items():
		rgb, _ = sorted(colors.items(), key=lambda item: (-item[1], item[0]))[0]
		start = palette_index * 3
		result.palette[start : start + 3] = bytes(rgb)
	for index, kind, original_value, _ in relinings:
		result.voxels[index] = (
			RELINED_TOOTH_PALETTE_FIRST
			if kind == "tooth"
			else RELINED_FLESH_PALETTE_FIRST
		) + original_value

	teeth = sum(1 for _, kind, _, _ in relinings if kind == "tooth")
	teeth += sum(
		1 for _, rgb in additions if mimic_interior_kind(rgb) == "tooth"
	)
	return result, GenerationStats(
		added=len(additions),
		relined=len(relinings),
		teeth=teeth,
		interior_facing=count_interior_facing_generated_voxels(
			result,
			interior_z_direction,
		),
	)


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

		generated, stats = add_interior(
			original_shell,
			regular,
			asset.interior_z_direction,
		)
		if stats.added != asset.expected_added:
			raise ValueError(
				f"{asset.name}: expected {asset.expected_added} added cavity voxels, "
				f"generated {stats.added}"
			)
		if stats.relined != asset.expected_relined:
			raise ValueError(
				f"{asset.name}: expected {asset.expected_relined} relined shell voxels, "
				f"generated {stats.relined}"
			)
		if stats.teeth != asset.expected_teeth:
			raise ValueError(
				f"{asset.name}: expected {asset.expected_teeth} tooth voxels, "
				f"generated {stats.teeth}"
			)
		if stats.interior_facing < asset.minimum_interior_facing:
			raise ValueError(
				f"{asset.name}: only {stats.interior_facing} generated "
				"interior-facing voxels; "
				"mouth lining would be hidden by the shell"
			)
		output_path = output_root / asset.mini_relative
		output_path.parent.mkdir(parents=True, exist_ok=True)
		output = generated.to_bytes()
		output_path.write_bytes(output)
		print(
			f"{asset.name}: preserved shell geometry, relined {stats.relined} "
			f"cells, added {stats.added} cavity voxels ({stats.teeth} teeth, "
			f"{stats.interior_facing} interior-facing), sha256={sha256(output)}"
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
