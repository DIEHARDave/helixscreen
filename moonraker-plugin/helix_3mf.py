# SPDX-License-Identifier: GPL-3.0-or-later
"""
HelixPrint .3mf converter.

A slicer with a "use 3MF" upload option sends `<name>.gcode.3mf`, a zip holding
the sliced plate as `Metadata/plate_<N>.gcode`. A Moonraker whose file manager
does not list `.3mf` as G-code stores the zip but can neither scan nor print it.
This converter writes each sliced plate out as a plain `.gcode` beside the zip,
through Moonraker's own upload path, so metadata, thumbnails and every client
treat it like any other print.

The rules (naming, ownership, cleanup) are documented in README.md, section
".3mf Conversion", together with the `server.helix.3mf_conversions` contract.

Pure helpers sit at the top and touch no Moonraker object; Helix3mfConverter
is the Moonraker-facing half and is driven by helix_print.py.
"""

from __future__ import annotations

import asyncio
import hashlib
import inspect
import logging
import os
import shutil
import sys
import uuid
import zipfile
import zlib
from pathlib import Path
from typing import Any, Callable, Dict, Iterable, List, Optional, Tuple

# Database namespace holding one record list per source .3mf
DB_NAMESPACE = "helix_3mf"

PLATE_PREFIX = "Metadata/plate_"
PLATE_SUFFIX = ".gcode"

# Inflate and hash in pieces this size: the whole plate is never in memory.
CHUNK_SIZE = 64 * 1024

STATE_DONE = "done"
STATE_PENDING = "pending"
STATE_FAILED = "failed"

# Klipper print states after which a file it held is free to replace.
_IDLE_JOB_STATES = ("standby", "complete", "cancelled", "error")


class ConversionError(Exception):
    """A .3mf that cannot be turned into G-code; the message is user-facing."""


# =============================================================================
# Pure helpers
# =============================================================================


def is_3mf(name: str) -> bool:
    return name.lower().endswith(".3mf")


def output_stem(source: str) -> str:
    """`dir/x.gcode.3mf` -> `dir/x`; `dir/x.3mf` -> `dir/x`."""
    lowered = source.lower()
    if lowered.endswith(".gcode.3mf"):
        return source[: -len(".gcode.3mf")]
    if lowered.endswith(".3mf"):
        return source[: -len(".3mf")]
    return source


def gcode_name_for(source: str) -> str:
    """The printable name a .3mf stands for: `dir/x.gcode.3mf` -> `dir/x.gcode`."""
    return output_stem(source) + PLATE_SUFFIX


def sliced_plates(names: Iterable[str]) -> List[int]:
    """Plate numbers with a sliced `Metadata/plate_<N>.gcode` entry, ascending."""
    plates = []
    for name in names:
        if not (name.startswith(PLATE_PREFIX) and name.endswith(PLATE_SUFFIX)):
            continue
        digits = name[len(PLATE_PREFIX): -len(PLATE_SUFFIX)]
        if digits.isdigit():
            plates.append(int(digits))
    return sorted(set(plates))


def _is_hidden(path: str) -> bool:
    """Under a dot-directory or a dot-file: Moonraker's and our own staging."""
    return any(part.startswith(".") for part in path.split("/") if part)


def plate_entry(plate: int) -> str:
    return f"{PLATE_PREFIX}{plate}{PLATE_SUFFIX}"


def file_md5(path: Path) -> str:
    digest = hashlib.md5()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(CHUNK_SIZE), b""):
            digest.update(chunk)
    return digest.hexdigest()


def read_plate_md5(zf: zipfile.ZipFile, plate: int) -> Optional[str]:
    """The slicer's own md5 of the plate, from `plate_<N>.gcode.md5`, if any."""
    try:
        raw = zf.read(plate_entry(plate) + ".md5")
    except KeyError:
        return None
    text = raw.decode("ascii", errors="ignore").strip().lower()
    if len(text) == 32 and all(c in "0123456789abcdef" for c in text):
        return text
    return None


