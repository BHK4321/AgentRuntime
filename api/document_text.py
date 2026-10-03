"""Extract bounded, readable text from common uploaded document formats."""
from __future__ import annotations

import io
import re
import zipfile
from html.parser import HTMLParser
from pathlib import PurePosixPath
from xml.etree import ElementTree

MAX_EXTRACTED_BYTES = 10 * 1024 * 1024
MAX_ARCHIVE_ENTRIES = 2000
MAX_ARCHIVE_BYTES = 100 * 1024 * 1024


class DocumentFormatError(ValueError):
    pass


class _HTMLText(HTMLParser):
    def __init__(self):
        super().__init__(convert_charrefs=True)
        self.parts: list[str] = []
        self.skip_depth = 0

    def handle_starttag(self, tag, attrs):
        if tag in {"script", "style", "noscript"}:
            self.skip_depth += 1
        elif tag in {"p", "br", "div", "li", "tr", "h1", "h2", "h3", "h4", "h5", "h6"}:
            self.parts.append("\n")

    def handle_endtag(self, tag):
        if tag in {"script", "style", "noscript"} and self.skip_depth:
            self.skip_depth -= 1
        elif tag in {"p", "div", "li", "tr", "h1", "h2", "h3", "h4", "h5", "h6"}:
            self.parts.append("\n")

    def handle_data(self, data):
        if not self.skip_depth:
            self.parts.append(data)


def _decode_text(data: bytes) -> str:
    for encoding in ("utf-8-sig", "utf-16", "cp1252"):
        try:
            text = data.decode(encoding)
            if "\x00" not in text:
                return text
        except UnicodeDecodeError:
            continue
    raise DocumentFormatError("Text file encoding is unsupported; use UTF-8 or UTF-16.")


def _archive(data: bytes) -> zipfile.ZipFile:
    try:
        archive = zipfile.ZipFile(io.BytesIO(data))
        infos = archive.infolist()
        if len(infos) > MAX_ARCHIVE_ENTRIES or sum(info.file_size for info in infos) > MAX_ARCHIVE_BYTES:
            archive.close()
            raise DocumentFormatError("Office document expands beyond the safe extraction limit.")
        return archive
    except zipfile.BadZipFile as error:
        raise DocumentFormatError("This Office document is damaged or not a valid file.") from error


def _xml(archive: zipfile.ZipFile, name: str):
    try:
        return ElementTree.fromstring(archive.read(name))
    except (KeyError, ElementTree.ParseError) as error:
        raise DocumentFormatError("Document XML is missing or malformed.") from error


def _docx(archive: zipfile.ZipFile) -> str:
    names = [n for n in archive.namelist() if re.fullmatch(r"word/(document|header\d+|footer\d+)\.xml", n)]
    result = []
    for name in names:
        root = _xml(archive, name)
        for paragraph in root.iter("{http://schemas.openxmlformats.org/wordprocessingml/2006/main}p"):
            result.append("".join(node.text or "" for node in paragraph.iter("{http://schemas.openxmlformats.org/wordprocessingml/2006/main}t")))
    return "\n".join(result)


