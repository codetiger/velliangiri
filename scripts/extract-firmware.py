#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""
Extract the BCM4334 Wi-Fi and Bluetooth firmware for S5L8947X (j33iap)
from the user's own copy of the stock 8.4.3 (12H1006) software.

The firmware is not redistributable, so it never enters this repository.
This script rebuilds it locally from the device's restore image:

  restore image (zip, local file or http(s) URL read with range requests)
    -> root filesystem disk image (largest .dmg member)
    -> decrypted with the user-supplied key (encrypted disk image, v2)
    -> UDIF chunks (raw, zero, zlib, bzip2, ADC)
    -> HFS+ volume (catalog, extents overflow, compressed files)
    -> Wi-Fi firmware and NVRAM, copied as they are
    -> Bluetooth patch, carved out of the program that carries it

Every output is chosen and verified by SHA-256, so the script names no
files from the stock system. Nothing is written outside --out and --cache.

Usage:
  extract-firmware.py --image URL|ZIP [--key HEX | --key-file FILE]
  extract-firmware.py --rootfs DMG    [--key HEX | --key-file FILE]

The key (72 hex digits) is only needed for an encrypted disk image; it is
never stored by this script. Requires Python 3 and the 'cryptography'
package (present in the build container).
"""

import argparse
import bz2
import hashlib
import hmac
import os
import plistlib
import struct
import sys
import urllib.request
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# Outputs: (subdirectory, file name, SHA-256, search prefix, extractor)
TARGETS = [
	("wifi", "brcmfmac4334.bin",
	 "2d635ebc4502107474063801a7cfa389b781599eed4f4806979230259f317fb1",
	 "/usr/share/firmware/wifi/", "file"),
	("wifi", "brcmfmac4334.txt",
	 "b7f43c7eb7c0a04d70a3f1bb25fb870bb49298a7dc1879360754e984b2b34a55",
	 "/usr/share/firmware/wifi/", "file"),
	("bt", "BCM4334B0.hcd",
	 "1c685b097b5cff26cfd5b95db4336d2713af02102dbfced35f3aa03aa0cce154",
	 "/usr/sbin/", "hcd"),
]


def die(msg):
	sys.exit("extract-firmware: " + msg)


def log(msg):
	print("[firmware] " + msg, file=sys.stderr, flush=True)


def be16(b, o):
	return struct.unpack_from(">H", b, o)[0]


def be32(b, o):
	return struct.unpack_from(">I", b, o)[0]


def be64(b, o):
	return struct.unpack_from(">Q", b, o)[0]


# ---------------------------------------------------------------- sources

class FileSource:
	def __init__(self, path):
		self.f = open(path, "rb")
		self.size = os.fstat(self.f.fileno()).st_size

	def read(self, off, n):
		self.f.seek(off)
		return self.f.read(n)


class HttpSource:
	def __init__(self, url):
		self.url = url
		self.size = int(self._get(0, 0, head=True))

	def _get(self, off, n, head=False):
		req = urllib.request.Request(self.url)
		req.add_header("Range", "bytes=%d-%d" % (off, off + max(n, 1) - 1))
		with urllib.request.urlopen(req) as r:
			if r.status != 206:
				die("server ignored the range request (HTTP %d)" % r.status)
			if head:
				return r.headers["Content-Range"].rsplit("/", 1)[1]
			return r.read()

	def read(self, off, n):
		return self._get(off, n)

	def stream(self, off, n, block=1 << 20):
		req = urllib.request.Request(self.url)
		req.add_header("Range", "bytes=%d-%d" % (off, off + n - 1))
		with urllib.request.urlopen(req) as r:
			while True:
				chunk = r.read(block)
				if not chunk:
					return
				yield chunk


def file_stream(src, off, n, block=1 << 20):
	while n > 0:
		chunk = src.read(off, min(block, n))
		if not chunk:
			return
		off += len(chunk)
		n -= len(chunk)
		yield chunk


def extract_rootfs(image, cache):
	"""Copy the largest .dmg member of the restore image into the cache."""
	src = HttpSource(image) if image.startswith(("http://", "https://")) \
		else FileSource(image)
	tail = src.read(max(0, src.size - 65558), min(src.size, 65558))
	eocd = tail.rfind(b"PK\x05\x06")
	if eocd < 0:
		die("the restore image is not a zip archive")
	count = struct.unpack_from("<H", tail, eocd + 10)[0]
	cd_size, cd_off = struct.unpack_from("<II", tail, eocd + 12)
	if cd_off == 0xffffffff:
		die("zip64 archives are not supported")
	cd = src.read(cd_off, cd_size)

	best, o = None, 0
	for _ in range(count):
		if cd[o:o + 4] != b"PK\x01\x02":
			die("corrupt zip central directory")
		method = struct.unpack_from("<H", cd, o + 10)[0]
		crc, csize, usize = struct.unpack_from("<III", cd, o + 16)
		nlen, xlen, clen = struct.unpack_from("<HHH", cd, o + 28)
		loff = struct.unpack_from("<I", cd, o + 42)[0]
		name = cd[o + 46:o + 46 + nlen].decode("utf-8", "replace")
		if name.lower().endswith(".dmg") and (best is None or usize > best[3]):
			best = (name, method, csize, usize, loff, crc)
		o += 46 + nlen + xlen + clen
	if best is None:
		die("no disk image in the restore image")
	name, method, csize, usize, loff, crc = best

	os.makedirs(cache, exist_ok=True)
	out = os.path.join(cache, os.path.basename(name))
	if os.path.exists(out) and os.path.getsize(out) == usize:
		log("using cached %s" % out)
		return out

	lh = src.read(loff, 30)
	if lh[:4] != b"PK\x03\x04":
		die("corrupt zip local header")
	data = loff + 30 + sum(struct.unpack_from("<HH", lh, 26))
	if method not in (0, 8):
		die("zip member uses unsupported method %d" % method)

	log("extracting %s (%d MB) ..." % (name, usize >> 20))
	chunks = src.stream(data, csize) if isinstance(src, HttpSource) \
		else file_stream(src, data, csize)
	inflate = zlib.decompressobj(-15) if method == 8 else None
	done, crc_now, step = 0, 0, usize // 20 or 1
	with open(out + ".part", "wb") as f:
		for chunk in chunks:
			plain = inflate.decompress(chunk) if inflate else chunk
			f.write(plain)
			crc_now = zlib.crc32(plain, crc_now)
			if (done + len(plain)) // step != done // step:
				log("  %d%%" % ((done + len(plain)) * 100 // usize))
			done += len(plain)
		if inflate:
			tail = inflate.flush()
			f.write(tail)
			crc_now = zlib.crc32(tail, crc_now)
			done += len(tail)
	if done != usize or crc_now != crc:
		die("disk image extraction failed (size or CRC mismatch)")
	os.replace(out + ".part", out)
	return out


# ------------------------------------------------------- encrypted image

class EncryptedImage:
	"""Version 2 encrypted disk image: AES-128-CBC per block, IV from HMAC-SHA1."""

	def __init__(self, src, key):
		try:
			from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
		except ImportError:
			die("the 'cryptography' Python package is required for encrypted images")
		self.Cipher, self.algorithms, self.modes = Cipher, algorithms, modes
		hdr = src.read(0, 0x48)
		if hdr[:8] != b"encrcdsa" or be32(hdr, 8) != 2:
			die("unsupported encrypted image header")
		if len(key) != 36:
			die("the key must be 36 bytes (72 hex digits)")
		self.src = src
		self.aes_key, self.hmac_key = key[:16], key[16:]
		self.bs = be32(hdr, 52)
		self.size = be64(hdr, 56)
		self.data = be64(hdr, 64)
		self.cache = {}

	def _block(self, i):
		blk = self.cache.get(i)
		if blk is None:
			iv = hmac.new(self.hmac_key, struct.pack(">I", i), hashlib.sha1).digest()[:16]
			dec = self.Cipher(self.algorithms.AES(self.aes_key), self.modes.CBC(iv)).decryptor()
			blk = dec.update(self.src.read(self.data + i * self.bs, self.bs)) + dec.finalize()
			if len(self.cache) > 4096:
				self.cache.clear()
			self.cache[i] = blk
		return blk

	def read(self, off, n):
		out = bytearray()
		while n > 0 and off < self.size:
			i, o = divmod(off, self.bs)
			part = self._block(i)[o:o + min(n, self.bs - o, self.size - off)]
			out += part
			off += len(part)
			n -= len(part)
		return bytes(out)


def open_image(path, key):
	src = FileSource(path)
	if src.read(0, 8) == b"encrcdsa":
		if key is None:
			die("the disk image is encrypted: pass --key or --key-file")
		img = EncryptedImage(src, key)
		if img.read(img.size - 512, 4) != b"koly":
			die("the key does not decrypt this disk image")
		return img
	return src


# ------------------------------------------------------------------ UDIF

def adc_decompress(src, outlen):
	out = bytearray()
	i = 0
	while i < len(src) and len(out) < outlen:
		b = src[i]
		if b & 0x80:
			n = (b & 0x7f) + 1
			out += src[i + 1:i + 1 + n]
			i += 1 + n
			continue
		if b & 0x40:
			n = (b & 0x3f) + 4
			dist = (src[i + 1] << 8 | src[i + 2]) + 1
			i += 3
		else:
			n = ((b & 0x3c) >> 2) + 3
			dist = ((b & 3) << 8 | src[i + 1]) + 1
			i += 2
		for _ in range(n):
			out.append(out[-dist])
	return bytes(out)


class Udif:
	"""The HFS partition of a UDIF disk image, as a flat readable volume."""

	def __init__(self, src):
		self.src = src
		koly = src.read(src.size - 512, 512)
		if koly[:4] != b"koly":
			die("not a UDIF disk image (no trailer)")
		self.fork = be64(koly, 24)
		plist = plistlib.loads(src.read(be64(koly, 216), be64(koly, 224)))
		blkx = plist["resource-fork"]["blkx"]
		part = next((p for p in blkx if "HFS" in p.get("Name", "")), None)
		if part is None:
			die("no HFS partition in the disk image")
		mish = part["Data"]
		if mish[:4] != b"mish":
			die("corrupt partition chunk table")
		self.base = be64(mish, 24)
		self.chunks = []
		for k in range(be32(mish, 200)):
			c = 204 + 40 * k
			kind = be32(mish, c)
			if kind in (0x7ffffffe, 0xffffffff):
				continue
			self.chunks.append((be64(mish, c + 8), be64(mish, c + 16), kind,
					    be64(mish, c + 24), be64(mish, c + 32)))
		self.chunks.sort()
		self.starts = [c[0] for c in self.chunks]
		self.cache = {}

	def _chunk(self, idx):
		data = self.cache.get(idx)
		if data is not None:
			return data
		sec, count, kind, coff, clen = self.chunks[idx]
		size = count * 512
		if kind in (0, 2):
			data = bytes(size)
		else:
			raw = self.src.read(self.fork + self.base + coff, clen)
			if kind == 1:
				data = raw
			elif kind == 0x80000005:
				data = zlib.decompress(raw)
			elif kind == 0x80000006:
				data = bz2.decompress(raw)
			elif kind == 0x80000004:
				data = adc_decompress(raw, size)
			else:
				die("unsupported disk image chunk type 0x%08x" % kind)
		if len(self.cache) > 64:
			self.cache.clear()
		self.cache[idx] = data
		return data

	def read(self, off, n):
		import bisect
		out = bytearray()
		while n > 0:
			sec = off // 512
			idx = bisect.bisect_right(self.starts, sec) - 1
			if idx < 0:
				die("read outside the disk image")
			start, count = self.chunks[idx][:2]
			if sec >= start + count:
				die("read outside the disk image")
			data = self._chunk(idx)
			o = off - start * 512
			part = data[o:o + min(n, count * 512 - o)]
			if not part:
				die("short disk image chunk")
			out += part
			off += len(part)
			n -= len(part)
		return bytes(out)


# ------------------------------------------------------------------ HFS+

class Hfs:
	def __init__(self, vol):
		self.vol = vol
		vh = vol.read(1024, 512)
		if vh[:2] not in (b"H+", b"HX"):
			die("the partition is not HFS+")
		self.bs = be32(vh, 40)
		self.extents_fork = (vh[192:272], 3)
		self.catalog_fork = (vh[272:352], 4)
		self.attr_fork = (vh[352:432], 8)

	def _extents(self, fork, cnid, fork_type=0):
		total = be32(fork, 12)
		ext = []
		for k in range(8):
			start, count = be32(fork, 16 + 8 * k), be32(fork, 20 + 8 * k)
			if count:
				ext.append((start, count))
		have = sum(c for _, c in ext)
		while have < total:
			more = self._overflow(cnid, fork_type, have)
			if not more:
				die("missing extents for file %d" % cnid)
			ext += more
			have += sum(c for _, c in more)
		return ext

	def _overflow(self, cnid, fork_type, start_block):
		for node, o in self._leaves(self.extents_fork):
			if node[o + 2] == fork_type and be32(node, o + 4) == cnid \
					and be32(node, o + 8) == start_block:
				rec = o + 2 + be16(node, o)
				return [(be32(node, rec + 8 * k), be32(node, rec + 4 + 8 * k))
					for k in range(8) if be32(node, rec + 4 + 8 * k)]
		return None

	def read_fork(self, fork, cnid, fork_type=0):
		size = be64(fork, 0)
		out = bytearray()
		for start, count in self._extents(fork, cnid, fork_type):
			out += self.vol.read(start * self.bs, count * self.bs)
			if len(out) >= size:
				break
		return bytes(out[:size])

	def _leaves(self, special):
		fork, cnid = special
		data = getattr(self, "_tree_%d" % cnid, None)
		if data is None:
			data = self.read_fork(fork, cnid)
			setattr(self, "_tree_%d" % cnid, data)
		ns = be16(data, 32)
		node_no = be32(data, 24)
		while node_no:
			node = data[node_no * ns:(node_no + 1) * ns]
			for i in range(be16(node, 10)):
				yield node, be16(node, ns - 2 * (i + 1))
			node_no = be32(node, 0)

	def files(self):
		"""Yield (path, catalog file record) for every regular file."""
		folders, files = {}, []
		for node, o in self._leaves(self.catalog_fork):
			parent = be32(node, o + 2)
			nlen = be16(node, o + 6)
			name = node[o + 8:o + 8 + 2 * nlen].decode("utf-16-be", "replace")
			rec = node[o + 2 + be16(node, o):]
			kind = be16(rec, 0)
			if kind == 1:
				folders[be32(rec, 8)] = (parent, name)
			elif kind == 2:
				files.append((parent, name, rec[:248]))

		def path(cnid):
			parts = []
			while cnid in folders and cnid != 2:
				cnid, name = folders[cnid]
				parts.append(name)
			return "/" + "/".join(reversed(parts))

		for parent, name, rec in files:
			yield path(parent).rstrip("/") + "/" + name, rec

	def _xattr(self, cnid, want):
		for node, o in self._leaves(self.attr_fork):
			if be32(node, o + 4) != cnid:
				continue
			nlen = be16(node, o + 12)
			if node[o + 14:o + 14 + 2 * nlen].decode("utf-16-be", "replace") != want:
				continue
			rec = o + 2 + be16(node, o)
			if be32(node, rec) != 0x10:
				die("unsupported extended attribute record")
			return node[rec + 16:rec + 16 + be32(node, rec + 12)]
		return None

	def read_file(self, rec):
		cnid = be32(rec, 8)
		data = self.read_fork(rec[88:168], cnid)
		if not rec[41] & 0x20:
			return data
		hdr = self._xattr(cnid, "com.apple.decmpfs")
		if hdr is None or hdr[:4] != b"fpmc":
			die("compressed file %d without a compression header" % cnid)
		kind, size = struct.unpack_from("<IQ", hdr, 4)
		if kind == 3:
			body = hdr[16:]
			out = body[1:] if body[:1] == b"\xff" else zlib.decompress(body)
		elif kind == 4:
			rsrc = self.read_fork(rec[168:248], cnid, 0xff)
			base = be32(rsrc, 0) + 4
			out = bytearray()
			for i in range(struct.unpack_from("<I", rsrc, base)[0]):
				off, ln = struct.unpack_from("<II", rsrc, base + 4 + 8 * i)
				blk = rsrc[base + off:base + off + ln]
				out += blk[1:] if blk[:1] == b"\xff" else zlib.decompress(blk)
			out = bytes(out)
		else:
			die("unsupported file compression type %d" % kind)
		if len(out) != size:
			die("decompressed size mismatch for file %d" % cnid)
		return out


# ----------------------------------------------------------- extractors

def carve_hcd(data):
	"""
	Yield every Broadcom patch embedded in data: a run of vendor HCI
	commands (opcode group 0x3f, little endian, then a length byte) that
	ends with Launch RAM (0xfc4e). Each record start is tried, so the
	exact patch is found by its hash.
	"""
	end_of = {}
	n = len(data)

	def walk(s):
		path = []
		o = s
		while True:
			if o in end_of:
				e = end_of[o]
				break
			if o + 3 > n or data[o + 1] & 0xfc != 0xfc:
				e = None
				break
			path.append(o)
			if data[o] == 0x4e and data[o + 1] == 0xfc:
				e = o + 3 + data[o + 2]
				break
			o += 3 + data[o + 2]
		for p in path:
			end_of[p] = e
		return e

	start = data.find(b"\x4c\xfc")
	while start >= 0:
		e = walk(start)
		if e is not None and e - start > 1024:
			yield data[start:e]
		start = data.find(b"\x4c\xfc", start + 1)


def main():
	ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
	src = ap.add_mutually_exclusive_group()
	src.add_argument("--image", default=os.environ.get("FIRMWARE_IMAGE"),
			 help="restore image: local zip or http(s) URL (env FIRMWARE_IMAGE)")
	src.add_argument("--rootfs", default=os.environ.get("FIRMWARE_ROOTFS"),
			 help="root filesystem disk image, encrypted or not (env FIRMWARE_ROOTFS)")
	ap.add_argument("--key", default=os.environ.get("FIRMWARE_KEY"),
			help="disk image key, 72 hex digits (env FIRMWARE_KEY)")
	ap.add_argument("--key-file", default=os.path.join(ROOT, "research", "firmware", "rootfs.key"),
			help="file holding the key (default research/firmware/rootfs.key)")
	ap.add_argument("--out", default=os.path.join(ROOT, "research", "firmware"),
			help="output directory (default research/firmware)")
	ap.add_argument("--cache", default=os.path.join(ROOT, "research", "firmware", "cache"),
			help="where the disk image is kept between runs")
	args = ap.parse_args()

	if not args.image and not args.rootfs:
		ap.error("give --image or --rootfs")

	key = args.key
	if key is None and os.path.isfile(args.key_file):
		with open(args.key_file) as f:
			key = f.read().strip()
	if key is not None:
		try:
			key = bytes.fromhex(key)
		except ValueError:
			die("the key is not hexadecimal")

	rootfs = args.rootfs or extract_rootfs(args.image, args.cache)
	log("reading %s" % rootfs)
	hfs = Hfs(Udif(open_image(rootfs, key)))

	wanted = {t[2]: t for t in TARGETS}
	prefixes = sorted({t[3] for t in TARGETS})
	found = {}
	for path, rec in hfs.files():
		if not any(path.startswith(p) for p in prefixes):
			continue
		kinds = {t[4] for t in TARGETS if path.startswith(t[3]) and t[2] not in found}
		if not kinds:
			continue
		data = hfs.read_file(rec)
		blobs = [data] if "file" in kinds else []
		if "hcd" in kinds:
			blobs += carve_hcd(data)
		for blob in blobs:
			digest = hashlib.sha256(blob).hexdigest()
			if digest in wanted and digest not in found:
				found[digest] = blob

	missing = [t[1] for t in TARGETS if t[2] not in found]
	for sub, name, digest, _, _ in TARGETS:
		if digest not in found:
			continue
		os.makedirs(os.path.join(args.out, sub), exist_ok=True)
		dst = os.path.join(args.out, sub, name)
		with open(dst, "wb") as f:
			f.write(found[digest])
		log("wrote %s (%d bytes, SHA-256 verified)" % (dst, len(found[digest])))
	if missing:
		die("not found in this image: %s (wrong build or key?)" % ", ".join(missing))


if __name__ == "__main__":
	main()