def extract_plate(zf: zipfile.ZipFile, plate: int, dest: Path) -> str:
    """Stream one plate's G-code to @p dest and return its md5.

    zipfile checks the entry's CRC as the last bytes are read; the slicer's md5,
    when the archive carries one, is checked on top. On any failure @p dest is
    removed and ConversionError raised.
    """
    expected = read_plate_md5(zf, plate)
    digest = hashlib.md5()
    try:
        with zf.open(plate_entry(plate)) as src, open(dest, "wb") as out:
            for chunk in iter(lambda: src.read(CHUNK_SIZE), b""):
                digest.update(chunk)
                out.write(chunk)
    except (zipfile.BadZipFile, zlib.error, OSError, EOFError) as e:
        _unlink_quietly(dest)
        raise ConversionError(f"plate {plate} could not be extracted: {e}")
    md5 = digest.hexdigest()
    if expected is not None and md5 != expected:
        _unlink_quietly(dest)
        raise ConversionError(f"plate {plate} md5 {md5} does not match the slicer's {expected}")
    return md5


def _unlink_quietly(path: Path) -> None:
    try:
        path.unlink()
    except OSError:
        pass


class Conversion:
    """One sliced plate of one source .3mf, as the endpoint reports it.

    `owned` is not part of the endpoint contract: it says the converter wrote
    `output` itself, as opposed to finding an identical file already there, and
    only an owned output is ever deleted.
    """

    FIELDS = (
        "source", "source_mtime", "source_size", "plate", "plate_md5",
        "output", "output_md5", "state", "error",
    )

    def __init__(
        self,
        source: str,
        source_mtime: float = 0.0,
        source_size: int = 0,
        plate: int = 0,
        plate_md5: str = "",
        output: str = "",
        output_md5: str = "",
        state: str = STATE_PENDING,
        error: str = "",
        owned: bool = False,
    ) -> None:
        self.source = source
        self.source_mtime = source_mtime
        self.source_size = source_size
        self.plate = plate
        self.plate_md5 = plate_md5
        self.output = output
        self.output_md5 = output_md5
        self.state = state
        self.error = error
        self.owned = owned

    def public(self) -> Dict[str, Any]:
        return {name: getattr(self, name) for name in self.FIELDS}

    def to_dict(self) -> Dict[str, Any]:
        record = self.public()
        record["owned"] = self.owned
        return record

    @classmethod
    def from_dict(cls, record: Dict[str, Any]) -> "Conversion":
        known = {k: record[k] for k in cls.FIELDS + ("owned",) if k in record}
        return cls(**known)


PlanAction = str  # "write" | "reuse" | "taken"


def plan_output(
    gc_path: Path,
    source: str,
    plate: int,
    multi: bool,
    plate_md5: str,
    records: Iterable[Conversion],
) -> Tuple[PlanAction, str, bool]:
    """Decide where a plate goes: (action, output, owned).

    `write` publishes the plate at `output`; `reuse` leaves an identical file
    there untouched; `taken` means every candidate name holds someone else's
    file. A file is ours only while a record of THIS source names it and its
    md5 still matches what we wrote: an edited output, or another source's
    output, is treated as foreign and never overwritten.
    """
    stem = output_stem(source)
    candidates = [] if multi else [stem + PLATE_SUFFIX]
    candidates.append(f"{stem}_plate{plate}{PLATE_SUFFIX}")
    records = list(records)
    for name in candidates:
        path = gc_path / name
        if not path.exists() and not path.is_symlink():
            return "write", name, True
        if not path.is_file():
            continue
        md5 = file_md5(path)
        owner = next(
            (r for r in records
             if r.output == name and r.state in (STATE_DONE, STATE_PENDING)),
            None,
        )
        if owner is not None and owner.output_md5 == md5:
            if owner.source != source:
                continue
            if not owner.owned:
                if md5 == plate_md5:
                    return "reuse", name, False
                continue
            return ("reuse" if md5 == plate_md5 else "write"), name, True
        if md5 == plate_md5:
            return "reuse", name, False
    return "taken", "", False


def native_3mf_support(file_manager: Any) -> Optional[bool]:
    """Whether this Moonraker treats .3mf as G-code itself (QIDI's does).

    Read from the file manager module's VALID_GCODE_EXTS; None when the module
    does not expose one, so the caller cannot tell.
    """
    module = sys.modules.get(type(file_manager).__module__)
    exts = getattr(module, "VALID_GCODE_EXTS", None)
    if exts is None:
        return None
    return ".3mf" in exts