def _xlsx(archive: zipfile.ZipFile) -> str:
    shared = []
    if "xl/sharedStrings.xml" in archive.namelist():
        shared = ["".join(node.text or "" for node in item.iter("{http://schemas.openxmlformats.org/spreadsheetml/2006/main}t"))
                  for item in _xml(archive, "xl/sharedStrings.xml")]
    workbook = _xml(archive, "xl/workbook.xml")
    rels = _xml(archive, "xl/_rels/workbook.xml.rels")
    rel_ns = "{http://schemas.openxmlformats.org/package/2006/relationships}Relationship"
    targets = {rel.attrib["Id"]: rel.attrib["Target"] for rel in rels.iter(rel_ns)}
    ns = "{http://schemas.openxmlformats.org/officeDocument/2006/relationships}id"
    out = []
    for sheet in workbook.iter("{http://schemas.openxmlformats.org/spreadsheetml/2006/main}sheet"):
        target = targets.get(sheet.attrib.get(ns))
        if not target:
            continue
        path = str(PurePosixPath("xl") / target) if not target.startswith("/") else target.lstrip("/")
        if path not in archive.namelist():
            continue
        out.append(f"[{sheet.attrib.get('name', 'Sheet')}]" )
        root = _xml(archive, path)
        for row in root.iter("{http://schemas.openxmlformats.org/spreadsheetml/2006/main}row"):
            values = []
            for cell in row:
                if cell.tag != "{http://schemas.openxmlformats.org/spreadsheetml/2006/main}c":
                    continue
                value = cell.find("{http://schemas.openxmlformats.org/spreadsheetml/2006/main}v")
                inline = cell.find("{http://schemas.openxmlformats.org/spreadsheetml/2006/main}is")
                text = "" if value is None else (value.text or "")
                if cell.attrib.get("t") == "s" and text:
                    try:
                        text = shared[int(text)]
                    except (ValueError, IndexError):
                        pass
                elif inline is not None:
                    text = "".join(n.text or "" for n in inline.iter("{http://schemas.openxmlformats.org/spreadsheetml/2006/main}t"))
                values.append(text)
            out.append("\t".join(values))
    return "\n".join(out)


def _pptx(archive: zipfile.ZipFile) -> str:
    names = sorted((n for n in archive.namelist() if re.fullmatch(r"ppt/slides/slide\d+\.xml", n)),
                   key=lambda n: int(re.search(r"slide(\d+)", n).group(1)))
    out = []
    text_tag = "{http://schemas.openxmlformats.org/drawingml/2006/main}t"
    paragraph_tag = "{http://schemas.openxmlformats.org/drawingml/2006/main}p"
    for name in names:
        out.append("\n".join("".join(n.text or "" for n in p.iter(text_tag))
                             for p in _xml(archive, name).iter(paragraph_tag)))
    return "\n\n".join(out)


def extract_document(filename: str, data: bytes) -> str:
    suffix = PurePosixPath(filename).suffix.lower()
    if suffix in {".txt", ".md", ".csv", ".json", ".xml"}:
        text = _decode_text(data)
        if suffix == ".xml":
            try:
                root = ElementTree.fromstring(text)
                text = "\n".join(part.strip() for part in root.itertext() if part.strip())
            except ElementTree.ParseError as error:
                raise DocumentFormatError("XML document is malformed.") from error
    elif suffix in {".html", ".htm"}:
        parser = _HTMLText()
        parser.feed(_decode_text(data))
        text = "".join(parser.parts)
    elif suffix == ".pdf":
        try:
            from pypdf import PdfReader
        except ImportError as error:
            raise DocumentFormatError("PDF support is not installed on the API service. Run `python -m pip install -r api/requirements.txt` in its virtual environment, then restart the API.") from error
        try:
            reader = PdfReader(io.BytesIO(data), strict=False)
            if reader.is_encrypted:
                raise DocumentFormatError("Password-protected PDFs are not supported.")
            if len(reader.pages) > 500:
                raise DocumentFormatError("PDFs are limited to 500 pages for this service.")
            text = "\n\n".join(page.extract_text() or "" for page in reader.pages)
        except DocumentFormatError:
            raise
        except Exception as error:
            raise DocumentFormatError("Could not read this PDF. It may be damaged or password-protected.") from error
    elif suffix in {".docx", ".xlsx", ".pptx"}:
        with _archive(data) as archive:
            text = {".docx": _docx, ".xlsx": _xlsx, ".pptx": _pptx}[suffix](archive)
    else:
        raise DocumentFormatError("Supported formats: TXT, Markdown, CSV, JSON, XML, HTML, PDF, DOCX, XLSX, and PPTX.")
    if len(text.encode("utf-8")) > MAX_EXTRACTED_BYTES:
        raise DocumentFormatError("Extracted document text exceeds the 10 MiB limit.")
    if not text.strip():
        raise DocumentFormatError("No readable text was found. Scanned PDFs and images need OCR, which is not enabled.")
    return text