def upload_hook_shape_ok(method: Any) -> bool:
    """Whether finalize_upload looks like the `async (form_args) -> dict` we wrap."""
    if method is None or not inspect.iscoroutinefunction(method):
        return False
    try:
        params = list(inspect.signature(method).parameters.values())
    except (TypeError, ValueError):
        return False
    return len(params) == 1 and params[0].kind in (
        inspect.Parameter.POSITIONAL_ONLY,
        inspect.Parameter.POSITIONAL_OR_KEYWORD,
    )


# =============================================================================
# Moonraker side
# =============================================================================


class _Request:
    def __init__(self) -> None:
        self.start_print = False
        self.user: Any = None
        self.waiters: List[asyncio.Future] = []


class Helix3mfConverter:
    """Converts uploaded .3mf files, one at a time, off Moonraker's event loop."""

    def __init__(
        self,
        server: Any,
        file_manager: Any,
        database: Any,
        klippy_apis: Any,
        gc_path: Path,
        temp_dir: str,
        start_on_upload: bool,
    ) -> None:
        self.server = server
        self.eventloop = server.get_event_loop()
        self.file_manager = file_manager
        self.database = database
        self.klippy_apis = klippy_apis
        self.gc_path = gc_path
        self.temp_dir = temp_dir
        self.start_on_upload = start_on_upload
        self.records: Dict[str, List[Conversion]] = {}
        self.active = False
        self.hook_installed = False
        self._queue: Dict[str, _Request] = {}
        self._worker: Optional[asyncio.Task] = None
        self._original_finalize: Optional[Callable] = None

    # -- lifecycle -----------------------------------------------------------

    async def start(self) -> None:
        native = native_3mf_support(self.file_manager)
        if native:
            logging.info("HelixPrint: Moonraker prints .3mf natively, .3mf conversion off")
            return
        if native is None:
            logging.info(
                "HelixPrint: cannot read Moonraker's G-code extensions, converting .3mf anyway"
            )
        self.active = True
        await self._load_records()
        self.server.register_event_handler(
            "file_manager:filelist_changed", self.on_filelist_changed
        )
        self.server.register_event_handler(
            "job_state:state_changed", self.on_job_state_changed
        )
        if self.start_on_upload:
            self.install_upload_hook()
        await self.backfill()

    def install_upload_hook(self) -> bool:
        method = getattr(self.file_manager, "finalize_upload", None)
        if not upload_hook_shape_ok(method):
            logging.warning(
                "HelixPrint: Moonraker's finalize_upload is not the expected "
                "`async (form_args)`; start_3mf_on_upload stays off, conversion continues"
            )
            return False
        self._original_finalize = method
        self.file_manager.finalize_upload = self._finalize_upload_hook
        self.hook_installed = True
        logging.info("HelixPrint: start_3mf_on_upload active")
        return True

    async def close(self) -> None:
        if self._original_finalize is not None:
            self.file_manager.finalize_upload = self._original_finalize
            self._original_finalize = None
            self.hook_installed = False
        if self._worker is not None:
            self._worker.cancel()

    # -- endpoint ------------------------------------------------------------

    async def handle_conversions(self, web_request: Any) -> Dict[str, Any]:
        source = web_request.get_str("source", "") or ""
        if source:
            entries = self.records.get(source.lstrip("/"), [])
        else:
            entries = [c for group in self.records.values() for c in group]
        return {"conversions": [c.public() for c in entries]}

    # -- events --------------------------------------------------------------

    async def on_filelist_changed(self, payload: Dict[str, Any]) -> None:
        if not self.active:
            return
        action = payload.get("action", "")
        item = payload.get("item") or {}
        if item.get("root", "gcodes") != "gcodes":
            return
        path = (item.get("path") or "").lstrip("/")
        source_item = payload.get("source_item") or {}
        old_path = (source_item.get("path") or "").lstrip("/")
        if _is_hidden(path) or _is_hidden(old_path):
            return

        if action in ("create_file", "modify_file"):
            if is_3mf(path):
                self.enqueue(path)
        elif action == "delete_file":
            if is_3mf(path):
                await self.remove_source(path)
            else:
                await self._forget_output(path)
        elif action == "move_file":
            if is_3mf(old_path) and old_path in self.records:
                self._rekey(old_path, path)
                await self._save(path)
            elif is_3mf(path):
                self.enqueue(path)
            else:
                await self._move_output(old_path, path)
        elif action == "delete_dir":
            prefix = path.rstrip("/") + "/"
            for source in [s for s in self.records if s.startswith(prefix)]:
                await self.remove_source(source)

    async def on_job_state_changed(
        self, job_event: Any, prev_stats: Dict[str, Any], new_stats: Dict[str, Any]
    ) -> None:
        if not self.active or new_stats.get("state", "") not in _IDLE_JOB_STATES:
            return
        for source, group in list(self.records.items()):
            if any(c.state == STATE_PENDING for c in group):
                self.enqueue(source)

    # -- queue ---------------------------------------------------------------

    def enqueue(self, source: str, start_print: bool = False, user: Any = None) -> asyncio.Future:
        request = self._queue.get(source)
        if request is None:
            request = _Request()
            self._queue[source] = request
        if start_print:
            request.start_print = True
            request.user = user
        waiter = asyncio.get_event_loop().create_future()
        request.waiters.append(waiter)
        if self._worker is None or self._worker.done():
            self._worker = asyncio.ensure_future(self._run_queue())
        return waiter

    async def _run_queue(self) -> None:
        while self._queue:
            source = next(iter(self._queue))
            request = self._queue.pop(source)
            started = False
            try:
                started = await self.convert(source, request.start_print, request.user)
            except Exception:
                logging.exception(f"HelixPrint: .3mf conversion of {source} failed")
            for waiter in request.waiters:
                if not waiter.done():
                    waiter.set_result(started)

    async def wait_idle(self) -> None:
        while self._worker is not None and not self._worker.done():
            await self._worker

    async def backfill(self) -> None:
        """Queue every .3mf with no up-to-date conversion; drop vanished sources."""
        found = await self.eventloop.run_in_thread(self._scan_3mf_files)
        for source in [s for s in self.records if s not in found]:
            await self.remove_source(source)
        for source in sorted(found):
            self.enqueue(source)

    def _scan_3mf_files(self) -> List[str]:
        found = []
        for root, dirs, files in os.walk(self.gc_path):
            dirs[:] = [d for d in dirs if not d.startswith(".")]
            for name in files:
                if is_3mf(name) and not name.startswith("."):
                    full = Path(root) / name
                    found.append(str(full.relative_to(self.gc_path)))
        return found

    # -- conversion ----------------------------------------------------------

    def _up_to_date(self, source: str, mtime: float, size: int) -> bool:
        group = self.records.get(source)
        if not group:
            return False
        for conv in group:
            if conv.source_mtime != mtime or conv.source_size != size:
                return False
            if conv.state == STATE_PENDING:
                return False
        return True

    async def convert(self, source: str, start_print: bool = False, user: Any = None) -> bool:
        """Convert one source; True when a print was started for it."""
        full = self.gc_path / source
        try:
            st = full.stat()
        except OSError:
            await self.remove_source(source)
            return False
        if self._up_to_date(source, st.st_mtime, st.st_size):
            self._mark_missing_outputs(source)
            await self._save(source)
            if start_print:
                return await self._start_existing(source, user)
            return False

        previous = self.records.get(source, [])
        try:
            plan = await self.eventloop.run_in_thread(self._read_plates, full)
        except ConversionError as e:
            self.records[source] = [
                Conversion(source, st.st_mtime, st.st_size, state=STATE_FAILED, error=str(e))
            ]
            await self._drop_outputs(previous, keep=())
            await self._save(source)
            logging.info(f"HelixPrint: {source}: {e}")
            return False

        multi = len(plan) > 1
        results: List[Conversion] = []
        started = False
        for plate, plate_md5, usize in plan:
            conv = Conversion(source, st.st_mtime, st.st_size, plate, plate_md5)
            # Copies: plan_output reads them on a worker thread.
            known = [
                Conversion.from_dict(c.to_dict())
                for s, g in self.records.items() if s != source for c in g
            ] + [Conversion.from_dict(c.to_dict()) for c in previous]
            action, output, owned = await self.eventloop.run_in_thread(
                plan_output, self.gc_path, source, plate, multi, plate_md5, known
            )
            if action == "taken":
                conv.state = STATE_FAILED
                conv.error = "every output name holds a file the plugin did not write"
            elif action == "reuse":
                conv.output, conv.output_md5, conv.owned = output, plate_md5, owned
                conv.state = STATE_DONE
                if start_print and not multi:
                    started = await self._start_print(output, user)
            else:
                want_start = start_print and not multi
                started_here = await self._write_output(conv, full, output, usize, want_start, user)
                started = started or started_here
            results.append(conv)
            if conv.state == STATE_FAILED:
                logging.info(f"HelixPrint: {source} plate {plate}: {conv.error}")
            elif conv.state == STATE_DONE:
                logging.info(f"HelixPrint: {source} plate {plate} -> {conv.output}")
        if start_print and multi:
            logging.info(f"HelixPrint: {source} holds {len(plan)} plates; not starting a print")

        self.records[source] = results
        await self._drop_outputs(previous, keep={c.output for c in results if c.output})
        await self._save(source)
        return started

    def _read_plates(self, full: Path) -> List[Tuple[int, str, int]]:
        """(plate, md5, uncompressed size) for every sliced plate, md5 from the
        archive when it carries one. Raises ConversionError for a non-zip or a
        zip with no sliced plate."""
        try:
            with zipfile.ZipFile(full) as zf:
                plates = sliced_plates(zf.namelist())
                if not plates:
                    raise ConversionError("no sliced plate in this .3mf (a project file?)")
                return [
                    (p, read_plate_md5(zf, p) or "", zf.getinfo(plate_entry(p)).file_size)
                    for p in plates
                ]
        except (zipfile.BadZipFile, OSError) as e:
            raise ConversionError(f"not a readable .3mf: {e}")

    async def _write_output(
        self, conv: Conversion, full: Path, output: str, usize: int, start_print: bool, user: Any
    ) -> bool:
        temp_dir = self.gc_path / self.temp_dir
        temp_dir.mkdir(parents=True, exist_ok=True)
        if shutil.disk_usage(str(temp_dir)).free < 2 * usize:
            conv.state, conv.error = STATE_FAILED, "not enough free space to convert"
            return False
        partial = temp_dir / f"3mf-{uuid.uuid4().hex}.partial"
        try:
            md5 = await self.eventloop.run_in_thread(self._extract, full, conv.plate, partial)
        except ConversionError as e:
            conv.state, conv.error = STATE_FAILED, str(e)
            return False
        if conv.plate_md5 == "":
            conv.plate_md5 = md5
        directory, filename = os.path.split(output)
        form_args = {
            "root": "gcodes",
            "filename": filename,
            "path": directory,
            "tmp_file_path": str(partial),
            "print": "true" if start_print else "false",
        }
        if user is not None:
            form_args["current_user"] = user
        finalize = self._original_finalize or self.file_manager.finalize_upload
        try:
            result = await finalize(form_args)
        except Exception as e:
            _unlink_quietly(partial)
            if getattr(e, "status_code", None) == 403:
                # The print holds our earlier output: it stays ours until the
                # retry replaces it.
                conv.state, conv.error = STATE_PENDING, "waiting for the print using the output to end"
                held = self.gc_path / output
                if held.is_file():
                    conv.output, conv.owned = output, True
                    conv.output_md5 = await self.eventloop.run_in_thread(file_md5, held)
            else:
                conv.state, conv.error = STATE_FAILED, f"publishing the output failed: {e}"
            return False
        conv.output, conv.output_md5, conv.owned = output, md5, True
        conv.state, conv.error = STATE_DONE, ""
        return bool((result or {}).get("print_started"))

    @staticmethod
    def _extract(full: Path, plate: int, dest: Path) -> str:
        try:
            with zipfile.ZipFile(full) as zf:
                return extract_plate(zf, plate, dest)
        except (zipfile.BadZipFile, OSError) as e:
            _unlink_quietly(dest)
            raise ConversionError(f"not a readable .3mf: {e}")

    async def _start_existing(self, source: str, user: Any) -> bool:
        group = [c for c in self.records.get(source, []) if c.state == STATE_DONE]
        if len(group) != 1 or len(self.records.get(source, [])) != 1:
            return False
        return await self._start_print(group[0].output, user)

    async def _start_print(self, output: str, user: Any) -> bool:
        try:
            await self.klippy_apis.start_print(output, user=user)
        except Exception as e:
            logging.info(f"HelixPrint: not starting {output}: {e}")
            return False
        return True

    # -- cleanup -------------------------------------------------------------

    def _mark_missing_outputs(self, source: str) -> None:
        for conv in self.records.get(source, []):
            if conv.state == STATE_DONE and not (self.gc_path / conv.output).is_file():
                self._mark_output_gone(conv)

    @staticmethod
    def _mark_output_gone(conv: Conversion) -> None:
        conv.state, conv.error = STATE_FAILED, "the output was deleted"
        conv.output, conv.output_md5, conv.owned = "", "", False

    async def remove_source(self, source: str) -> None:
        group = self.records.pop(source, None)
        if group is None:
            return
        await self._drop_outputs(group, keep=())
        await self._delete_record(source)

    async def _drop_outputs(self, group: Iterable[Conversion], keep: Iterable[str]) -> None:
        """Delete each output in @p group that we wrote and nobody changed."""
        keep = set(keep)
        for conv in group:
            if not conv.owned or not conv.output or conv.output in keep:
                continue
            path = self.gc_path / conv.output
            try:
                unchanged = path.is_file() and await self.eventloop.run_in_thread(
                    file_md5, path
                ) == conv.output_md5
            except OSError:
                unchanged = False
            if not unchanged:
                continue
            try:
                await self.file_manager.delete_file(f"gcodes/{conv.output}")
                logging.info(f"HelixPrint: removed {conv.output} (its .3mf is gone)")
            except Exception as e:
                logging.info(f"HelixPrint: keeping {conv.output}: {e}")

    async def _forget_output(self, output: str) -> None:
        for source, group in self.records.items():
            for conv in group:
                if conv.output == output and conv.state == STATE_DONE:
                    self._mark_output_gone(conv)
                    await self._save(source)
                    return

    async def _move_output(self, old: str, new: str) -> None:
        for source, group in self.records.items():
            for conv in group:
                if conv.output == old:
                    conv.output = new
                    await self._save(source)
                    return

    def _rekey(self, old: str, new: str) -> None:
        group = self.records.pop(old)
        for conv in group:
            conv.source = new
        self.records[new] = group
        asyncio.ensure_future(self._delete_record(old))

    # -- persistence ---------------------------------------------------------

    async def _load_records(self) -> None:
        if self.database is None:
            return
        try:
            stored = await self.database.get_item(DB_NAMESPACE, None, {})
        except Exception as e:
            logging.warning(f"HelixPrint: could not read .3mf conversions: {e}")
            return
        for source, group in (stored or {}).items():
            try:
                self.records[source] = [Conversion.from_dict(r) for r in group]
            except (TypeError, KeyError):
                logging.warning(f"HelixPrint: dropping unreadable .3mf record for {source}")

    async def _save(self, source: str) -> None:
        if self.database is None or source not in self.records:
            return
        value = [c.to_dict() for c in self.records[source]]
        try:
            await self.database.insert_item(DB_NAMESPACE, [source], value)
        except Exception as e:
            logging.warning(f"HelixPrint: could not save .3mf conversion of {source}: {e}")

    async def _delete_record(self, source: str) -> None:
        if self.database is None:
            return
        try:
            await self.database.delete_item(DB_NAMESPACE, [source])
        except Exception:
            pass

    # -- upload hook ---------------------------------------------------------

    async def _finalize_upload_hook(self, form_args: Dict[str, Any]) -> Dict[str, Any]:
        original = self._original_finalize
        assert original is not None
        if not isinstance(form_args, dict):
            return await original(form_args)
        filename = str(form_args.get("filename", ""))
        root = str(form_args.get("root", "gcodes")).lower()
        wants_print = str(form_args.get("print", "false")) == "true"
        if not (root == "gcodes" and is_3mf(filename) and wants_print):
            return await original(form_args)

        stored_args = dict(form_args)
        stored_args.pop("print", None)
        result = await original(stored_args)
        item = (result or {}).get("item") or {}
        source = str(item.get("path", "")).lstrip("/")
        if not source:
            return result
        started = await self.enqueue(source, start_print=True, user=form_args.get("current_user"))
        result = dict(result)
        result["print_started"] = bool(started)
        result.setdefault("print_queued", False)
        return result
